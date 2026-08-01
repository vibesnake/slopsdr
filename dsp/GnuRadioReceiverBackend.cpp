// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "GnuRadioReceiverBackend.hpp"

#include "AudioDspPlan.hpp"
#include "AudioSampleBuffer.hpp"
#include "FftFrameProcessor.hpp"
#include "FlowgraphLifecycle.hpp"
#include "ReceiverStateModel.hpp"
#include "SpectrumFramePacing.hpp"
#include "SpectrumWindow.hpp"
#include "DeviceController.hpp"
#include "WidebandIqSources.hpp"

#include <gnuradio/analog/agc3_cc.h>
#include <gnuradio/analog/pwr_squelch_cc.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/blocks/complex_to_mag.h>
#include <gnuradio/blocks/complex_to_real.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/selector.h>
#include <gnuradio/fft/fft_v.h>
#include <gnuradio/filter/firdes.h>
#include <gnuradio/filter/dc_blocker_ff.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/filter/rational_resampler.h>
#include <gnuradio/filter/single_pole_iir_filter_ff.h>
#include <gnuradio/gr_complex.h>
#include <gnuradio/io_signature.h>
#include <gnuradio/block.h>
#include <gnuradio/sync_block.h>
#include <gnuradio/top_block.h>

#include <pmt/pmt.h>

#include <boost/thread/interruption.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sdr::dsp {
namespace {

constexpr double disabledSquelchThresholdDb = -160.0;
constexpr double narrowFmDeemphasisSeconds = 300e-6;
constexpr double wideFmDeemphasisSeconds = 75e-6;
constexpr float narrowModeAgcAttackRate = 0.01F;
constexpr float narrowModeAgcDecayRate = 0.00001F;
constexpr float narrowModeAgcReference = 0.35F;
constexpr float narrowModeAgcMaximumGain = 50.0F;
constexpr std::size_t channelAudioBufferCapacity =
    radio::receiverAudioSampleRate * 3 / 50;
SpectrumDisplayConfiguration validateSpectrumConfiguration(
    SpectrumDisplayConfiguration configuration)
{
    if (!isSupportedSpectrumFftSize(configuration.fftSize)) {
        throw std::invalid_argument(
            "Spectrum FFT size must be one of 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, or 262144");
    }
    if (!isSupportedSpectrumFrameRate(configuration.targetFramesPerSecond)) {
        throw std::invalid_argument(
            "Internal spectrum source cadence must be from 1 through 240 frames/s");
    }
    if (!std::isfinite(configuration.minimumDbfs) ||
        !std::isfinite(configuration.maximumDbfs) ||
        configuration.maximumDbfs <= configuration.minimumDbfs) {
        throw std::invalid_argument("Spectrum dBFS display range is invalid");
    }
    return configuration;
}

std::shared_ptr<FftFrameProcessor> makeSpectrumProcessor(
    std::shared_ptr<radio::SpectrumFrameQueue> outputQueue,
    std::shared_ptr<SpectrumProcessingCounters> counters,
    const SpectrumDisplayConfiguration& configuration,
    float windowCoherentGain)
{
    const auto validated = validateSpectrumConfiguration(configuration);
    return std::make_shared<FftFrameProcessor>(
        std::move(outputQueue),
        std::move(counters),
        validated.fftSize,
        windowCoherentGain,
        validated.minimumDbfs,
        validated.maximumDbfs);
}

struct PreparedSpectrumResources {
    std::unordered_map<
        std::size_t,
        gr::fft::fft_v<gr_complex, true>::sptr> fftPlans;
    std::unordered_map<
        std::size_t,
        gr::blocks::complex_to_mag::sptr> magnitudeBuffers;
    std::unordered_map<std::size_t, float> windowCoherentGains;

    PreparedSpectrumResources()
    {
        std::string lastError;
        for (const std::size_t fftSize : supportedSpectrumFftSizes) {
            try {
                const auto window = makeHannWindow(fftSize);
                auto fft = gr::fft::fft_v<gr_complex, true>::make(
                    static_cast<int>(fftSize), window, true);
                auto magnitude =
                    gr::blocks::complex_to_mag::make(fftSize);
                fftPlans.emplace(fftSize, std::move(fft));
                magnitudeBuffers.emplace(fftSize, std::move(magnitude));
                windowCoherentGains.emplace(
                    fftSize, coherentGain(window));
            } catch (const std::exception& error) {
                fftPlans.erase(fftSize);
                magnitudeBuffers.erase(fftSize);
                windowCoherentGains.erase(fftSize);
                lastError = error.what();
            }
        }
        if (fftPlans.empty()) {
            throw std::runtime_error(
                lastError.empty()
                    ? "GNU Radio could not allocate any supported spectrum FFT plan"
                    : "GNU Radio could not allocate a spectrum FFT plan: " +
                          lastError);
        }
    }

    [[nodiscard]] float windowCoherentGain(std::size_t fftSize) const
    {
        return windowCoherentGains.at(fftSize);
    }

    [[nodiscard]] std::size_t effectiveFftSize(
        std::size_t requestedFftSize) const
    {
        if (fftPlans.contains(requestedFftSize)) {
            return requestedFftSize;
        }
        for (auto size = supportedSpectrumFftSizes.rbegin();
             size != supportedSpectrumFftSizes.rend(); ++size) {
            if (*size < requestedFftSize && fftPlans.contains(*size)) {
                return *size;
            }
        }
        for (const std::size_t size : supportedSpectrumFftSizes) {
            if (fftPlans.contains(size)) {
                return size;
            }
        }
        throw std::runtime_error(
            "GNU Radio has no usable spectrum FFT plan");
    }
};

struct InitialSpectrumConfiguration {
    SpectrumDisplayConfiguration requested;
    SpectrumDisplayConfiguration effective;
    PreparedSpectrumResources resources;

    explicit InitialSpectrumConfiguration(
        SpectrumDisplayConfiguration configuration)
        : requested(validateSpectrumConfiguration(configuration))
        , effective(requested)
    {
        effective.fftSize = resources.effectiveFftSize(requested.fftSize);
    }
};

double translationOffsetHz(const radio::ReceiverState& state) noexcept
{
    return static_cast<double>(state.listeningFrequency) -
           static_cast<double>(state.centerFrequency);
}

double squelchThresholdDb(const radio::ReceiverState& state) noexcept
{
    return state.squelchMode == radio::SquelchMode::Disabled
               ? disabledSquelchThresholdDb
               : state.squelchLevelDb;
}

ChannelRatePlan stableChannelRatePlan(
    std::uint64_t effectiveSampleRate,
    radio::DemodulationMode mode)
{
    const auto filterRange =
        radio::filterWidthRange(mode, effectiveSampleRate);
    return makeChannelRatePlan(
        effectiveSampleRate, mode, filterRange.maximum);
}

std::vector<gr_complex> channelFilterTaps(
    const radio::ReceiverState& state, std::uint64_t effectiveSampleRate)
{
    const auto ratePlan = makeChannelRatePlan(
        effectiveSampleRate, state.demodulationMode, state.filterWidth);
    const double sampleRate = static_cast<double>(effectiveSampleRate);
    const double width = static_cast<double>(state.filterWidth);
    double lowerCutoff = -width / 2.0;
    double upperCutoff = width / 2.0;
    if (state.demodulationMode == radio::DemodulationMode::Usb ||
        state.demodulationMode == radio::DemodulationMode::Lsb) {
        lowerCutoff = -width;
        upperCutoff = width;
    }
    return gr::filter::firdes::complex_band_pass(
        1.0,
        sampleRate,
        lowerCutoff,
        upperCutoff,
        ratePlan.transitionWidthHz);
}

std::vector<gr_complex> sidebandChannelTaps(
    const radio::ReceiverState& state,
    radio::DemodulationMode sidebandMode,
    std::uint32_t channelSampleRate)
{
    if (state.demodulationMode != sidebandMode) {
        return {gr_complex{1.0F, 0.0F}};
    }
    const double width = static_cast<double>(state.filterWidth);
    const bool upper = sidebandMode == radio::DemodulationMode::Usb;
    return gr::filter::firdes::complex_band_pass(
        1.0,
        static_cast<double>(channelSampleRate),
        upper ? 300.0 : -width,
        upper ? width : -300.0,
        250.0);
}

float fmDemodulationGain(
    std::uint64_t effectiveSampleRate, double deviationHz) noexcept
{
    return static_cast<float>(
        static_cast<double>(effectiveSampleRate) /
        (2.0 * 3.141592653589793 * deviationHz));
}

gr::filter::rational_resampler_fff::sptr makeAudioResampler(
    std::uint64_t channelSampleRate)
{
    const auto conversion = makeAudioRateConversion(channelSampleRate);
    return gr::filter::rational_resampler_fff::make(
        conversion.interpolation,
        conversion.decimation,
        {},
        0.4F);
}

std::vector<float> sidebandAudioTaps(const radio::ReceiverState& state)
{
    const double upperCutoff = std::min(
        static_cast<double>(state.filterWidth), 4'000.0);
    return gr::filter::firdes::band_pass(
        1.0,
        radio::receiverAudioSampleRate,
        300.0,
        upperCutoff,
        250.0);
}

radio::OperationResult backendFailure(const std::exception& error)
{
    return {
        radio::ReceiverError::BackendFailure,
        false,
        false,
        std::string("GNU Radio backend failure: ") + error.what(),
    };
}

radio::OperationResult unknownBackendFailure()
{
    return {
        radio::ReceiverError::BackendFailure,
        false,
        false,
        "GNU Radio backend failed with an unknown error",
    };
}

radio::OperationResult deviceFailure(devices::DeviceOperationResult result)
{
    using devices::DeviceError;
    radio::ReceiverError error = radio::ReceiverError::BackendFailure;
    switch (result.error) {
    case DeviceError::FrequencyUnsupported:
        error = radio::ReceiverError::CenterFrequencyOutOfRange;
        break;
    case DeviceError::SampleRateUnsupported:
        error = radio::ReceiverError::SampleRateOutOfRange;
        break;
    case DeviceError::PpmCorrectionUnsupported:
        error = radio::ReceiverError::PpmCorrectionUnsupported;
        break;
    case DeviceError::PpmCorrectionFailed:
        error = radio::ReceiverError::BackendFailure;
        break;
    case DeviceError::PpmCalibrationUnsupported:
        error = radio::ReceiverError::PpmCorrectionUnsupported;
        break;
    case DeviceError::PpmCalibrationFailed:
        error = radio::ReceiverError::BackendFailure;
        break;
    default:
        break;
    }
    return {error, false, false, std::move(result.message)};
}

class RuntimeFailureState final
{
public:
    void set(std::string message)
    {
        std::scoped_lock lock(m_mutex);
        if (!m_message.has_value()) {
            m_message = std::move(message);
        }
    }

    [[nodiscard]] std::optional<std::string> take()
    {
        std::scoped_lock lock(m_mutex);
        auto message = std::move(m_message);
        m_message.reset();
        return message;
    }

    void clear()
    {
        std::scoped_lock lock(m_mutex);
        m_message.reset();
    }

private:
    std::mutex m_mutex;
    std::optional<std::string> m_message;
};

struct CaptureMetadataSnapshot {
    std::uint64_t centerFrequency = 0;
    std::uint64_t tuningGeneration = 0;

    friend bool operator==(
        const CaptureMetadataSnapshot&,
        const CaptureMetadataSnapshot&) = default;
};

class CaptureMetadataState final
{
public:
    CaptureMetadataState(
        std::uint64_t centerFrequency,
        std::uint64_t tuningGeneration)
        : m_snapshot{centerFrequency, tuningGeneration}
    {
    }

    [[nodiscard]] CaptureMetadataSnapshot snapshot() const
    {
        std::scoped_lock lock(m_mutex);
        return m_snapshot;
    }

    void update(
        std::uint64_t centerFrequency,
        std::uint64_t tuningGeneration)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot = {centerFrequency, tuningGeneration};
    }

private:
    mutable std::mutex m_mutex;
    CaptureMetadataSnapshot m_snapshot;
};

const pmt::pmt_t& captureMetadataTagKey()
{
    static const pmt::pmt_t key = pmt::intern("sdr_capture_metadata");
    return key;
}

const pmt::pmt_t& captureTimestampTagKey()
{
    static const pmt::pmt_t key = pmt::intern("sdr_capture_timestamp_ns");
    return key;
}

std::uint64_t steadyClockNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t sampleOffsetNanoseconds(
    std::uint64_t sampleOffset, std::uint64_t sampleRate) noexcept
{
    return static_cast<std::uint64_t>(std::llround(
        static_cast<long double>(sampleOffset) * 1'000'000'000.0L /
        static_cast<long double>(sampleRate)));
}

pmt::pmt_t captureMetadataTagValue(CaptureMetadataSnapshot metadata)
{
    return pmt::cons(
        pmt::from_uint64(metadata.centerFrequency),
        pmt::from_uint64(metadata.tuningGeneration));
}

std::optional<CaptureMetadataSnapshot> captureMetadataFromTag(
    const gr::tag_t& tag)
{
    if (!pmt::is_pair(tag.value) ||
        !pmt::is_uint64(pmt::car(tag.value)) ||
        !pmt::is_uint64(pmt::cdr(tag.value))) {
        return std::nullopt;
    }
    return CaptureMetadataSnapshot{
        pmt::to_uint64(pmt::car(tag.value)),
        pmt::to_uint64(pmt::cdr(tag.value)),
    };
}

class WidebandIqSourceBlock final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<WidebandIqSourceBlock>;

    static sptr make(
        std::shared_ptr<radio::WidebandIqSource> source,
        std::shared_ptr<RuntimeFailureState> failure,
        std::shared_ptr<CaptureMetadataState> captureMetadata)
    {
        return gnuradio::make_block_sptr<WidebandIqSourceBlock>(
            std::move(source),
            std::move(failure),
            std::move(captureMetadata));
    }

    WidebandIqSourceBlock(
        std::shared_ptr<radio::WidebandIqSource> source,
        std::shared_ptr<RuntimeFailureState> failure,
        std::shared_ptr<CaptureMetadataState> captureMetadata)
        : gr::sync_block(
              "wideband_iq_source",
              gr::io_signature::make(0, 0, 0),
              gr::io_signature::make(1, 1, sizeof(gr_complex)))
        , m_source(std::move(source))
        , m_failure(std::move(failure))
        , m_captureMetadata(std::move(captureMetadata))
    {
    }

    bool start() override
    {
        m_running.store(true, std::memory_order_release);
        return true;
    }

    bool stop() override
    {
        m_running.store(false, std::memory_order_release);
        return true;
    }

    int work(
        int itemCount,
        gr_vector_const_void_star&,
        gr_vector_void_star& outputItems) override
    {
        if (itemCount <= 0 || outputItems.empty() || !outputItems.front()) {
            m_failure->set(
                "GNU Radio supplied an invalid output buffer to the IQ source");
            return WORK_DONE;
        }

        auto* output = static_cast<gr_complex*>(outputItems.front());
        const std::size_t outputCapacity = static_cast<std::size_t>(itemCount);
        while (m_running.load(std::memory_order_acquire)) {
            const auto captureMetadata = m_captureMetadata->snapshot();
            const auto result = m_source->read(
                std::span<std::complex<float>>(output, outputCapacity),
                std::chrono::milliseconds(50));
            switch (result.status) {
            case radio::WidebandIqReadStatus::Samples:
                if (result.sampleCount == 0 ||
                    result.sampleCount > outputCapacity) {
                    m_failure->set("Wideband IQ source returned an invalid sample count");
                    return WORK_DONE;
                }
                add_item_tag(
                    0,
                    nitems_written(0),
                    captureMetadataTagKey(),
                    captureMetadataTagValue(captureMetadata));
                return static_cast<int>(result.sampleCount);
            case radio::WidebandIqReadStatus::Timeout:
                boost::this_thread::interruption_point();
                continue;
            case radio::WidebandIqReadStatus::EndOfFile:
                m_failure->set(result.message.empty()
                    ? "Recorded IQ playback reached end of file" : result.message);
                return WORK_DONE;
            case radio::WidebandIqReadStatus::Stopped:
            case radio::WidebandIqReadStatus::Disconnected:
            case radio::WidebandIqReadStatus::Failed:
                m_failure->set(
                    result.message.empty()
                        ? "Wideband IQ source stopped streaming"
                        : result.message);
                return WORK_DONE;
            }
            m_failure->set("Wideband IQ source returned an unknown stream state");
            return WORK_DONE;
        }

        return WORK_DONE;
    }

private:
    std::shared_ptr<radio::WidebandIqSource> m_source;
    std::shared_ptr<RuntimeFailureState> m_failure;
    std::shared_ptr<CaptureMetadataState> m_captureMetadata;
    std::atomic_bool m_running = false;
};

class AudioSampleSink final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<AudioSampleSink>;

    static sptr make(
        std::shared_ptr<radio::AudioSampleBuffer> samples,
        bool boundSamples = false)
    {
        return gnuradio::make_block_sptr<AudioSampleSink>(
            std::move(samples), boundSamples);
    }

    explicit AudioSampleSink(
        std::shared_ptr<radio::AudioSampleBuffer> samples,
        bool boundSamples)
        : gr::sync_block(
              "bounded_audio_sample_sink",
              gr::io_signature::make(1, 1, sizeof(float)),
              gr::io_signature::make(0, 0, 0))
        , m_samples(std::move(samples))
        , m_boundSamples(boundSamples)
    {
    }

    int work(
        int itemCount,
        gr_vector_const_void_star& inputItems,
        gr_vector_void_star&) override
    {
        const auto* input = static_cast<const float*>(inputItems.front());
        if (m_boundSamples) {
            m_boundedSamples.resize(static_cast<std::size_t>(itemCount));
            std::ranges::transform(
                std::span<const float>(
                    input, static_cast<std::size_t>(itemCount)),
                m_boundedSamples.begin(),
                [](float sample) {
                    return std::isfinite(sample)
                               ? std::clamp(sample, -1.0F, 1.0F)
                               : 0.0F;
                });
            static_cast<void>(m_samples->push(m_boundedSamples));
        } else {
            static_cast<void>(m_samples->push(
                std::span<const float>(
                    input, static_cast<std::size_t>(itemCount))));
        }
        return itemCount;
    }

private:
    std::shared_ptr<radio::AudioSampleBuffer> m_samples;
    bool m_boundSamples = false;
    std::vector<float> m_boundedSamples;
};

class ComplexSampleSink final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<ComplexSampleSink>;

    static sptr make(std::shared_ptr<radio::ComplexSampleBuffer> samples)
    {
        return gnuradio::make_block_sptr<ComplexSampleSink>(std::move(samples));
    }

    explicit ComplexSampleSink(std::shared_ptr<radio::ComplexSampleBuffer> samples)
        : gr::sync_block("bounded_full_bandwidth_iq_sink",
              gr::io_signature::make(1, 1, sizeof(gr_complex)),
              gr::io_signature::make(0, 0, 0))
        , m_samples(std::move(samples))
    {
    }

    int work(int itemCount, gr_vector_const_void_star& inputItems,
        gr_vector_void_star&) override
    {
        const auto* input = static_cast<const gr_complex*>(inputItems.front());
        m_samples->push(std::span<const std::complex<float>>(
            reinterpret_cast<const std::complex<float>*>(input),
            static_cast<std::size_t>(itemCount)));
        return itemCount;
    }

private:
    std::shared_ptr<radio::ComplexSampleBuffer> m_samples;
};

class RmsDiagnosticSink final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<RmsDiagnosticSink>;

    static sptr make(
        std::string stage,
        std::size_t itemSize,
        std::uint32_t sampleRate,
        bool complexSamples)
    {
        return gnuradio::make_block_sptr<RmsDiagnosticSink>(
            std::move(stage), itemSize, sampleRate, complexSamples);
    }

    RmsDiagnosticSink(
        std::string stage,
        std::size_t itemSize,
        std::uint32_t sampleRate,
        bool complexSamples)
        : gr::sync_block(
              "rms_diagnostic_" + stage,
              gr::io_signature::make(1, 1, static_cast<int>(itemSize)),
              gr::io_signature::make(0, 0, 0))
        , m_stage(std::move(stage))
        , m_sampleRate(sampleRate)
        , m_complexSamples(complexSamples)
        , m_lastReport(std::chrono::steady_clock::now())
    {
        if (sampleRate == 0 ||
            (complexSamples && itemSize != sizeof(gr_complex)) ||
            (!complexSamples && itemSize != sizeof(float))) {
            throw std::invalid_argument("RMS diagnostic configuration is invalid");
        }
    }

    int work(
        int itemCount,
        gr_vector_const_void_star& inputItems,
        gr_vector_void_star&) override
    {
        if (m_complexSamples) {
            const auto* input = static_cast<const gr_complex*>(inputItems.front());
            for (int index = 0; index < itemCount; ++index) {
                const double power = std::norm(input[index]);
                m_sumSquares += power;
                m_peak = std::max(m_peak, std::sqrt(power));
            }
        } else {
            const auto* input = static_cast<const float*>(inputItems.front());
            for (int index = 0; index < itemCount; ++index) {
                const double sample = static_cast<double>(input[index]);
                m_sumSquares += sample * sample;
                m_peak = std::max(m_peak, std::abs(sample));
            }
        }
        m_samples += static_cast<std::uint64_t>(itemCount);
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastReport >= std::chrono::seconds(1)) {
            const double rms = std::sqrt(
                m_sumSquares / static_cast<double>(m_samples));
            std::osyncstream(std::clog)
                << "DSP RMS: stage=" << m_stage << " rate=" << m_sampleRate
                << " sps rms=" << rms << " peak=" << m_peak << '\n';
            m_samples = 0;
            m_sumSquares = 0.0;
            m_peak = 0.0;
            m_lastReport = now;
        }
        return itemCount;
    }

private:
    const std::string m_stage;
    const std::uint32_t m_sampleRate;
    const bool m_complexSamples;
    std::chrono::steady_clock::time_point m_lastReport;
    std::uint64_t m_samples = 0;
    double m_sumSquares = 0.0;
    double m_peak = 0.0;
};

class SquelchSignalStrengthState final
{
public:
    void publish(double signalStrengthDb) noexcept
    {
        m_latestDb.store(signalStrengthDb, std::memory_order_relaxed);
    }

    [[nodiscard]] std::optional<double> latest() const noexcept
    {
        const double value = m_latestDb.load(std::memory_order_relaxed);
        return std::isfinite(value) ? std::optional<double>(value)
                                    : std::nullopt;
    }

private:
    std::atomic<double> m_latestDb{-std::numeric_limits<double>::infinity()};
};

class SquelchSignalStrengthSink final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<SquelchSignalStrengthSink>;

    static sptr make(std::shared_ptr<SquelchSignalStrengthState> state)
    {
        return gnuradio::make_block_sptr<SquelchSignalStrengthSink>(
            std::move(state));
    }

    explicit SquelchSignalStrengthSink(
        std::shared_ptr<SquelchSignalStrengthState> state)
        : gr::sync_block(
              "squelch_signal_strength",
              gr::io_signature::make(1, 1, sizeof(gr_complex)),
              gr::io_signature::make(0, 0, 0))
        , m_state(std::move(state))
    {
    }

    int work(
        int itemCount,
        gr_vector_const_void_star& inputItems,
        gr_vector_void_star&) override
    {
        const auto* input = static_cast<const gr_complex*>(inputItems.front());
        double sumPower = 0.0;
        for (int index = 0; index < itemCount; ++index) {
            sumPower += std::norm(input[index]);
        }
        if (itemCount > 0 && std::isfinite(sumPower) && sumPower > 0.0) {
            const double meanPower = sumPower / static_cast<double>(itemCount);
            const double signalStrengthDb = 10.0 * std::log10(meanPower);
            if (std::isfinite(signalStrengthDb)) {
                m_state->publish(signalStrengthDb);
            }
        }
        return itemCount;
    }

private:
    std::shared_ptr<SquelchSignalStrengthState> m_state;
};

class SpectrumWindowGenerator final : public gr::block
{
public:
    using sptr = std::shared_ptr<SpectrumWindowGenerator>;

    static sptr make(
        std::size_t vectorSize,
        std::uint64_t effectiveSampleRate,
        std::uint32_t targetFramesPerSecond,
        std::shared_ptr<SpectrumProcessingCounters> counters,
        CaptureMetadataSnapshot initialCaptureMetadata)
    {
        return gnuradio::make_block_sptr<SpectrumWindowGenerator>(
            vectorSize,
            effectiveSampleRate,
            targetFramesPerSecond,
            std::move(counters),
            initialCaptureMetadata);
    }

    SpectrumWindowGenerator(
        std::size_t vectorSize,
        std::uint64_t effectiveSampleRate,
        std::uint32_t targetFramesPerSecond,
        std::shared_ptr<SpectrumProcessingCounters> counters,
        CaptureMetadataSnapshot initialCaptureMetadata)
        : gr::block(
              "spectrum_window_generator",
              gr::io_signature::make(1, 1, sizeof(gr_complex)),
              gr::io_signature::make(1, 1, itemSize(vectorSize)))
        , m_vectorSize(vectorSize)
        , m_vectorBytes(vectorSize * sizeof(gr_complex))
        , m_effectiveSampleRate(effectiveSampleRate)
        , m_scheduler(
              effectiveSampleRate, vectorSize, targetFramesPerSecond)
        , m_counters(std::move(counters))
        , m_captureMetadata(initialCaptureMetadata)
    {
        if (!m_counters) {
            throw std::invalid_argument(
                "Spectrum vector selector requires counters");
        }
        set_relative_rate(
            m_scheduler.achievableFramesPerSecond() /
            static_cast<double>(effectiveSampleRate));
        set_tag_propagation_policy(gr::block::TPP_DONT);
        m_buffer.reserve(vectorSize);
    }

    void forecast(
        int outputItemCount, gr_vector_int& inputItemsRequired) override
    {
        static_cast<void>(outputItemCount);
        // Windows are accumulated in m_buffer so large FFTs do not require
        // the scheduler to provide an entire window in one input span.
        std::ranges::fill(inputItemsRequired, 1);
    }

    int general_work(
        int outputItemCount,
        gr_vector_int& inputItemCounts,
        gr_vector_const_void_star& inputItems,
        gr_vector_void_star& outputItems) override
    {
        const auto* input = static_cast<const gr_complex*>(inputItems.front());
        auto* output = static_cast<gr_complex*>(outputItems.front());
        const int availableItems = inputItemCounts.front();
        std::vector<gr::tag_t> metadataTags;
        get_tags_in_range(
            metadataTags,
            0,
            nitems_read(0),
            nitems_read(0) + static_cast<std::uint64_t>(availableItems),
            captureMetadataTagKey());
        std::size_t nextMetadataTag = 0;
        int consumed = 0;
        int produced = 0;
        while (consumed < availableItems) {
            const std::uint64_t inputOffset =
                nitems_read(0) + static_cast<std::uint64_t>(consumed);
            while (nextMetadataTag < metadataTags.size() &&
                   metadataTags[nextMetadataTag].offset <= inputOffset) {
                if (const auto metadata = captureMetadataFromTag(
                        metadataTags[nextMetadataTag]);
                    metadata.has_value() && *metadata != m_captureMetadata) {
                    m_captureMetadata = *metadata;
                    m_buffer.clear();
                    m_bufferStart = inputOffset;
                    m_nextWindowStart = inputOffset;
                    m_scheduler.reset();
                    m_timestampOriginNanoseconds = 0;
                }
                ++nextMetadataTag;
            }

            if (inputOffset < m_nextWindowStart) {
                const std::uint64_t skip = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(availableItems - consumed),
                    m_nextWindowStart - inputOffset);
                consumed += static_cast<int>(skip);
                continue;
            }

            if (m_buffer.empty()) {
                m_bufferStart = inputOffset;
            }
            const std::uint64_t requiredEnd = m_nextWindowStart + m_vectorSize;
            const std::uint64_t bufferEnd =
                m_bufferStart + static_cast<std::uint64_t>(m_buffer.size());
            if (bufferEnd < requiredEnd) {
                std::uint64_t appendCount = std::min<std::uint64_t>(
                    requiredEnd - bufferEnd,
                    static_cast<std::uint64_t>(availableItems - consumed));
                if (nextMetadataTag < metadataTags.size()) {
                    appendCount = std::min(
                        appendCount,
                        metadataTags[nextMetadataTag].offset - inputOffset);
                }
                if (appendCount == 0) {
                    continue;
                }
                m_buffer.insert(
                    m_buffer.end(), input + consumed, input + consumed + appendCount);
                consumed += static_cast<int>(appendCount);
            }

            const std::uint64_t updatedBufferEnd =
                m_bufferStart + static_cast<std::uint64_t>(m_buffer.size());
            if (updatedBufferEnd < requiredEnd) {
                continue;
            }
            if (produced >= outputItemCount) {
                break;
            }

            const std::size_t windowOffset = static_cast<std::size_t>(
                m_nextWindowStart - m_bufferStart);
            std::memcpy(
                output + static_cast<std::size_t>(produced) * m_vectorSize,
                m_buffer.data() + windowOffset,
                m_vectorBytes);
            add_item_tag(
                0,
                nitems_written(0) + static_cast<std::uint64_t>(produced),
                captureMetadataTagKey(),
                captureMetadataTagValue(m_captureMetadata));
            if (m_timestampOriginNanoseconds == 0) {
                const auto windowEndNanoseconds = sampleOffsetNanoseconds(
                    m_nextWindowStart + m_vectorSize, m_effectiveSampleRate);
                const auto now = steadyClockNanoseconds();
                m_timestampOriginNanoseconds =
                    now > windowEndNanoseconds ? now - windowEndNanoseconds : 1;
            }
            add_item_tag(
                0,
                nitems_written(0) + static_cast<std::uint64_t>(produced),
                captureTimestampTagKey(),
                pmt::from_uint64(
                    m_timestampOriginNanoseconds + sampleOffsetNanoseconds(
                        m_nextWindowStart + m_vectorSize / 2,
                        m_effectiveSampleRate)));
            ++produced;
            m_counters->vectorsReceived.fetch_add(1, std::memory_order_relaxed);
            m_nextWindowStart += m_scheduler.nextHopSize();

            const std::uint64_t discardBefore = std::min(
                m_nextWindowStart, updatedBufferEnd);
            const std::size_t discardCount = static_cast<std::size_t>(
                discardBefore - m_bufferStart);
            m_buffer.erase(
                m_buffer.begin(),
                m_buffer.begin() + static_cast<std::ptrdiff_t>(discardCount));
            m_bufferStart = discardBefore;
        }
        m_counters->inputSamples.fetch_add(
            static_cast<std::uint64_t>(consumed), std::memory_order_relaxed);
        consume_each(consumed);
        return produced;
    }

private:
    [[nodiscard]] static int itemSize(std::size_t vectorSize)
    {
        const std::size_t bytes = vectorSize * sizeof(gr_complex);
        if (bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Spectrum vector item size is too large");
        }
        return static_cast<int>(bytes);
    }

    const std::size_t m_vectorSize;
    const std::size_t m_vectorBytes;
    const std::uint64_t m_effectiveSampleRate;
    SpectrumWindowHopScheduler m_scheduler;
    std::shared_ptr<SpectrumProcessingCounters> m_counters;
    CaptureMetadataSnapshot m_captureMetadata;
    std::vector<gr_complex> m_buffer;
    std::uint64_t m_bufferStart = 0;
    std::uint64_t m_nextWindowStart = 0;
    std::uint64_t m_timestampOriginNanoseconds = 0;
};

class DisplayMagnitudeSink final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<DisplayMagnitudeSink>;

    static sptr make(
        std::shared_ptr<FftFrameProcessor> processor,
        CaptureMetadataSnapshot initialCaptureMetadata,
        std::uint64_t sampleRate,
        std::size_t fftSize)
    {
        return gnuradio::make_block_sptr<DisplayMagnitudeSink>(
            std::move(processor),
            initialCaptureMetadata,
            sampleRate,
            fftSize);
    }

    DisplayMagnitudeSink(
        std::shared_ptr<FftFrameProcessor> processor,
        CaptureMetadataSnapshot initialCaptureMetadata,
        std::uint64_t sampleRate,
        std::size_t fftSize)
        : gr::sync_block(
              "display_magnitude_sink",
              gr::io_signature::make(
                  1, 1, itemSize(fftSize)),
              gr::io_signature::make(0, 0, 0))
        , m_processor(std::move(processor))
        , m_captureMetadata(initialCaptureMetadata)
        , m_sampleRate(sampleRate)
        , m_fftSize(fftSize)
    {
    }

    int work(
        int itemCount,
        gr_vector_const_void_star& inputItems,
        gr_vector_void_star&) override
    {
        const auto* magnitudes = static_cast<const float*>(inputItems.front());
        std::vector<gr::tag_t> metadataTags;
        std::vector<gr::tag_t> timestampTags;
        get_tags_in_range(
            metadataTags,
            0,
            nitems_read(0),
            nitems_read(0) + static_cast<std::uint64_t>(itemCount),
            captureMetadataTagKey());
        get_tags_in_range(
            timestampTags,
            0,
            nitems_read(0),
            nitems_read(0) + static_cast<std::uint64_t>(itemCount),
            captureTimestampTagKey());
        std::size_t nextMetadataTag = 0;
        std::size_t nextTimestampTag = 0;
        std::uint64_t timestampNanoseconds = 0;
        for (int item = 0; item < itemCount; ++item) {
            const std::uint64_t inputOffset =
                nitems_read(0) + static_cast<std::uint64_t>(item);
            while (nextMetadataTag < metadataTags.size() &&
                   metadataTags[nextMetadataTag].offset <= inputOffset) {
                if (const auto metadata = captureMetadataFromTag(
                        metadataTags[nextMetadataTag])) {
                    m_captureMetadata = *metadata;
                }
                ++nextMetadataTag;
            }
            while (nextTimestampTag < timestampTags.size() &&
                   timestampTags[nextTimestampTag].offset <= inputOffset) {
                if (pmt::is_uint64(timestampTags[nextTimestampTag].value)) {
                    timestampNanoseconds = pmt::to_uint64(
                        timestampTags[nextTimestampTag].value);
                }
                ++nextTimestampTag;
            }
            const std::span<const float> frame(
                magnitudes + static_cast<std::size_t>(item) *
                                 m_fftSize,
                m_fftSize);
            static_cast<void>(m_processor->submitMagnitudeFrame(
                frame,
                m_captureMetadata.centerFrequency,
                m_sampleRate,
                timestampNanoseconds == 0
                    ? steadyClockNanoseconds()
                    : timestampNanoseconds,
                m_captureMetadata.tuningGeneration));
        }
        return itemCount;
    }

private:
    [[nodiscard]] static int itemSize(std::size_t fftSize)
    {
        const std::size_t bytes = sizeof(float) * fftSize;
        if (bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Spectrum magnitude item size is too large");
        }
        return static_cast<int>(bytes);
    }

    std::shared_ptr<FftFrameProcessor> m_processor;
    CaptureMetadataSnapshot m_captureMetadata;
    std::uint64_t m_sampleRate;
    std::size_t m_fftSize;
};

}  // namespace

class GnuRadioReceiverBackend::Impl final
{
public:
    [[nodiscard]] static radio::ReceiverLimits receiverLimitsForDevice(
        const std::shared_ptr<devices::DeviceController>& device)
    {
        radio::ReceiverLimits limits;
        if (device && device->selectedDevice().has_value()) {
            const auto& capabilities = device->selectedDevice()->capabilities;
            limits.allowsPartialPassbandAtFrequencyEdges =
                capabilities.rtlSdrBlogV4 &&
                capabilities.driverManagedHfBelow27Mhz;
        }
        return limits;
    }

    [[nodiscard]] static radio::ReceiverLimits receiverLimitsForRecordedSource(
        const radio::RecordedIqSourceConfiguration& source)
    {
        const std::uint64_t halfRate = source.sampleRate / 2;
        return {.frequency = {source.centerFrequency - halfRate,
                              source.centerFrequency + halfRate},
                .sampleRate = {source.sampleRate, source.sampleRate}};
    }

    class Flowgraph final
    {
    public:
        Flowgraph(
            const radio::ReceiverState& state,
            std::uint64_t effectiveSampleRate,
            PreparedSpectrumResources spectrumResources,
            std::shared_ptr<FftFrameProcessor> spectrumProcessor,
            std::shared_ptr<SpectrumProcessingCounters> spectrumCounters,
            SpectrumDisplayConfiguration spectrumConfiguration,
            std::uint64_t tuningGeneration,
            std::shared_ptr<radio::AudioSampleBuffer> sharedAudioSamples,
            std::shared_ptr<radio::AudioSampleBuffer> sharedDecoderInputSamples,
            std::shared_ptr<radio::ComplexSampleBuffer> sharedIqSamples,
            std::shared_ptr<radio::WidebandIqSource> inputSource,
            std::shared_ptr<RuntimeFailureState> sharedRuntimeFailure,
            bool verboseDspMetrics)
            : spectrumFftPlans(std::move(spectrumResources.fftPlans))
            , spectrumMagnitudeBuffers(
                  std::move(spectrumResources.magnitudeBuffers))
            , spectrumWindowCoherentGains(
                  std::move(spectrumResources.windowCoherentGains))
            , m_spectrumFftSize(spectrumConfiguration.fftSize)
            , captureMetadata(std::make_shared<CaptureMetadataState>(
                  inputSource->captureMetadata().centerFrequency, tuningGeneration))
            , topBlock(gr::make_top_block(
                  inputSource->capabilities().kind == radio::ReceiverSourceKind::Hardware
                      ? "sdr_receiver_selected_device"
                      : "sdr_receiver_synthetic"))
            , channelRatePlan(stableChannelRatePlan(
                  effectiveSampleRate, state.demodulationMode))
            , channelFilter(gr::filter::freq_xlating_fir_filter_ccc::make(
                  static_cast<int>(channelRatePlan.decimation),
                  channelFilterTaps(state, effectiveSampleRate),
                  translationOffsetHz(state),
                  static_cast<double>(effectiveSampleRate)))
            , squelch(gr::analog::pwr_squelch_cc::make(squelchThresholdDb(state)))
            , squelchSignalStrength(std::make_shared<SquelchSignalStrengthState>())
            , squelchMeasurement(SquelchSignalStrengthSink::make(
                  squelchSignalStrength))
            , demodulationInputSelector(gr::blocks::selector::make(
                  sizeof(gr_complex),
                  0,
                  static_cast<unsigned int>(state.demodulationMode)))
            , amAgc(gr::analog::agc3_cc::make(
                  narrowModeAgcAttackRate,
                  narrowModeAgcDecayRate,
                  narrowModeAgcReference,
                  1.0F,
                  1,
                  narrowModeAgcMaximumGain))
            , amDemodulator(gr::blocks::complex_to_mag::make())
            , amResampler(makeAudioResampler(channelRatePlan.outputSampleRate))
            , amDcBlocker(gr::filter::dc_blocker_ff::make(32, false))
            , nfmDemodulator(gr::analog::quadrature_demod_cf::make(
                  fmDemodulationGain(
                      channelRatePlan.outputSampleRate, 5'000.0)))
            , nfmResampler(makeAudioResampler(channelRatePlan.outputSampleRate))
            , nfmDeemphasis(gr::filter::single_pole_iir_filter_ff::make(
                  deemphasisAlpha(
                      radio::receiverAudioSampleRate,
                      narrowFmDeemphasisSeconds)))
            , wfmDemodulator(gr::analog::quadrature_demod_cf::make(
                  fmDemodulationGain(
                      channelRatePlan.outputSampleRate, 75'000.0)))
            , wfmResampler(makeAudioResampler(channelRatePlan.outputSampleRate))
            , wfmDeemphasis(gr::filter::single_pole_iir_filter_ff::make(
                  deemphasisAlpha(
                      radio::receiverAudioSampleRate,
                      wideFmDeemphasisSeconds)))
            , usbSidebandFilter(gr::filter::fir_filter_ccc::make(
                  1,
                  sidebandChannelTaps(
                      state,
                      radio::DemodulationMode::Usb,
                      channelRatePlan.outputSampleRate)))
            , usbAgc(gr::analog::agc3_cc::make(
                  narrowModeAgcAttackRate,
                  narrowModeAgcDecayRate,
                  narrowModeAgcReference,
                  1.0F,
                  1,
                  narrowModeAgcMaximumGain))
            , usbDemodulator(gr::blocks::complex_to_real::make())
            , usbResampler(makeAudioResampler(channelRatePlan.outputSampleRate))
            , usbAudioFilter(gr::filter::fir_filter_fff::make(
                  1, sidebandAudioTaps(state)))
            , lsbSidebandFilter(gr::filter::fir_filter_ccc::make(
                  1,
                  sidebandChannelTaps(
                      state,
                      radio::DemodulationMode::Lsb,
                      channelRatePlan.outputSampleRate)))
            , lsbAgc(gr::analog::agc3_cc::make(
                  narrowModeAgcAttackRate,
                  narrowModeAgcDecayRate,
                  narrowModeAgcReference,
                  1.0F,
                  1,
                  narrowModeAgcMaximumGain))
            , lsbDemodulator(gr::blocks::complex_to_real::make())
            , lsbResampler(makeAudioResampler(channelRatePlan.outputSampleRate))
            , lsbAudioFilter(gr::filter::fir_filter_fff::make(
                  1, sidebandAudioTaps(state)))
            , digitalDemodulator(gr::analog::quadrature_demod_cf::make(
                  fmDemodulationGain(
                      channelRatePlan.outputSampleRate, 6'250.0)))
            , digitalResampler(makeAudioResampler(
                  channelRatePlan.outputSampleRate))
            , digitalDcBlocker(gr::filter::dc_blocker_ff::make(32, false))
            , digitalBranchSelector(gr::blocks::selector::make(
                  sizeof(gr_complex),
                  0,
                  state.demodulationMode ==
                          radio::DemodulationMode::DigitalDecoderOutput
                      ? 0U
                      : 1U))
            , demodulationSelector(gr::blocks::selector::make(
                  sizeof(float),
                  static_cast<unsigned int>(state.demodulationMode),
                  0))
            , outputRouter(gr::blocks::selector::make(
                  sizeof(float),
                  0,
                  state.demodulationMode == radio::DemodulationMode::DigitalDecoderOutput
                      ? 1U
                      : 0U))
            , audioSink(AudioSampleSink::make(sharedAudioSamples))
            , iqSink(ComplexSampleSink::make(std::move(sharedIqSamples)))
            , digitalInputNullSink(gr::blocks::null_sink::make(
                  sizeof(gr_complex)))
            , digitalInactiveNullSink(gr::blocks::null_sink::make(
                  sizeof(gr_complex)))
            , digitalOutputNullSink(gr::blocks::null_sink::make(
                  sizeof(float)))
            , digitalDecoderSink(AudioSampleSink::make(
                  sharedDecoderInputSamples, true))
            , spectrumWindowGenerator(SpectrumWindowGenerator::make(
                  m_spectrumFftSize,
                  effectiveSampleRate,
                  spectrumConfiguration.targetFramesPerSecond,
                  std::move(spectrumCounters),
                  captureMetadata->snapshot()))
            , spectrumFft(spectrumFftPlans.at(m_spectrumFftSize))
            , spectrumMagnitude(
                  spectrumMagnitudeBuffers.at(m_spectrumFftSize))
            , spectrumSink(DisplayMagnitudeSink::make(
                  std::move(spectrumProcessor),
                  captureMetadata->snapshot(),
                  effectiveSampleRate,
                  m_spectrumFftSize))
            , m_source(std::move(inputSource))
            , runtimeFailure(std::move(sharedRuntimeFailure))
            , desiredMode(state.demodulationMode)
            , m_effectiveSampleRate(effectiveSampleRate)
            , lifecycle({
                  .startScheduler = [this] { topBlock->start(); },
                  .stopScheduler = [this] { topBlock->stop(); },
                  .waitScheduler = [this] { topBlock->wait(); },
                  .startSource = [this] {
                      const auto result = m_source->start();
                      if (!result.succeeded) {
                          throw std::runtime_error(result.message);
                      }
                  },
                  .stopSource = [this] {
                      const auto result = m_source->stop();
                      if (!result.succeeded) {
                          throw std::runtime_error(result.message);
                      }
                  },
              })
        {
            if (m_source->captureMetadata().effectiveSampleRate !=
                effectiveSampleRate) {
                throw std::invalid_argument(
                    "Wideband IQ source sample rate does not match the receiver flowgraph");
            }
            widebandSource = WidebandIqSourceBlock::make(
                this->m_source, this->runtimeFailure, captureMetadata);

            topBlock->connect(widebandSource, 0, iqSink, 0);
            topBlock->connect(widebandSource, 0, channelFilter, 0);
            topBlock->connect(channelFilter, 0, squelch, 0);
            topBlock->connect(channelFilter, 0, squelchMeasurement, 0);
            topBlock->connect(squelch, 0, demodulationInputSelector, 0);
            topBlock->connect(demodulationInputSelector, 0, amAgc, 0);
            topBlock->connect(amAgc, 0, amDemodulator, 0);
            topBlock->connect(amDemodulator, 0, amResampler, 0);
            topBlock->connect(amResampler, 0, amDcBlocker, 0);
            topBlock->connect(amDcBlocker, 0, demodulationSelector, 0);
            topBlock->connect(demodulationInputSelector, 1, nfmDemodulator, 0);
            topBlock->connect(nfmDemodulator, 0, nfmResampler, 0);
            topBlock->connect(nfmResampler, 0, nfmDeemphasis, 0);
            topBlock->connect(nfmDeemphasis, 0, demodulationSelector, 1);
            topBlock->connect(demodulationInputSelector, 2, wfmDemodulator, 0);
            topBlock->connect(wfmDemodulator, 0, wfmResampler, 0);
            topBlock->connect(wfmResampler, 0, wfmDeemphasis, 0);
            topBlock->connect(wfmDeemphasis, 0, demodulationSelector, 2);
            topBlock->connect(demodulationInputSelector, 3, usbAgc, 0);
            topBlock->connect(usbAgc, 0, usbSidebandFilter, 0);
            topBlock->connect(usbSidebandFilter, 0, usbDemodulator, 0);
            topBlock->connect(usbDemodulator, 0, usbResampler, 0);
            topBlock->connect(usbResampler, 0, usbAudioFilter, 0);
            topBlock->connect(usbAudioFilter, 0, demodulationSelector, 3);
            topBlock->connect(demodulationInputSelector, 4, lsbAgc, 0);
            topBlock->connect(lsbAgc, 0, lsbSidebandFilter, 0);
            topBlock->connect(lsbSidebandFilter, 0, lsbDemodulator, 0);
            topBlock->connect(lsbDemodulator, 0, lsbResampler, 0);
            topBlock->connect(lsbResampler, 0, lsbAudioFilter, 0);
            topBlock->connect(lsbAudioFilter, 0, demodulationSelector, 4);
            topBlock->connect(
                demodulationInputSelector, 5, digitalInputNullSink, 0);
            topBlock->connect(channelFilter, 0, digitalBranchSelector, 0);
            topBlock->connect(
                digitalBranchSelector, 0, digitalDemodulator, 0);
            topBlock->connect(
                digitalBranchSelector, 1, digitalInactiveNullSink, 0);
            topBlock->connect(digitalDemodulator, 0, digitalResampler, 0);
            topBlock->connect(digitalResampler, 0, digitalDcBlocker, 0);
            topBlock->connect(digitalDcBlocker, 0, demodulationSelector, 5);
            topBlock->connect(digitalDcBlocker, 0, digitalDecoderSink, 0);
            topBlock->connect(demodulationSelector, 0, outputRouter, 0);
            topBlock->connect(outputRouter, 0, audioSink, 0);
            topBlock->connect(outputRouter, 1, digitalOutputNullSink, 0);

            topBlock->connect(widebandSource, 0, spectrumWindowGenerator, 0);
            topBlock->connect(spectrumWindowGenerator, 0, spectrumFft, 0);
            topBlock->connect(spectrumFft, 0, spectrumMagnitude, 0);
            topBlock->connect(spectrumMagnitude, 0, spectrumSink, 0);

            if (verboseDspMetrics) {
                addRmsDiagnostics(state.demodulationMode);
            }
        }

        [[nodiscard]] std::size_t effectiveSpectrumFftSize(
            std::size_t requestedFftSize) const
        {
            if (spectrumFftPlans.contains(requestedFftSize)) {
                return requestedFftSize;
            }
            for (auto size = supportedSpectrumFftSizes.rbegin();
                 size != supportedSpectrumFftSizes.rend(); ++size) {
                if (*size < requestedFftSize &&
                    spectrumFftPlans.contains(*size)) {
                    return *size;
                }
            }
            for (const std::size_t size : supportedSpectrumFftSizes) {
                if (spectrumFftPlans.contains(size)) {
                    return size;
                }
            }
            throw std::runtime_error(
                "GNU Radio has no usable spectrum FFT plan");
        }

        [[nodiscard]] float spectrumWindowCoherentGain(
            std::size_t fftSize) const
        {
            return spectrumWindowCoherentGains.at(fftSize);
        }

        void reconfigureSpectrum(
            SpectrumDisplayConfiguration configuration,
            std::shared_ptr<FftFrameProcessor> processor,
            std::shared_ptr<SpectrumProcessingCounters> counters)
        {
            auto replacementWindowGenerator = SpectrumWindowGenerator::make(
                configuration.fftSize,
                m_effectiveSampleRate,
                configuration.targetFramesPerSecond,
                std::move(counters),
                captureMetadata->snapshot());
            const auto replacementFft = spectrumFftPlans.at(configuration.fftSize);
            const auto replacementMagnitude =
                spectrumMagnitudeBuffers.at(configuration.fftSize);
            auto replacementSink = DisplayMagnitudeSink::make(
                std::move(processor),
                captureMetadata->snapshot(),
                m_effectiveSampleRate,
                configuration.fftSize);

            topBlock->lock();
            try {
                topBlock->disconnect(widebandSource, 0, spectrumWindowGenerator, 0);
                topBlock->disconnect(spectrumWindowGenerator, 0, spectrumFft, 0);
                topBlock->disconnect(spectrumFft, 0, spectrumMagnitude, 0);
                topBlock->disconnect(spectrumMagnitude, 0, spectrumSink, 0);
                topBlock->connect(widebandSource, 0, replacementWindowGenerator, 0);
                topBlock->connect(replacementWindowGenerator, 0, replacementFft, 0);
                topBlock->connect(replacementFft, 0, replacementMagnitude, 0);
                topBlock->connect(replacementMagnitude, 0, replacementSink, 0);
            } catch (...) {
                try {
                    topBlock->disconnect(widebandSource, 0, replacementWindowGenerator, 0);
                    topBlock->disconnect(replacementWindowGenerator, 0, replacementFft, 0);
                    topBlock->disconnect(replacementFft, 0, replacementMagnitude, 0);
                    topBlock->disconnect(replacementMagnitude, 0, replacementSink, 0);
                    topBlock->connect(widebandSource, 0, spectrumWindowGenerator, 0);
                    topBlock->connect(spectrumWindowGenerator, 0, spectrumFft, 0);
                    topBlock->connect(spectrumFft, 0, spectrumMagnitude, 0);
                    topBlock->connect(spectrumMagnitude, 0, spectrumSink, 0);
                } catch (...) {
                }
                topBlock->unlock();
                throw;
            }
            spectrumWindowGenerator = std::move(replacementWindowGenerator);
            spectrumFft = std::move(replacementFft);
            spectrumMagnitude = std::move(replacementMagnitude);
            spectrumSink = std::move(replacementSink);
            m_spectrumFftSize = configuration.fftSize;
            topBlock->unlock();
        }

        void start()
        {
            if (lifecycle.running()) {
                return;
            }
            runtimeFailure->clear();
            try {
                lifecycle.start([this] {
                    selectDemodulationMode(desiredMode);
                });
            } catch (...) {
                rebuildBeforeNextStart = true;
                throw;
            }
        }

        void stopAndWait()
        {
            const bool completedRun = lifecycle.running();
            rebuildBeforeNextStart = rebuildBeforeNextStart || completedRun;
            lifecycle.stopAndWait();
        }

        [[nodiscard]] bool requiresRebuildBeforeStart() const noexcept
        {
            return rebuildBeforeNextStart;
        }

        [[nodiscard]] std::uint64_t effectiveSampleRate() const noexcept
        {
            return m_effectiveSampleRate;
        }

        void updateFrequency(const radio::ReceiverState& state)
        {
            channelFilter->set_center_freq(translationOffsetHz(state));
        }

        void updateFilterWidth(const radio::ReceiverState& state)
        {
            channelFilter->set_taps(
                channelFilterTaps(state, m_effectiveSampleRate));
            if (state.demodulationMode == radio::DemodulationMode::Usb) {
                usbSidebandFilter->set_taps(sidebandChannelTaps(
                    state,
                    radio::DemodulationMode::Usb,
                    channelRatePlan.outputSampleRate));
                usbAudioFilter->set_taps(sidebandAudioTaps(state));
            } else if (
                state.demodulationMode == radio::DemodulationMode::Lsb) {
                lsbSidebandFilter->set_taps(sidebandChannelTaps(
                    state,
                    radio::DemodulationMode::Lsb,
                    channelRatePlan.outputSampleRate));
                lsbAudioFilter->set_taps(sidebandAudioTaps(state));
            }
        }

        void updateCenterFrequency(
            const radio::ReceiverState& state,
            std::uint64_t tuningGeneration)
        {
            updateFrequency(state);
            captureMetadata->update(
                state.centerFrequency, tuningGeneration);
        }

        void updateSquelch(const radio::ReceiverState& state)
        {
            squelch->set_threshold(squelchThresholdDb(state));
        }

        [[nodiscard]] bool squelchOpen() const noexcept
        {
            return squelch->unmuted();
        }

        [[nodiscard]] std::optional<double> squelchSignalStrengthDb()
            const noexcept
        {
            return squelchSignalStrength->latest();
        }

        void updateOutputRouteForMode(radio::DemodulationMode mode)
        {
            outputRouter->set_output_index(
                mode == radio::DemodulationMode::DigitalDecoderOutput
                    ? 1U
                    : 0U);
        }

        void selectDemodulationMode(radio::DemodulationMode mode)
        {
            const auto index = static_cast<unsigned int>(mode);
            demodulationSelector->set_input_index(index);
            demodulationInputSelector->set_output_index(index);
            digitalBranchSelector->set_output_index(
                mode == radio::DemodulationMode::DigitalDecoderOutput
                    ? 0U
                    : 1U);
            updateOutputRouteForMode(mode);
        }

        void addRmsProbe(
            const gr::basic_block_sptr& source,
            std::string stage,
            std::size_t itemSize,
            std::uint32_t sampleRate,
            bool complexSamples)
        {
            auto probe = RmsDiagnosticSink::make(
                std::move(stage), itemSize, sampleRate, complexSamples);
            topBlock->connect(source, 0, probe, 0);
            rmsDiagnosticSinks.push_back(std::move(probe));
        }

        void addRmsDiagnostics(radio::DemodulationMode mode)
        {
            const auto channelRate = channelRatePlan.outputSampleRate;
            addRmsProbe(
                channelFilter,
                "channel-filter",
                sizeof(gr_complex),
                channelRate,
                true);
            switch (mode) {
            case radio::DemodulationMode::Am:
                addRmsProbe(
                    amDemodulator,
                    "am-demod",
                    sizeof(float),
                    channelRate,
                    false);
                addRmsProbe(
                    amResampler,
                    "am-resampler",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                addRmsProbe(
                    amDcBlocker,
                    "am-audio-filter",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                break;
            case radio::DemodulationMode::Nfm:
                addRmsProbe(
                    nfmDemodulator,
                    "nfm-demod",
                    sizeof(float),
                    channelRate,
                    false);
                addRmsProbe(
                    nfmResampler,
                    "nfm-resampler",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                addRmsProbe(
                    nfmDeemphasis,
                    "nfm-audio-filter",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                break;
            case radio::DemodulationMode::Wfm:
                addRmsProbe(
                    wfmDemodulator,
                    "wfm-demod",
                    sizeof(float),
                    channelRate,
                    false);
                addRmsProbe(
                    wfmResampler,
                    "wfm-resampler",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                addRmsProbe(
                    wfmDeemphasis,
                    "wfm-audio-filter",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                break;
            case radio::DemodulationMode::Usb:
                addRmsProbe(
                    usbSidebandFilter,
                    "usb-channel-filter",
                    sizeof(gr_complex),
                    channelRate,
                    true);
                addRmsProbe(
                    usbDemodulator,
                    "usb-demod",
                    sizeof(float),
                    channelRate,
                    false);
                addRmsProbe(
                    usbResampler,
                    "usb-resampler",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                addRmsProbe(
                    usbAudioFilter,
                    "usb-audio-filter",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                break;
            case radio::DemodulationMode::Lsb:
                addRmsProbe(
                    lsbSidebandFilter,
                    "lsb-channel-filter",
                    sizeof(gr_complex),
                    channelRate,
                    true);
                addRmsProbe(
                    lsbDemodulator,
                    "lsb-demod",
                    sizeof(float),
                    channelRate,
                    false);
                addRmsProbe(
                    lsbResampler,
                    "lsb-resampler",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                addRmsProbe(
                    lsbAudioFilter,
                    "lsb-audio-filter",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
                break;
            case radio::DemodulationMode::DigitalDecoderOutput:
                break;
            }
            if (mode != radio::DemodulationMode::DigitalDecoderOutput) {
                addRmsProbe(
                    outputRouter,
                    "bounded-audio-sink",
                    sizeof(float),
                    radio::receiverAudioSampleRate,
                    false);
            }
        }

    private:
        std::unordered_map<
            std::size_t,
            gr::fft::fft_v<gr_complex, true>::sptr> spectrumFftPlans;
        std::unordered_map<
            std::size_t,
            gr::blocks::complex_to_mag::sptr> spectrumMagnitudeBuffers;
        std::unordered_map<std::size_t, float> spectrumWindowCoherentGains;
        std::size_t m_spectrumFftSize;
        std::shared_ptr<CaptureMetadataState> captureMetadata;
        gr::top_block_sptr topBlock;
        const ChannelRatePlan channelRatePlan;
        gr::basic_block_sptr widebandSource;
        gr::filter::freq_xlating_fir_filter_ccc::sptr channelFilter;
        gr::analog::pwr_squelch_cc::sptr squelch;
        std::shared_ptr<SquelchSignalStrengthState> squelchSignalStrength;
        SquelchSignalStrengthSink::sptr squelchMeasurement;
        gr::blocks::selector::sptr demodulationInputSelector;
        gr::analog::agc3_cc::sptr amAgc;
        gr::blocks::complex_to_mag::sptr amDemodulator;
        gr::filter::rational_resampler_fff::sptr amResampler;
        gr::filter::dc_blocker_ff::sptr amDcBlocker;
        gr::analog::quadrature_demod_cf::sptr nfmDemodulator;
        gr::filter::rational_resampler_fff::sptr nfmResampler;
        gr::filter::single_pole_iir_filter_ff::sptr nfmDeemphasis;
        gr::analog::quadrature_demod_cf::sptr wfmDemodulator;
        gr::filter::rational_resampler_fff::sptr wfmResampler;
        gr::filter::single_pole_iir_filter_ff::sptr wfmDeemphasis;
        gr::filter::fir_filter_ccc::sptr usbSidebandFilter;
        gr::analog::agc3_cc::sptr usbAgc;
        gr::blocks::complex_to_real::sptr usbDemodulator;
        gr::filter::rational_resampler_fff::sptr usbResampler;
        gr::filter::fir_filter_fff::sptr usbAudioFilter;
        gr::filter::fir_filter_ccc::sptr lsbSidebandFilter;
        gr::analog::agc3_cc::sptr lsbAgc;
        gr::blocks::complex_to_real::sptr lsbDemodulator;
        gr::filter::rational_resampler_fff::sptr lsbResampler;
        gr::filter::fir_filter_fff::sptr lsbAudioFilter;
        gr::analog::quadrature_demod_cf::sptr digitalDemodulator;
        gr::filter::rational_resampler_fff::sptr digitalResampler;
        gr::filter::dc_blocker_ff::sptr digitalDcBlocker;
        gr::blocks::selector::sptr digitalBranchSelector;
        gr::blocks::selector::sptr demodulationSelector;
        gr::blocks::selector::sptr outputRouter;
        AudioSampleSink::sptr audioSink;
        ComplexSampleSink::sptr iqSink;
        gr::blocks::null_sink::sptr digitalInputNullSink;
        gr::blocks::null_sink::sptr digitalInactiveNullSink;
        gr::blocks::null_sink::sptr digitalOutputNullSink;
        AudioSampleSink::sptr digitalDecoderSink;
        SpectrumWindowGenerator::sptr spectrumWindowGenerator;
        gr::fft::fft_v<gr_complex, true>::sptr spectrumFft;
        gr::blocks::complex_to_mag::sptr spectrumMagnitude;
        DisplayMagnitudeSink::sptr spectrumSink;
        std::vector<RmsDiagnosticSink::sptr> rmsDiagnosticSinks;
        std::shared_ptr<radio::WidebandIqSource> m_source;
        std::shared_ptr<RuntimeFailureState> runtimeFailure;
        radio::DemodulationMode desiredMode;
        const std::uint64_t m_effectiveSampleRate;
        detail::FlowgraphLifecycle lifecycle;
        bool rebuildBeforeNextStart = false;
    };

    explicit Impl(
        std::shared_ptr<devices::DeviceController> device = {},
        std::optional<radio::RecordedIqSourceConfiguration> recorded = std::nullopt,
        SpectrumDisplayConfiguration spectrumConfiguration = {},
        bool verboseDspMetrics = false)
        : m_initialSpectrum(spectrumConfiguration)
        , model(recorded ? receiverLimitsForRecordedSource(*recorded)
                         : receiverLimitsForDevice(device))
        , spectrumFrames(std::make_shared<radio::SpectrumFrameQueue>(64))
        , spectrumCounters(std::make_shared<SpectrumProcessingCounters>())
        , spectrumProcessor(makeSpectrumProcessor(
              spectrumFrames,
              spectrumCounters,
              m_initialSpectrum.effective,
              m_initialSpectrum.resources.windowCoherentGain(
                  m_initialSpectrum.effective.fftSize)))
        , audioSamples(std::make_shared<radio::AudioSampleBuffer>(
              channelAudioBufferCapacity))
        , decoderInputSamples(std::make_shared<radio::AudioSampleBuffer>(
              channelAudioBufferCapacity))
        , iqSamples(std::make_shared<radio::ComplexSampleBuffer>(
              2'400'000))
        , runtimeFailure(std::make_shared<RuntimeFailureState>())
        , selectedDevice(std::move(device))
        , recordedSource(std::move(recorded))
        , m_requestedSpectrumFftSize(m_initialSpectrum.requested.fftSize)
        , m_spectrumConfiguration(m_initialSpectrum.effective)
        , m_verboseDspMetrics(verboseDspMetrics)
        , flowgraph(makeFlowgraph(
              model.state(), std::move(m_initialSpectrum.resources)))
    {
        if (recordedSource) {
            static_cast<void>(model.setSampleRate(recordedSource->sampleRate));
            static_cast<void>(model.setCenterFrequency(recordedSource->centerFrequency));
            sourceCapabilities = {.kind = radio::ReceiverSourceKind::RecordedIq,
                                  .sampleRateChangeSupported = false};
        }
        if (this->selectedDevice) {
            const auto& deviceCapabilities =
                this->selectedDevice->selectedDevice()->capabilities;
            capabilities.ppmCorrectionSupported =
                deviceCapabilities.ppmCorrectionSupported;
            capabilities.automaticPpmCalibrationSupported =
                deviceCapabilities.ppmCorrectionSupported &&
                deviceCapabilities.rtlSdrTestModeSupported;
            sourceCapabilities = {
                .kind = radio::ReceiverSourceKind::Hardware,
                .hardwareTuningSupported = true,
                .gainControlSupported = deviceCapabilities.gainSupported,
                .ppmCorrectionSupported = deviceCapabilities.ppmCorrectionSupported,
                .automaticPpmCalibrationSupported =
                    capabilities.automaticPpmCalibrationSupported,
            };
            if (const auto ppm = this->selectedDevice->ppmCorrection()) {
                static_cast<void>(model.setPpmCorrection(*ppm));
            }
        }
    }

    [[nodiscard]] std::unique_ptr<Flowgraph> makeFlowgraph(
        const radio::ReceiverState& state,
        PreparedSpectrumResources spectrumResources)
    {
        const radio::WidebandIqCaptureMetadata metadata{
            .centerFrequency = state.centerFrequency,
            .effectiveSampleRate = effectiveSampleRate,
        };
        std::shared_ptr<radio::WidebandIqSource> source;
        if (recordedSource) {
            source = std::make_shared<RecordedIqSource>(*recordedSource);
        } else if (selectedDevice) {
            source = std::make_shared<DeviceControllerIqSource>(
                selectedDevice, metadata);
        } else {
            source = std::make_shared<SyntheticIqSource>(metadata);
        }
        return std::make_unique<Flowgraph>(
            state,
            effectiveSampleRate,
            std::move(spectrumResources),
            spectrumProcessor,
            spectrumCounters,
            m_spectrumConfiguration,
            tuningGeneration,
            audioSamples,
            decoderInputSamples,
            iqSamples,
            std::move(source),
            runtimeFailure,
            m_verboseDspMetrics);
    }

    [[nodiscard]] std::unique_ptr<Flowgraph> makeFlowgraph(
        const radio::ReceiverState& state)
    {
        PreparedSpectrumResources spectrumResources;
        if (spectrumResources.effectiveFftSize(
                m_spectrumConfiguration.fftSize) !=
            m_spectrumConfiguration.fftSize) {
            throw std::runtime_error(
                "GNU Radio could not recreate the effective spectrum FFT plan");
        }
        return makeFlowgraph(state, std::move(spectrumResources));
    }

    [[nodiscard]] std::optional<radio::OperationResult> configureDevice(
        const radio::ReceiverState& state)
    {
        if (!selectedDevice) {
            return std::nullopt;
        }

        auto result = selectedDevice->setSampleRate(state.sampleRate);
        if (!result.succeeded()) {
            return deviceFailure(std::move(result));
        }
        const auto effectiveRate = selectedDevice->effectiveSampleRate();
        if (!effectiveRate.has_value()) {
            return radio::OperationResult{
                radio::ReceiverError::BackendFailure,
                false,
                false,
                "Selected SDR did not confirm an effective sample rate",
            };
        }
        effectiveSampleRate = *effectiveRate;
        const auto& deviceCapabilities =
            selectedDevice->selectedDevice()->capabilities;
        if (deviceCapabilities.ppmCorrectionSupported) {
            result = selectedDevice->setPpmCorrection(state.ppmCorrection);
            if (!result.succeeded()) {
                return deviceFailure(std::move(result));
            }
        }
        result = selectedDevice->tuneCenterFrequency(state.centerFrequency);
        if (!result.succeeded()) {
            return deviceFailure(std::move(result));
        }
        if (deviceCapabilities.gainSupported) {
            result = selectedDevice->setGain(state.gainDb);
            if (!result.succeeded()) {
                return deviceFailure(std::move(result));
            }
        }
        return std::nullopt;
    }

    template <typename Transition, typename DspUpdate>
    radio::OperationResult apply(Transition transition, DspUpdate dspUpdate)
    {
        radio::ReceiverStateModel candidate = model;
        radio::OperationResult result = std::invoke(transition, candidate);
        if (!result.succeeded() || !result.stateChanged) {
            return result;
        }

        try {
            std::invoke(dspUpdate, *flowgraph, candidate.state());
            model = std::move(candidate);
            return result;
        } catch (const std::exception& error) {
            return backendFailure(error);
        } catch (...) {
            return unknownBackendFailure();
        }
    }

    template <typename Transition>
    radio::OperationResult applyWithFlowgraphRebuild(Transition transition)
    {
        radio::ReceiverStateModel candidate = model;
        radio::OperationResult result = std::invoke(transition, candidate);
        if (!result.succeeded() || !result.stateChanged) {
            return result;
        }

        const bool wasRunning = model.state().running;
        try {
            if (wasRunning) {
                flowgraph->stopAndWait();
            }
            auto replacement = makeFlowgraph(candidate.state());
            audioSamples->clear();
            decoderInputSamples->clear();
            if (wasRunning) {
                replacement->start();
            }
            flowgraph = std::move(replacement);
            model = std::move(candidate);
            return result;
        } catch (const std::exception& error) {
            audioSamples->clear();
            decoderInputSamples->clear();
            if (wasRunning) {
                try {
                    auto recovery = makeFlowgraph(model.state());
                    recovery->start();
                    flowgraph = std::move(recovery);
                } catch (...) {
                }
            }
            return backendFailure(error);
        } catch (...) {
            audioSamples->clear();
            decoderInputSamples->clear();
            if (wasRunning) {
                try {
                    auto recovery = makeFlowgraph(model.state());
                    recovery->start();
                    flowgraph = std::move(recovery);
                } catch (...) {
                }
            }
            return unknownBackendFailure();
        }
    }

    InitialSpectrumConfiguration m_initialSpectrum;
    radio::ReceiverStateModel model;
    radio::ReceiverCapabilities capabilities;
    radio::ReceiverSourceCapabilities sourceCapabilities{
        .kind = radio::ReceiverSourceKind::Synthetic,
    };
    std::shared_ptr<radio::SpectrumFrameQueue> spectrumFrames;
    std::shared_ptr<SpectrumProcessingCounters> spectrumCounters;
    std::shared_ptr<FftFrameProcessor> spectrumProcessor;
    std::shared_ptr<radio::AudioSampleBuffer> audioSamples;
    std::shared_ptr<radio::AudioSampleBuffer> decoderInputSamples;
    std::shared_ptr<radio::ComplexSampleBuffer> iqSamples;
    std::shared_ptr<RuntimeFailureState> runtimeFailure;
    std::shared_ptr<devices::DeviceController> selectedDevice;
    std::optional<radio::RecordedIqSourceConfiguration> recordedSource;
    std::size_t m_requestedSpectrumFftSize = defaultSpectrumFftSize;
    SpectrumDisplayConfiguration m_spectrumConfiguration;
    bool m_verboseDspMetrics = false;
    std::uint64_t tuningGeneration = 0;
    std::uint64_t effectiveSampleRate = model.state().sampleRate;
    std::unique_ptr<Flowgraph> flowgraph;
    bool ppmCalibrationActive = false;
    bool receptionPausedForPpmCalibration = false;
};

GnuRadioReceiverBackend::GnuRadioReceiverBackend()
    : m_impl(std::make_unique<Impl>())
{
}

GnuRadioReceiverBackend::GnuRadioReceiverBackend(
    SpectrumDisplayConfiguration spectrumConfiguration,
    bool verboseDspMetrics)
    : m_impl(std::make_unique<Impl>(
          nullptr, std::nullopt, spectrumConfiguration, verboseDspMetrics))
{
}

GnuRadioReceiverBackend::GnuRadioReceiverBackend(
    std::unique_ptr<devices::DeviceController> explicitlySelectedDevice,
    SpectrumDisplayConfiguration spectrumConfiguration,
    bool verboseDspMetrics)
{
    if (!explicitlySelectedDevice ||
        !explicitlySelectedDevice->selectedDevice().has_value()) {
        throw std::invalid_argument(
            "GNU Radio hardware reception requires an explicitly selected device");
    }
    m_impl = std::make_unique<Impl>(
        std::shared_ptr<devices::DeviceController>(
            std::move(explicitlySelectedDevice)),
        std::nullopt,
        spectrumConfiguration,
        verboseDspMetrics);
}

GnuRadioReceiverBackend::GnuRadioReceiverBackend(
    radio::RecordedIqSourceConfiguration recordedSource,
    SpectrumDisplayConfiguration spectrumConfiguration,
    bool verboseDspMetrics)
{
    const auto resolved = RecordedIqSource::resolveConfiguration(std::move(recordedSource));
    m_impl = std::make_unique<Impl>(
        nullptr, resolved, spectrumConfiguration, verboseDspMetrics);
}

GnuRadioReceiverBackend::~GnuRadioReceiverBackend()
{
    if (m_impl && m_impl->ppmCalibrationActive && m_impl->selectedDevice) {
        static_cast<void>(m_impl->selectedDevice->stopRtlSdrTestStream());
    }
}

const radio::ReceiverLimits& GnuRadioReceiverBackend::limits() const noexcept
{
    return m_impl->model.limits();
}

const radio::ReceiverCapabilities& GnuRadioReceiverBackend::capabilities()
    const noexcept
{
    return m_impl->capabilities;
}

radio::ReceiverSourceCapabilities GnuRadioReceiverBackend::sourceCapabilities()
    const noexcept
{
    return m_impl->sourceCapabilities;
}

const radio::ReceiverState& GnuRadioReceiverBackend::state() const noexcept
{
    return m_impl->model.state();
}

std::uint64_t GnuRadioReceiverBackend::effectiveSampleRate() const noexcept
{
    return m_impl->effectiveSampleRate;
}

std::uint64_t GnuRadioReceiverBackend::tuningGeneration() const noexcept
{
    return m_impl->tuningGeneration;
}

bool GnuRadioReceiverBackend::squelchOpen() const noexcept
{
    return m_impl->flowgraph->squelchOpen();
}

std::optional<double> GnuRadioReceiverBackend::squelchSignalStrengthDb()
    const noexcept
{
    return m_impl->flowgraph->squelchSignalStrengthDb();
}

std::optional<radio::SpectrumFrame> GnuRadioReceiverBackend::takeLatestSpectrumFrame()
{
    return m_impl->spectrumFrames->takeLatest();
}

std::vector<radio::SpectrumFrame>
GnuRadioReceiverBackend::takePendingSpectrumFrames(std::size_t maximumFrames)
{
    std::vector<radio::SpectrumFrame> frames;
    frames.reserve(std::min(maximumFrames, m_impl->spectrumFrames->size()));
    while (frames.size() < maximumFrames) {
        auto frame = m_impl->spectrumFrames->takeOldest();
        if (!frame) {
            break;
        }
        frames.push_back(std::move(*frame));
    }
    return frames;
}

radio::SpectrumProcessingMetrics
GnuRadioReceiverBackend::spectrumProcessingMetrics() const
{
    const auto fftCount = m_impl->spectrumCounters->fftsExecuted.load(
        std::memory_order_relaxed);
    const SpectrumWindowHopScheduler scheduler(
        m_impl->effectiveSampleRate,
        m_impl->m_spectrumConfiguration.fftSize,
        m_impl->m_spectrumConfiguration.targetFramesPerSecond);
    return {
        .inputSamples = m_impl->spectrumCounters->inputSamples.load(
            std::memory_order_relaxed),
        .vectorsReceived = m_impl->spectrumCounters->vectorsReceived.load(
            std::memory_order_relaxed),
        .fftsExecuted = fftCount,
        .framesPublished = m_impl->spectrumCounters->framesPublished.load(
            std::memory_order_relaxed),
        .framesDropped = m_impl->spectrumFrames->droppedFrameCount(),
        .fftSize = m_impl->m_spectrumConfiguration.fftSize,
        .queueDepth = m_impl->spectrumFrames->size(),
        .effectiveSampleRate = static_cast<double>(m_impl->effectiveSampleRate),
        .availableVectorsPerSecond =
            static_cast<double>(m_impl->effectiveSampleRate) /
            static_cast<double>(m_impl->m_spectrumConfiguration.fftSize),
        .targetFramesPerSecond = static_cast<double>(
            m_impl->m_spectrumConfiguration.targetFramesPerSecond),
        .achievableFramesPerSecond = scheduler.achievableFramesPerSecond(),
        .hertzPerBin = static_cast<double>(m_impl->effectiveSampleRate) /
                       static_cast<double>(m_impl->m_spectrumConfiguration.fftSize),
        .hopSize = scheduler.nominalHopSize(),
        .overlapPercentage = scheduler.overlapPercentage(),
        .averageProcessingMilliseconds = fftCount == 0
            ? 0.0
            : static_cast<double>(
                  m_impl->spectrumCounters->processingNanoseconds.load(
                      std::memory_order_relaxed)) /
                  static_cast<double>(fftCount) / 1'000'000.0,
    };
}

std::size_t GnuRadioReceiverBackend::spectrumFftSize() const noexcept
{
    return m_impl->m_spectrumConfiguration.fftSize;
}

std::size_t GnuRadioReceiverBackend::requestedSpectrumFftSize() const noexcept
{
    return m_impl->m_requestedSpectrumFftSize;
}

std::uint32_t GnuRadioReceiverBackend::spectrumFramesPerSecond() const noexcept
{
    return m_impl->m_spectrumConfiguration.targetFramesPerSecond;
}

radio::OperationResult GnuRadioReceiverBackend::setSpectrumFftSize(
    std::size_t fftSize)
{
    const std::size_t requestedFftSize = fftSize;
    auto configuration = m_impl->m_spectrumConfiguration;
    configuration.fftSize = fftSize;
    try {
        configuration = validateSpectrumConfiguration(configuration);
    } catch (const std::exception& error) {
        return {
            radio::ReceiverError::BackendFailure,
            false,
            false,
            error.what(),
        };
    }
    if (requestedFftSize == m_impl->m_requestedSpectrumFftSize) {
        return {
            radio::ReceiverError::None,
            false,
            false,
            "Spectrum FFT size is unchanged",
        };
    }

    try {
        configuration.fftSize =
            m_impl->flowgraph->effectiveSpectrumFftSize(requestedFftSize);
        const bool effectiveSizeChanged =
            configuration.fftSize !=
            m_impl->m_spectrumConfiguration.fftSize;
        if (!effectiveSizeChanged) {
            m_impl->m_requestedSpectrumFftSize = requestedFftSize;
            return {
                radio::ReceiverError::None,
                true,
                configuration.fftSize != requestedFftSize,
                configuration.fftSize == requestedFftSize
                    ? "Spectrum FFT size changed"
                    : "Requested spectrum FFT size is unavailable; retained the effective fallback",
            };
        }
        auto counters = std::make_shared<SpectrumProcessingCounters>();
        auto processor = makeSpectrumProcessor(
            m_impl->spectrumFrames,
            counters,
            configuration,
            m_impl->flowgraph->spectrumWindowCoherentGain(
                configuration.fftSize));
        m_impl->spectrumFrames->clear();
        m_impl->flowgraph->reconfigureSpectrum(
            configuration, processor, counters);
        m_impl->spectrumProcessor = std::move(processor);
        m_impl->spectrumCounters = std::move(counters);
        m_impl->m_requestedSpectrumFftSize = requestedFftSize;
        m_impl->m_spectrumConfiguration = configuration;
        return {
            radio::ReceiverError::None,
            true,
            configuration.fftSize != requestedFftSize,
            configuration.fftSize == requestedFftSize
                ? "Spectrum FFT size changed without restarting audio"
                : "Requested spectrum FFT size was unavailable; a lower effective size was applied without restarting audio",
        };
    } catch (const std::exception& error) {
        return backendFailure(error);
    } catch (...) {
        return unknownBackendFailure();
    }
}

radio::OperationResult GnuRadioReceiverBackend::setSpectrumFramesPerSecond(
    std::uint32_t framesPerSecond)
{
    auto configuration = m_impl->m_spectrumConfiguration;
    configuration.targetFramesPerSecond = framesPerSecond;
    try {
        configuration = validateSpectrumConfiguration(configuration);
    } catch (const std::exception& error) {
        return {
            radio::ReceiverError::BackendFailure,
            false,
            false,
            error.what(),
        };
    }
    if (configuration.targetFramesPerSecond ==
        m_impl->m_spectrumConfiguration.targetFramesPerSecond) {
        return {
            radio::ReceiverError::None,
            false,
            false,
            "Internal spectrum source cadence is unchanged",
        };
    }

    try {
        auto counters = std::make_shared<SpectrumProcessingCounters>();
        auto processor = makeSpectrumProcessor(
            m_impl->spectrumFrames,
            counters,
            configuration,
            m_impl->flowgraph->spectrumWindowCoherentGain(
                configuration.fftSize));
        m_impl->spectrumFrames->clear();
        m_impl->flowgraph->reconfigureSpectrum(
            configuration, processor, counters);
        m_impl->spectrumProcessor = std::move(processor);
        m_impl->spectrumCounters = std::move(counters);
        m_impl->m_spectrumConfiguration = configuration;
        return {
            radio::ReceiverError::None,
            true,
            false,
            "Internal spectrum source cadence changed without restarting audio",
        };
    } catch (const std::exception& error) {
        return backendFailure(error);
    } catch (...) {
        return unknownBackendFailure();
    }
}

std::vector<float> GnuRadioReceiverBackend::takeAudioSamples(
    std::size_t maximumSamples)
{
    return m_impl->audioSamples->take(maximumSamples);
}

void GnuRadioReceiverBackend::clearAudioSamples()
{
    m_impl->audioSamples->clear();
}

std::uint64_t GnuRadioReceiverBackend::audioProducedSamples() const
{
    return m_impl->audioSamples->totalProducedSamples();
}

std::uint64_t GnuRadioReceiverBackend::audioDroppedSamples() const
{
    return m_impl->audioSamples->totalDroppedSamples();
}

void GnuRadioReceiverBackend::setFullBandwidthIqCaptureEnabled(bool enabled)
{
    m_impl->iqSamples->setEnabled(enabled);
}

std::vector<std::complex<float>> GnuRadioReceiverBackend::takeFullBandwidthIqSamples(
    std::size_t maximumSamples)
{
    return m_impl->iqSamples->take(maximumSamples);
}

void GnuRadioReceiverBackend::clearFullBandwidthIqSamples()
{
    m_impl->iqSamples->clear();
}

std::uint64_t GnuRadioReceiverBackend::fullBandwidthIqDroppedSamples() const
{
    return m_impl->iqSamples->totalDroppedSamples();
}

std::size_t GnuRadioReceiverBackend::audioBufferedSampleCount() const
{
    return m_impl->audioSamples->size();
}

std::vector<float> GnuRadioReceiverBackend::takeDecoderInputSamples(
    std::size_t maximumSamples)
{
    return m_impl->decoderInputSamples->take(maximumSamples);
}

void GnuRadioReceiverBackend::clearDecoderInputSamples()
{
    m_impl->decoderInputSamples->clear();
}

std::uint64_t GnuRadioReceiverBackend::decoderInputDroppedSamples() const
{
    return m_impl->decoderInputSamples->totalDroppedSamples();
}

radio::OperationResult GnuRadioReceiverBackend::startReception()
{
    radio::ReceiverStateModel candidate = m_impl->model;
    auto result = candidate.startReception();
    if (!result.succeeded() || !result.stateChanged) {
        return result;
    }
    if (auto configurationError = m_impl->configureDevice(candidate.state())) {
        return *configurationError;
    }
    if (m_impl->selectedDevice) {
        if (const auto effectiveGain = m_impl->selectedDevice->gain()) {
            static_cast<void>(candidate.setGain(*effectiveGain));
        }
    }
    try {
        m_impl->audioSamples->clear();
        m_impl->decoderInputSamples->clear();
        if (m_impl->selectedDevice ||
            m_impl->flowgraph->requiresRebuildBeforeStart() ||
            m_impl->flowgraph->effectiveSampleRate() !=
                m_impl->effectiveSampleRate) {
            m_impl->flowgraph = m_impl->makeFlowgraph(candidate.state());
        }
        m_impl->flowgraph->start();
        m_impl->model = std::move(candidate);
        return result;
    } catch (const std::exception& error) {
        return backendFailure(error);
    } catch (...) {
        return unknownBackendFailure();
    }
}

radio::OperationResult GnuRadioReceiverBackend::stopReception()
{
    radio::ReceiverStateModel candidate = m_impl->model;
    auto result = candidate.stopReception();
    if (!result.succeeded() || !result.stateChanged) {
        return result;
    }
    try {
        m_impl->flowgraph->stopAndWait();
        m_impl->audioSamples->clear();
        m_impl->decoderInputSamples->clear();
        m_impl->model = std::move(candidate);
        return result;
    } catch (const std::exception& error) {
        m_impl->model = std::move(candidate);
        auto failure = backendFailure(error);
        failure.stateChanged = true;
        return failure;
    } catch (...) {
        m_impl->model = std::move(candidate);
        auto failure = unknownBackendFailure();
        failure.stateChanged = true;
        return failure;
    }
}

radio::OperationResult GnuRadioReceiverBackend::setCenterFrequency(
    std::uint64_t frequency)
{
    if (m_impl->recordedSource) {
        return {radio::ReceiverError::CenterFrequencyOutOfRange, false, false,
                "Recorded IQ capture center is fixed; tune the listening frequency instead"};
    }
    radio::ReceiverStateModel candidate = m_impl->model;
    auto result = candidate.setCenterFrequency(frequency);
    if (!result.succeeded() || !result.stateChanged) {
        return result;
    }
    if (m_impl->selectedDevice) {
        auto deviceResult = m_impl->selectedDevice->tuneCenterFrequency(
            candidate.state().centerFrequency);
        if (!deviceResult.succeeded()) {
            return deviceFailure(std::move(deviceResult));
        }
    }
    try {
        const std::uint64_t nextGeneration = m_impl->tuningGeneration + 1;
        m_impl->flowgraph->updateCenterFrequency(
            candidate.state(), nextGeneration);
        m_impl->audioSamples->clear();
        m_impl->decoderInputSamples->clear();
        m_impl->tuningGeneration = nextGeneration;
        m_impl->model = std::move(candidate);
        return result;
    } catch (const std::exception& error) {
        return backendFailure(error);
    } catch (...) {
        return unknownBackendFailure();
    }
}

radio::OperationResult GnuRadioReceiverBackend::setListeningFrequency(
    std::uint64_t frequency)
{
    return m_impl->apply(
        [frequency](radio::ReceiverStateModel& model) {
            return model.setListeningFrequency(frequency);
        },
        [](Impl::Flowgraph& flowgraph, const radio::ReceiverState& state) {
            flowgraph.updateFrequency(state);
        });
}

radio::OperationResult GnuRadioReceiverBackend::tuneListeningFrequency(
    double normalizedPosition)
{
    return m_impl->apply(
        [normalizedPosition](radio::ReceiverStateModel& model) {
            return model.tuneListeningFrequency(normalizedPosition);
        },
        [](Impl::Flowgraph& flowgraph, const radio::ReceiverState& state) {
            flowgraph.updateFrequency(state);
        });
}

radio::OperationResult GnuRadioReceiverBackend::shiftCenterFrequency(
    std::int64_t requestedStep)
{
    if (m_impl->recordedSource) {
        return {radio::ReceiverError::CenterFrequencyOutOfRange, false, false,
                "Recorded IQ capture center is fixed; tune the listening frequency instead"};
    }
    radio::ReceiverStateModel candidate = m_impl->model;
    auto result = candidate.shiftCenterFrequency(requestedStep);
    if (!result.succeeded() || !result.stateChanged) {
        return result;
    }
    if (m_impl->selectedDevice) {
        auto deviceResult = m_impl->selectedDevice->tuneCenterFrequency(
            candidate.state().centerFrequency);
        if (!deviceResult.succeeded()) {
            return deviceFailure(std::move(deviceResult));
        }
    }
    try {
        const std::uint64_t nextGeneration = m_impl->tuningGeneration + 1;
        m_impl->flowgraph->updateCenterFrequency(
            candidate.state(), nextGeneration);
        m_impl->audioSamples->clear();
        m_impl->decoderInputSamples->clear();
        m_impl->tuningGeneration = nextGeneration;
        m_impl->model = std::move(candidate);
        return result;
    } catch (const std::exception& error) {
        return backendFailure(error);
    } catch (...) {
        return unknownBackendFailure();
    }
}

radio::OperationResult GnuRadioReceiverBackend::setSampleRate(
    std::uint64_t sampleRate)
{
    if (m_impl->recordedSource) {
        return {radio::ReceiverError::SampleRateOutOfRange, false, false,
                "Recorded IQ sample rate is fixed by the capture metadata"};
    }
    radio::ReceiverStateModel candidate = m_impl->model;
    radio::OperationResult result = candidate.setSampleRate(sampleRate);
    if (!result.succeeded() || !result.stateChanged) {
        return result;
    }

    const bool wasRunning = m_impl->model.state().running;
    const std::uint64_t previousEffectiveSampleRate = m_impl->effectiveSampleRate;
    const auto stopAfterFailedReconfiguration = [this, wasRunning](
                                                   radio::OperationResult failure) {
        if (wasRunning) {
            auto stopped = m_impl->model.stopReception();
            m_impl->audioSamples->clear();
            m_impl->decoderInputSamples->clear();
            m_impl->spectrumFrames->clear();
            failure.stateChanged = stopped.stateChanged;
            failure.message += "; reception stopped after capture-bandwidth reconfiguration failed";
        }
        return failure;
    };
    try {
        if (wasRunning) {
            m_impl->flowgraph->stopAndWait();
        }
        if (m_impl->selectedDevice) {
            auto deviceResult = m_impl->selectedDevice->setSampleRate(sampleRate);
            if (!deviceResult.succeeded()) {
                return stopAfterFailedReconfiguration(deviceFailure(std::move(deviceResult)));
            }
            const auto effectiveRate = m_impl->selectedDevice->effectiveSampleRate();
            if (!effectiveRate.has_value()) {
                return stopAfterFailedReconfiguration({
                    radio::ReceiverError::BackendFailure,
                    false,
                    false,
                    "Selected SDR did not confirm an effective sample rate",
                });
            }
            m_impl->effectiveSampleRate = *effectiveRate;
        } else {
            m_impl->effectiveSampleRate = sampleRate;
        }
        auto replacement = m_impl->makeFlowgraph(candidate.state());
        m_impl->audioSamples->clear();
        m_impl->decoderInputSamples->clear();
        m_impl->spectrumFrames->clear();
        if (wasRunning) {
            replacement->start();
        }
        m_impl->flowgraph = std::move(replacement);
        m_impl->model = std::move(candidate);
        return result;
    } catch (const std::exception& error) {
        m_impl->effectiveSampleRate = previousEffectiveSampleRate;
        return stopAfterFailedReconfiguration(backendFailure(error));
    } catch (...) {
        m_impl->effectiveSampleRate = previousEffectiveSampleRate;
        return stopAfterFailedReconfiguration(unknownBackendFailure());
    }
}

radio::OperationResult GnuRadioReceiverBackend::setFilterWidth(
    std::uint64_t filterWidth)
{
    return m_impl->apply(
        [filterWidth](radio::ReceiverStateModel& model) {
            return model.setFilterWidth(filterWidth);
        },
        [](Impl::Flowgraph& flowgraph, const radio::ReceiverState& state) {
            flowgraph.updateFilterWidth(state);
        });
}

radio::OperationResult GnuRadioReceiverBackend::setGain(double gainDb)
{
    radio::ReceiverStateModel candidate = m_impl->model;
    auto result = candidate.setGain(gainDb);
    if (!result.succeeded() || !result.stateChanged) {
        return result;
    }
    if (m_impl->selectedDevice) {
        auto deviceResult = m_impl->selectedDevice->setGain(gainDb);
        if (!deviceResult.succeeded()) {
            return deviceFailure(std::move(deviceResult));
        }
        if (const auto effectiveGain = m_impl->selectedDevice->gain()) {
            result = candidate.setGain(*effectiveGain);
            if (!result.succeeded()) {
                return result;
            }
            if (*effectiveGain != gainDb) {
                result.adjusted = true;
                result.message = "SDR device applied an effective gain of " +
                                 std::to_string(*effectiveGain) + " dB";
            }
        }
    }
    m_impl->model = std::move(candidate);
    return result;
}

radio::OperationResult GnuRadioReceiverBackend::setPpmCorrection(
    double ppmCorrection)
{
    if (!m_impl->selectedDevice ||
        !m_impl->capabilities.ppmCorrectionSupported) {
        return {
            radio::ReceiverError::PpmCorrectionUnsupported,
            false,
            false,
            "PPM correction requires a selected device that supports it",
        };
    }
    radio::ReceiverStateModel candidate = m_impl->model;
    auto result = candidate.setPpmCorrection(ppmCorrection);
    if (!result.succeeded()) {
        return result;
    }
    auto deviceResult = m_impl->selectedDevice->setPpmCorrection(ppmCorrection);
    if (!deviceResult.succeeded()) {
        return deviceFailure(std::move(deviceResult));
    }
    if (const auto effectivePpm = m_impl->selectedDevice->ppmCorrection()) {
        result = candidate.setPpmCorrection(*effectivePpm);
        if (!result.succeeded()) {
            return result;
        }
        if (*effectivePpm != ppmCorrection) {
            result.adjusted = true;
            result.message = "SDR device applied an effective correction of " +
                             std::to_string(*effectivePpm) + " PPM";
        }
    }
    m_impl->model = std::move(candidate);
    return result;
}

radio::OperationResult GnuRadioReceiverBackend::setDemodulationMode(
    radio::DemodulationMode mode)
{
    return m_impl->applyWithFlowgraphRebuild(
        [mode](radio::ReceiverStateModel& model) {
            return model.setDemodulationMode(mode);
        });
}

radio::OperationResult GnuRadioReceiverBackend::setSquelchLevel(
    double squelchLevelDb)
{
    return m_impl->apply(
        [squelchLevelDb](radio::ReceiverStateModel& model) {
            return model.setSquelchLevel(squelchLevelDb);
        },
        [](Impl::Flowgraph& flowgraph, const radio::ReceiverState& state) {
            flowgraph.updateSquelch(state);
        });
}

radio::OperationResult GnuRadioReceiverBackend::enableManualSquelch()
{
    return m_impl->apply(
        [](radio::ReceiverStateModel& model) {
            return model.enableManualSquelch();
        },
        [](Impl::Flowgraph& flowgraph, const radio::ReceiverState& state) {
            flowgraph.updateSquelch(state);
        });
}

radio::OperationResult GnuRadioReceiverBackend::disableSquelch()
{
    return m_impl->apply(
        [](radio::ReceiverStateModel& model) { return model.disableSquelch(); },
        [](Impl::Flowgraph& flowgraph, const radio::ReceiverState& state) {
            flowgraph.updateSquelch(state);
        });
}

double GnuRadioReceiverBackend::frequencyTranslationOffsetHz() const noexcept
{
    return translationOffsetHz(state());
}

bool GnuRadioReceiverBackend::usesHardwareSource() const noexcept
{
    return m_impl->selectedDevice != nullptr;
}

radio::OperationResult GnuRadioReceiverBackend::beginPpmCalibration()
{
    if (!m_impl->selectedDevice ||
        !m_impl->capabilities.automaticPpmCalibrationSupported) {
        return {
            radio::ReceiverError::PpmCorrectionUnsupported,
            false,
            false,
            "Automatic PPM calibration requires RTL-SDR test mode and frequency correction",
        };
    }
    if (m_impl->ppmCalibrationActive) {
        return {
            radio::ReceiverError::None,
            false,
            false,
            "Automatic PPM calibration is already running",
        };
    }

    m_impl->receptionPausedForPpmCalibration = m_impl->model.state().running;
    try {
        if (m_impl->receptionPausedForPpmCalibration) {
            m_impl->flowgraph->stopAndWait();
        }
        m_impl->audioSamples->clear();
        m_impl->decoderInputSamples->clear();
        m_impl->spectrumFrames->clear();
        auto correctionReset =
            m_impl->selectedDevice->setPpmCorrection(0.0);
        if (!correctionReset.succeeded()) {
            if (m_impl->receptionPausedForPpmCalibration) {
                auto replacement = m_impl->makeFlowgraph(m_impl->model.state());
                replacement->start();
                m_impl->flowgraph = std::move(replacement);
                m_impl->receptionPausedForPpmCalibration = false;
            }
            return deviceFailure(std::move(correctionReset));
        }
        auto result = m_impl->selectedDevice->startRtlSdrTestStream();
        if (!result.succeeded()) {
            static_cast<void>(m_impl->selectedDevice->setPpmCorrection(
                m_impl->model.state().ppmCorrection));
            if (m_impl->receptionPausedForPpmCalibration) {
                auto replacement = m_impl->makeFlowgraph(m_impl->model.state());
                replacement->start();
                m_impl->flowgraph = std::move(replacement);
                m_impl->receptionPausedForPpmCalibration = false;
            }
            return deviceFailure(std::move(result));
        }
        m_impl->ppmCalibrationActive = true;
        return {
            radio::ReceiverError::None,
            true,
            false,
            "RTL-SDR test mode enabled for PPM calibration",
        };
    } catch (const std::exception& error) {
        return backendFailure(error);
    } catch (...) {
        return unknownBackendFailure();
    }
}

radio::PpmCalibrationReadResult
GnuRadioReceiverBackend::readPpmCalibrationBytes(
    std::span<std::uint8_t> bytes,
    std::chrono::milliseconds timeout)
{
    if (!m_impl->ppmCalibrationActive || !m_impl->selectedDevice) {
        return {
            radio::PpmCalibrationReadStatus::Stopped,
            0,
            false,
            "Automatic PPM calibration is stopped",
        };
    }
    auto result =
        m_impl->selectedDevice->readRtlSdrTestBytes(bytes, timeout);
    radio::PpmCalibrationReadStatus status =
        radio::PpmCalibrationReadStatus::Failed;
    switch (result.status) {
    case devices::DeviceReadStatus::Samples:
        status = radio::PpmCalibrationReadStatus::Bytes;
        break;
    case devices::DeviceReadStatus::Timeout:
        status = radio::PpmCalibrationReadStatus::Timeout;
        break;
    case devices::DeviceReadStatus::Stopped:
        status = radio::PpmCalibrationReadStatus::Stopped;
        break;
    case devices::DeviceReadStatus::Disconnected:
        status = radio::PpmCalibrationReadStatus::Disconnected;
        break;
    case devices::DeviceReadStatus::Failed:
        status = radio::PpmCalibrationReadStatus::Failed;
        break;
    }
    return {
        status,
        result.byteCount,
        result.droppedData,
        std::move(result.message),
    };
}

radio::OperationResult GnuRadioReceiverBackend::endPpmCalibration()
{
    if (!m_impl->selectedDevice || !m_impl->ppmCalibrationActive) {
        return {
            radio::ReceiverError::None,
            false,
            false,
            "Automatic PPM calibration is stopped",
        };
    }
    auto result = m_impl->selectedDevice->stopRtlSdrTestStream();
    m_impl->ppmCalibrationActive = false;
    if (!result.succeeded()) {
        return deviceFailure(std::move(result));
    }
    return {
        radio::ReceiverError::None,
        true,
        false,
        "RTL-SDR test mode disabled",
    };
}

radio::OperationResult
GnuRadioReceiverBackend::resumeReceptionAfterPpmCalibration()
{
    if (!m_impl->receptionPausedForPpmCalibration) {
        return {
            radio::ReceiverError::None,
            false,
            false,
            "Reception was stopped before calibration",
        };
    }
    if (m_impl->ppmCalibrationActive) {
        return {
            radio::ReceiverError::BackendFailure,
            false,
            false,
            "Disable RTL-SDR test mode before restoring reception",
        };
    }
    try {
        auto replacement = m_impl->makeFlowgraph(m_impl->model.state());
        replacement->start();
        m_impl->flowgraph = std::move(replacement);
        m_impl->receptionPausedForPpmCalibration = false;
        return {
            radio::ReceiverError::None,
            true,
            false,
            "Normal reception restored after PPM calibration",
        };
    } catch (const std::exception& error) {
        m_impl->receptionPausedForPpmCalibration = false;
        if (m_impl->model.state().running) {
            static_cast<void>(m_impl->model.stopReception());
        }
        return backendFailure(error);
    } catch (...) {
        m_impl->receptionPausedForPpmCalibration = false;
        if (m_impl->model.state().running) {
            static_cast<void>(m_impl->model.stopReception());
        }
        return unknownBackendFailure();
    }
}

std::optional<radio::OperationResult> GnuRadioReceiverBackend::takeRuntimeError()
{
    auto message = m_impl->runtimeFailure->take();
    if (!message.has_value()) {
        return std::nullopt;
    }
    try {
        m_impl->flowgraph->stopAndWait();
    } catch (const std::exception& error) {
        *message += std::string("; cleanup failed: ") + error.what();
    } catch (...) {
        *message += "; cleanup failed with an unknown error";
    }
    if (m_impl->model.state().running) {
        static_cast<void>(m_impl->model.stopReception());
    }
    return radio::OperationResult{
        radio::ReceiverError::BackendFailure,
        true,
        false,
        std::move(*message),
    };
}

}  // namespace sdr::dsp
