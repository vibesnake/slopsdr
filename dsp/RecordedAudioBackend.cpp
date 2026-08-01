// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordedAudioBackend.hpp"

#include "AudioSampleBuffer.hpp"
#include "FftFrameProcessor.hpp"
#include "ReceiverStateModel.hpp"
#include "SpectrumWindow.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sdr::dsp {
namespace {

constexpr std::size_t readFrames = 480;
constexpr std::size_t audioFrameCapacity = radio::receiverAudioSampleRate / 20;

[[nodiscard]] radio::OperationResult unavailable(const char* control)
{
    return {radio::ReceiverError::BackendFailure, false, false,
            std::string(control) + " is unavailable for recorded audio playback"};
}

[[nodiscard]] bool validConfiguration(const SpectrumDisplayConfiguration& configuration)
{
    return isSupportedSpectrumFftSize(configuration.fftSize) &&
           isSupportedSpectrumFrameRate(configuration.targetFramesPerSecond) &&
           std::isfinite(configuration.minimumDbfs) &&
           std::isfinite(configuration.maximumDbfs) &&
           configuration.maximumDbfs > configuration.minimumDbfs;
}

void fft(std::vector<std::complex<float>>& values)
{
    const std::size_t count = values.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        std::size_t bit = count >> 1U;
        for (; (reversed & bit) != 0; bit >>= 1U) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) std::swap(values[index], values[reversed]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const float angle = -2.0F * std::numbers::pi_v<float> / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t base = 0; base < count; base += length) {
            std::complex<float> twiddle{1.0F, 0.0F};
            for (std::size_t offset = 0; offset < length / 2U; ++offset) {
                const auto even = values[base + offset];
                const auto odd = values[base + offset + length / 2U] * twiddle;
                values[base + offset] = even + odd;
                values[base + offset + length / 2U] = even - odd;
                twiddle *= step;
            }
        }
    }
}

[[nodiscard]] std::uint64_t monotonicNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

class RecordedAudioBackend::Impl final
{
public:
    Impl(std::filesystem::path path, SpectrumDisplayConfiguration requestedConfiguration)
        : source(std::move(path))
        , displaySampleRate(source.metadata().sampleRate / 2U)
        , model({.frequency = {0, displaySampleRate},
                 .allowsPartialPassbandAtFrequencyEdges = true,
                 .sampleRate = {displaySampleRate, displaySampleRate}})
        , audio(std::make_shared<radio::StereoAudioSampleBuffer>(audioFrameCapacity))
        , frames(std::make_shared<radio::SpectrumFrameQueue>(64))
        , counters(std::make_shared<SpectrumProcessingCounters>())
        , configuration(requestedConfiguration)
    {
        if (!validConfiguration(requestedConfiguration)) {
            throw std::invalid_argument("Recorded audio spectrum configuration is invalid");
        }
        static_cast<void>(model.setSampleRate(displaySampleRate));
        static_cast<void>(model.setCenterFrequency(displaySampleRate / 2U));
        static_cast<void>(model.setListeningFrequency(displaySampleRate / 2U));
        createProcessor();
    }

    ~Impl() { stop(); }

    void createProcessor()
    {
        window = makeHannWindow(configuration.fftSize);
        processor = std::make_shared<FftFrameProcessor>(frames, counters,
            configuration.fftSize, coherentGain(window),
            configuration.minimumDbfs, configuration.maximumDbfs);
        visualization.clear();
    }

    void start()
    {
        if (reader.joinable()) reader.join();
        const auto result = source.start();
        if (!result.succeeded) throw std::runtime_error(result.message);
        reachedEnd = false;
        endReported = false;
        endedPosition = 0;
        runtimeError.reset();
        audio->clear();
        frames->clear();
        {
            std::scoped_lock lock(visualizationMutex);
            visualization.clear();
        }
        readerRunning = true;
        resampleInput.clear();
        resamplePosition = 0.0;
        reader = std::thread([this] { readerLoop(); });
    }

    void stop() noexcept
    {
        readerRunning = false;
        static_cast<void>(source.stop());
        if (reader.joinable()) reader.join();
        audio->clear();
        frames->clear();
    }

    void readerLoop() noexcept
    {
        try {
            const auto& metadata = source.metadata();
            const std::size_t sourceReadFrames = std::min(
                readFrames,
                std::max<std::size_t>(std::size_t{1}, metadata.sampleRate / 100U));
            std::vector<float> decoded(sourceReadFrames * metadata.channelCount);
            std::vector<float> stereo;
            while (readerRunning) {
                const auto result = source.read(decoded, std::chrono::milliseconds(50));
                if (result.status == radio::RecordedAudioReadStatus::Timeout) continue;
                if (result.status == radio::RecordedAudioReadStatus::EndOfFile) {
                    reachedEnd = true;
                    return;
                }
                if (result.status == radio::RecordedAudioReadStatus::Stopped) return;
                if (result.status == radio::RecordedAudioReadStatus::Failed) {
                    std::scoped_lock lock(errorMutex);
                    runtimeError = result.message.empty() ? "Recorded audio playback failed" : result.message;
                    return;
                }
                if (result.status != radio::RecordedAudioReadStatus::Frames || result.frameCount == 0) {
                    std::scoped_lock lock(errorMutex);
                    runtimeError = "Recorded audio source returned an invalid frame count";
                    return;
                }
                stereo.resize(result.frameCount * 2U);
                for (std::size_t frame = 0; frame < result.frameCount; ++frame) {
                    const float left = decoded[frame * metadata.channelCount];
                    const float right = metadata.channelCount == 2
                                            ? decoded[frame * 2U + 1U] : left;
                    stereo[frame * 2U] = left;
                    stereo[frame * 2U + 1U] = right;
                    processVisualizationSample((left + right) * 0.5F);
                }
                static_cast<void>(audio->push(resampleForOutput(stereo)));
            }
        } catch (const std::exception& error) {
            std::scoped_lock lock(errorMutex);
            runtimeError = std::string("Recorded audio playback failed: ") + error.what();
        } catch (...) {
            std::scoped_lock lock(errorMutex);
            runtimeError = "Recorded audio playback failed with an unknown error";
        }
    }

    void processVisualizationSample(float sample)
    {
        std::scoped_lock lock(visualizationMutex);
        visualization.push_back(std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F);
        counters->inputSamples.fetch_add(1, std::memory_order_relaxed);
        if (visualization.size() < configuration.fftSize) return;
        std::vector<std::complex<float>> bins(configuration.fftSize);
        for (std::size_t index = 0; index < bins.size(); ++index) {
            bins[index] = {visualization[index] * window[index], 0.0F};
        }
        fft(bins);
        // A real audio signal has useful bins from DC through Nyquist.  Map
        // that one-sided spectrum across the normal display-frame width so
        // the shared spectrum/waterfall viewport is exactly 0..Nyquist,
        // rather than showing a mirrored negative-frequency half.
        std::vector<float> magnitudes(bins.size());
        const double sourceBins = static_cast<double>(bins.size() / 2U);
        const double destinationBins = static_cast<double>(magnitudes.size() - 1U);
        for (std::size_t index = 0; index < magnitudes.size(); ++index) {
            const double position = static_cast<double>(index) * sourceBins / destinationBins;
            const auto lower = static_cast<std::size_t>(position);
            const auto upper = std::min(lower + 1U, bins.size() / 2U);
            const float fraction = static_cast<float>(position - static_cast<double>(lower));
            const float first = std::abs(bins[lower]);
            const float second = std::abs(bins[upper]);
            magnitudes[index] = first + (second - first) * fraction;
        }
        static_cast<void>(processor->submitMagnitudeFrame(magnitudes,
            displaySampleRate / 2U, displaySampleRate, monotonicNanoseconds()));
        counters->vectorsReceived.fetch_add(1, std::memory_order_relaxed);
        const SpectrumWindowHopScheduler hop(source.metadata().sampleRate,
            configuration.fftSize, configuration.targetFramesPerSecond);
        const std::size_t discard = static_cast<std::size_t>(hop.nominalHopSize());
        visualization.erase(visualization.begin(), visualization.begin() +
            static_cast<std::ptrdiff_t>(std::min(discard, visualization.size())));
    }

    [[nodiscard]] std::vector<float> resampleForOutput(
        std::span<const float> sourceFrames)
    {
        if (source.metadata().sampleRate == radio::receiverAudioSampleRate) {
            return {sourceFrames.begin(), sourceFrames.end()};
        }
        resampleInput.insert(
            resampleInput.end(), sourceFrames.begin(), sourceFrames.end());
        const std::size_t inputFrames = resampleInput.size() / 2U;
        std::vector<float> output;
        const double inputPerOutput = static_cast<double>(source.metadata().sampleRate) /
                                      static_cast<double>(radio::receiverAudioSampleRate);
        while (resamplePosition + 1.0 < static_cast<double>(inputFrames)) {
            const auto lower = static_cast<std::size_t>(resamplePosition);
            const float fraction = static_cast<float>(
                resamplePosition - static_cast<double>(lower));
            for (std::size_t channel = 0; channel < 2U; ++channel) {
                const float first = resampleInput[lower * 2U + channel];
                const float second = resampleInput[(lower + 1U) * 2U + channel];
                output.push_back(first + (second - first) * fraction);
            }
            resamplePosition += inputPerOutput;
        }
        const std::size_t consumed = static_cast<std::size_t>(resamplePosition);
        if (consumed > 0) {
            resampleInput.erase(resampleInput.begin(), resampleInput.begin() +
                static_cast<std::ptrdiff_t>(consumed * 2U));
            resamplePosition -= static_cast<double>(consumed);
        }
        return output;
    }

    radio::RecordedAudioSource source;
    const std::uint64_t displaySampleRate;
    radio::ReceiverStateModel model;
    radio::ReceiverCapabilities capabilities;
    std::shared_ptr<radio::StereoAudioSampleBuffer> audio;
    std::shared_ptr<radio::SpectrumFrameQueue> frames;
    std::shared_ptr<SpectrumProcessingCounters> counters;
    SpectrumDisplayConfiguration configuration;
    std::shared_ptr<FftFrameProcessor> processor;
    std::vector<float> window;
    std::vector<float> visualization;
    std::vector<float> resampleInput;
    double resamplePosition = 0.0;
    mutable std::mutex visualizationMutex;
    std::thread reader;
    std::atomic_bool readerRunning = false;
    std::atomic_bool reachedEnd = false;
    std::atomic_bool endReported = false;
    std::atomic<std::uint64_t> endedPosition = 0;
    std::mutex errorMutex;
    std::optional<std::string> runtimeError;
};

RecordedAudioBackend::RecordedAudioBackend(
    std::filesystem::path path, SpectrumDisplayConfiguration configuration)
    : m_impl(std::make_unique<Impl>(std::move(path), configuration)) {}
RecordedAudioBackend::~RecordedAudioBackend() = default;

const radio::ReceiverLimits& RecordedAudioBackend::limits() const noexcept { return m_impl->model.limits(); }
const radio::ReceiverCapabilities& RecordedAudioBackend::capabilities() const noexcept { return m_impl->capabilities; }
radio::ReceiverSourceCapabilities RecordedAudioBackend::sourceCapabilities() const noexcept
{
    return {.kind = radio::ReceiverSourceKind::RecordedAudio,
            .sampleRateChangeSupported = false,
            .rfControlsSupported = false,
            .scannerSupported = false,
            .iqRecordingSupported = false};
}
radio::RecordingTransportState RecordedAudioBackend::recordingTransport() const noexcept
{
    const auto& metadata = m_impl->source.metadata();
    radio::RecordingTransportState state{
        .state = radio::RecordingPlaybackState::Stopped,
        .positionSamples = m_impl->reachedEnd ? m_impl->endedPosition.load()
                                               : m_impl->source.positionFrames(),
        .totalSamples = metadata.frameCount,
        .sampleRate = metadata.sampleRate,
        .displayName = m_impl->source.path().filename().string(),
        .message = {},
    };
    if (m_impl->reachedEnd) state.state = radio::RecordingPlaybackState::Ended;
    else if (m_impl->source.paused()) state.state = radio::RecordingPlaybackState::Paused;
    else if (m_impl->model.state().running) state.state = radio::RecordingPlaybackState::Playing;
    return state;
}
std::optional<std::string> RecordedAudioBackend::takePlaybackEnd()
{
    if (!m_impl->reachedEnd || m_impl->endReported.exchange(true)) return std::nullopt;
    m_impl->endedPosition = m_impl->source.positionFrames();
    static_cast<void>(m_impl->source.stop());
    if (m_impl->model.state().running) static_cast<void>(m_impl->model.stopReception());
    return std::string("Recorded audio playback reached end of file");
}
radio::OperationResult RecordedAudioBackend::setPlaybackPaused(bool paused)
{
    if (!m_impl->model.state().running) return unavailable("Pause");
    m_impl->source.setPaused(paused);
    return {radio::ReceiverError::None, true, false,
            paused ? "Recorded audio playback paused" : "Recorded audio playback resumed"};
}
radio::OperationResult RecordedAudioBackend::restartPlayback()
{
    if (m_impl->model.state().running) static_cast<void>(stopReception());
    return startReception();
}
const radio::ReceiverState& RecordedAudioBackend::state() const noexcept { return m_impl->model.state(); }
std::uint64_t RecordedAudioBackend::effectiveSampleRate() const noexcept
{
    return m_impl->displaySampleRate;
}
std::optional<radio::SpectrumFrame> RecordedAudioBackend::takeLatestSpectrumFrame() { return m_impl->frames->takeLatest(); }
std::vector<radio::SpectrumFrame> RecordedAudioBackend::takePendingSpectrumFrames(std::size_t maximumFrames)
{
    std::vector<radio::SpectrumFrame> result;
    while (result.size() < maximumFrames) { auto frame = m_impl->frames->takeOldest(); if (!frame) break; result.push_back(std::move(*frame)); }
    return result;
}
radio::SpectrumProcessingMetrics RecordedAudioBackend::spectrumProcessingMetrics() const
{
    const auto fftCount = m_impl->counters->fftsExecuted.load(std::memory_order_relaxed);
    const auto sourceRate = m_impl->source.metadata().sampleRate;
    const SpectrumWindowHopScheduler scheduler(sourceRate, m_impl->configuration.fftSize, m_impl->configuration.targetFramesPerSecond);
    return {.inputSamples = m_impl->counters->inputSamples.load(std::memory_order_relaxed),
            .vectorsReceived = m_impl->counters->vectorsReceived.load(std::memory_order_relaxed),
            .fftsExecuted = fftCount,
            .framesPublished = m_impl->counters->framesPublished.load(std::memory_order_relaxed),
            .framesDropped = m_impl->frames->droppedFrameCount(),
            .fftSize = m_impl->configuration.fftSize,
            .queueDepth = m_impl->frames->size(),
            .effectiveSampleRate = static_cast<double>(sourceRate),
            .availableVectorsPerSecond = static_cast<double>(sourceRate) /
                                       static_cast<double>(m_impl->configuration.fftSize),
            .targetFramesPerSecond = static_cast<double>(m_impl->configuration.targetFramesPerSecond),
            .achievableFramesPerSecond = scheduler.achievableFramesPerSecond(),
            .hertzPerBin = static_cast<double>(sourceRate) /
                            static_cast<double>(m_impl->configuration.fftSize),
            .hopSize = scheduler.nominalHopSize(),
            .overlapPercentage = scheduler.overlapPercentage()};
}
std::size_t RecordedAudioBackend::spectrumFftSize() const noexcept { return m_impl->configuration.fftSize; }
std::size_t RecordedAudioBackend::requestedSpectrumFftSize() const noexcept { return spectrumFftSize(); }
radio::OperationResult RecordedAudioBackend::setSpectrumFftSize(std::size_t fftSize)
{
    if (!isSupportedSpectrumFftSize(fftSize)) return {radio::ReceiverError::BackendFailure, false, false, "Unsupported recorded audio spectrum FFT size"};
    if (fftSize == m_impl->configuration.fftSize) return {};
    std::scoped_lock lock(m_impl->visualizationMutex);
    m_impl->configuration.fftSize = fftSize;
    m_impl->createProcessor();
    m_impl->frames->clear();
    return {radio::ReceiverError::None, true, false, "Recorded audio spectrum FFT size updated"};
}
std::uint32_t RecordedAudioBackend::spectrumFramesPerSecond() const noexcept { return m_impl->configuration.targetFramesPerSecond; }
radio::OperationResult RecordedAudioBackend::setSpectrumFramesPerSecond(std::uint32_t frames)
{
    if (!isSupportedSpectrumFrameRate(frames)) return {radio::ReceiverError::BackendFailure, false, false, "Recorded audio spectrum cadence is unsupported"};
    if (frames == m_impl->configuration.targetFramesPerSecond) return {};
    std::scoped_lock lock(m_impl->visualizationMutex);
    m_impl->configuration.targetFramesPerSecond = frames;
    m_impl->visualization.clear();
    return {radio::ReceiverError::None, true, false, "Recorded audio spectrum cadence updated"};
}
std::vector<float> RecordedAudioBackend::takeStereoAudioSamples(std::size_t maximumFrames) { return m_impl->audio->take(maximumFrames); }
void RecordedAudioBackend::clearAudioSamples() { m_impl->audio->clear(); }
std::uint64_t RecordedAudioBackend::audioProducedSamples() const { return m_impl->audio->totalProducedFrames(); }
std::uint64_t RecordedAudioBackend::audioDroppedSamples() const { return m_impl->audio->totalDroppedFrames(); }
std::size_t RecordedAudioBackend::audioBufferedSampleCount() const { return m_impl->audio->size(); }
std::optional<radio::OperationResult> RecordedAudioBackend::takeRuntimeError()
{
    std::scoped_lock lock(m_impl->errorMutex);
    if (!m_impl->runtimeError) return std::nullopt;
    auto message = std::move(m_impl->runtimeError);
    m_impl->runtimeError.reset();
    if (m_impl->model.state().running) static_cast<void>(m_impl->model.stopReception());
    return radio::OperationResult{radio::ReceiverError::BackendFailure, true, false, std::move(*message)};
}
radio::OperationResult RecordedAudioBackend::startReception()
{
    auto result = m_impl->model.startReception();
    if (!result.succeeded() || !result.stateChanged) return result;
    try { m_impl->start(); return {radio::ReceiverError::None, true, false, "Recorded audio playback started"}; }
    catch (const std::exception& error) { static_cast<void>(m_impl->model.stopReception()); return {radio::ReceiverError::BackendFailure, false, false, error.what()}; }
}
radio::OperationResult RecordedAudioBackend::stopReception()
{
    const auto result = m_impl->model.stopReception();
    m_impl->reachedEnd = false;
    m_impl->endedPosition = 0;
    m_impl->stop();
    return result.succeeded() ? radio::OperationResult{radio::ReceiverError::None, result.stateChanged, false, "Recorded audio playback stopped and rewound"} : result;
}
radio::OperationResult RecordedAudioBackend::setCenterFrequency(std::uint64_t) { return unavailable("RF tuning"); }
radio::OperationResult RecordedAudioBackend::setListeningFrequency(std::uint64_t) { return unavailable("RF tuning"); }
radio::OperationResult RecordedAudioBackend::tuneListeningFrequency(double) { return unavailable("RF tuning"); }
radio::OperationResult RecordedAudioBackend::shiftCenterFrequency(std::int64_t) { return unavailable("RF tuning"); }
radio::OperationResult RecordedAudioBackend::setSampleRate(std::uint64_t) { return unavailable("Sample-rate changes"); }
radio::OperationResult RecordedAudioBackend::setFilterWidth(std::uint64_t) { return unavailable("Filter controls"); }
radio::OperationResult RecordedAudioBackend::setGain(double) { return unavailable("Gain controls"); }
radio::OperationResult RecordedAudioBackend::setPpmCorrection(double) { return unavailable("PPM correction"); }
radio::OperationResult RecordedAudioBackend::setDemodulationMode(radio::DemodulationMode) { return unavailable("Demodulation controls"); }
radio::OperationResult RecordedAudioBackend::setSquelchLevel(double) { return unavailable("Squelch controls"); }
radio::OperationResult RecordedAudioBackend::enableManualSquelch() { return unavailable("Squelch controls"); }
radio::OperationResult RecordedAudioBackend::disableSquelch() { return unavailable("Squelch controls"); }

}  // namespace sdr::dsp
