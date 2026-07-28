// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioOutputService.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sdr::platform {
namespace {

constexpr std::size_t maximumWriteFrames =
    static_cast<std::size_t>(radio::receiverAudioSampleRate / 25);
constexpr std::size_t targetSinkBufferedFrames =
    static_cast<std::size_t>(radio::receiverAudioSampleRate * 3 / 100);
constexpr std::size_t startupPrefillFrames =
    static_cast<std::size_t>(radio::receiverAudioSampleRate * 3 / 100);
constexpr std::size_t maximumWriteAttempts = 8;
constexpr std::size_t audioChannelCount = 2;
constexpr std::size_t audioFrameBytes =
    audioChannelCount * sizeof(std::int16_t);
constexpr auto diagnosticInterval = std::chrono::seconds(1);

const AudioOutputDevice* findDevice(
    const std::vector<AudioOutputDevice>& devices,
    const std::string& identifier)
{
    const auto found = std::ranges::find(
        devices, identifier, &AudioOutputDevice::identifier);
    return found == devices.end() ? nullptr : &*found;
}

}  // namespace

AudioOutputService::AudioOutputService(
    std::unique_ptr<AudioSinkBackend> sink, std::size_t bufferCapacity)
    : m_sink(std::move(sink))
    , m_buffer(bufferCapacity * audioChannelCount)
{
    if (!m_sink) {
        throw std::invalid_argument("Audio output service requires a sink backend");
    }
    m_stereoScratch.reserve(maximumWriteFrames * audioChannelCount);
    m_pendingPcm.reserve(maximumWriteFrames * audioChannelCount);
}

AudioOutputService::~AudioOutputService()
{
    stop();
}

void AudioOutputService::refreshDevices()
{
    const std::string previousSelection = m_state.selectedDeviceIdentifier;
    m_state.devices = m_sink->devices();
    if (!previousSelection.empty() && !selectedDeviceExists()) {
        m_sink->close();
        m_buffer.clear();
        m_pendingPcm.clear();
        m_pendingPcmOffsetBytes = 0;
        m_pendingWriteRemainderBytes = 0;
        m_playbackStarted = false;
        m_state.selectedDeviceIdentifier.clear();
        m_state.ready = false;
        m_state.running = false;
        m_state.statusText = "The selected audio output device disappeared";
        m_inUnderrun = false;
        m_initialRefreshComplete = true;
        return;
    }

    if (!m_initialRefreshComplete && m_state.selectedDeviceIdentifier.empty()) {
        const auto preferred = std::ranges::find(
            m_state.devices, true, &AudioOutputDevice::systemDefault);
        if (preferred != m_state.devices.end()) {
            m_state.selectedDeviceIdentifier = preferred->identifier;
        } else if (!m_state.devices.empty()) {
            m_state.selectedDeviceIdentifier = m_state.devices.front().identifier;
        }
    }
    m_initialRefreshComplete = true;
    m_state.ready = selectedDeviceExists();
    if (m_state.devices.empty()) {
        m_state.statusText = "No audio output device is available";
    } else if (m_state.ready && !m_state.running) {
        m_state.statusText = "Audio output is ready";
    }
}

bool AudioOutputService::selectDevice(const std::string& identifier)
{
    if (!findDevice(m_state.devices, identifier)) {
        m_state.statusText = "Select an available audio output device";
        return false;
    }
    if (identifier == m_state.selectedDeviceIdentifier) {
        return true;
    }

    m_sink->close();
    m_buffer.clear();
    m_pendingPcm.clear();
    m_pendingPcmOffsetBytes = 0;
    m_pendingWriteRemainderBytes = 0;
    m_playbackStarted = false;
    m_inUnderrun = false;
    m_lastDiagnostic.reset();
    m_state.selectedDeviceIdentifier = identifier;
    m_state.ready = true;
    m_state.running = false;
    if (m_receptionActive) {
        return openSelectedDevice();
    }
    m_state.statusText = "Audio output device selected";
    return true;
}

bool AudioOutputService::start()
{
    m_receptionActive = true;
    m_buffer.clear();
    m_pendingPcm.clear();
    m_pendingPcmOffsetBytes = 0;
    m_pendingWriteRemainderBytes = 0;
    m_playbackStarted = false;
    m_inUnderrun = false;
    m_lastDiagnostic.reset();
    if (m_state.running) {
        return true;
    }
    return openSelectedDevice();
}

void AudioOutputService::stop() noexcept
{
    m_receptionActive = false;
    m_sink->close();
    m_buffer.clear();
    m_pendingPcm.clear();
    m_pendingPcmOffsetBytes = 0;
    m_pendingWriteRemainderBytes = 0;
    m_playbackStarted = false;
    m_inUnderrun = false;
    m_lastDiagnostic.reset();
    m_state.running = false;
    if (m_state.ready) {
        m_state.statusText = "Audio output stopped";
    }
}

void AudioOutputService::flush()
{
    m_sink->close();
    m_buffer.clear();
    m_pendingPcm.clear();
    m_pendingPcmOffsetBytes = 0;
    m_pendingWriteRemainderBytes = 0;
    m_playbackStarted = false;
    m_inUnderrun = false;
    if (m_receptionActive) {
        static_cast<void>(openSelectedDevice());
    }
}

void AudioOutputService::enqueue(std::span<const float> samples)
{
    enqueueMono(samples);
}

void AudioOutputService::enqueueMono(std::span<const float> samples)
{
    if (!m_receptionActive || samples.empty()) {
        return;
    }
    const std::size_t availableFrames = availableBufferCapacity();
    if (samples.size() > availableFrames) {
        const std::size_t dropped = samples.size() - availableFrames;
        samples = samples.last(availableFrames);
        ++m_state.overflowEvents;
        m_state.droppedSamples += dropped;
        setRateLimitedDiagnostic(
            "Audio buffer overflow; oldest frames were dropped");
    }
    m_stereoScratch.resize(samples.size() * audioChannelCount);
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        m_stereoScratch[frame * audioChannelCount] = samples[frame];
        m_stereoScratch[frame * audioChannelCount + 1] = samples[frame];
    }
    enqueueInterleavedStereo(m_stereoScratch);
}

void AudioOutputService::enqueueStereo(
    std::span<const float> interleavedSamples)
{
    if (interleavedSamples.size() % audioChannelCount != 0U) {
        throw std::invalid_argument(
            "Stereo audio must contain complete interleaved frames");
    }
    enqueueInterleavedStereo(interleavedSamples);
}

void AudioOutputService::enqueueInterleavedStereo(
    std::span<const float> samples)
{
    if (!m_receptionActive || samples.empty()) {
        return;
    }
    const std::size_t frameCount = samples.size() / audioChannelCount;
    const std::size_t availableFrames = availableBufferCapacity();
    if (frameCount > availableFrames) {
        const std::size_t droppedFrames = frameCount - availableFrames;
        samples = samples.last(availableFrames * audioChannelCount);
        ++m_state.overflowEvents;
        m_state.droppedSamples += droppedFrames;
        setRateLimitedDiagnostic(
            "Audio buffer overflow; oldest frames were dropped");
    }
    const auto result = m_buffer.push(samples);
    m_state.enqueuedSamples += result.acceptedSamples / audioChannelCount;
    if (result.droppedSamples > 0) {
        ++m_state.overflowEvents;
        m_state.droppedSamples +=
            result.droppedSamples / audioChannelCount;
        setRateLimitedDiagnostic(
            "Audio buffer overflow; oldest frames were dropped");
    }
}

void AudioOutputService::reportUpstreamOverflow(std::uint64_t droppedSamples)
{
    if (droppedSamples == 0) {
        return;
    }
    ++m_state.overflowEvents;
    m_state.droppedSamples += droppedSamples;
    setRateLimitedDiagnostic(
        "DSP audio buffer overflow; oldest samples were dropped");
}

void AudioOutputService::process()
{
    if (!m_state.running) {
        return;
    }
    if (auto error = m_sink->takeRuntimeError()) {
        m_sink->close();
        m_buffer.clear();
        m_pendingPcm.clear();
        m_pendingPcmOffsetBytes = 0;
        m_pendingWriteRemainderBytes = 0;
        m_playbackStarted = false;
        m_state.running = false;
        m_state.statusText = "Audio output failed: " + *error;
        return;
    }

    if (!m_playbackStarted) {
        if (m_buffer.size() < startupPrefillFrames * audioChannelCount) {
            return;
        }
        const AudioSinkOpenResult startResult = m_sink->startPlayback();
        if (!startResult.succeeded()) {
            m_sink->close();
            m_buffer.clear();
            m_pendingPcm.clear();
            m_pendingPcmOffsetBytes = 0;
            m_pendingWriteRemainderBytes = 0;
            m_state.running = false;
            m_state.statusText = startResult.message.empty()
                                     ? "Starting audio playback failed"
                                     : startResult.message;
            return;
        }
        m_playbackStarted = true;
        m_state.statusText = "Audio output active at 48 kHz stereo";
    }

    const std::uint64_t platformUnderruns = m_sink->takePlatformUnderrunEvents();
    if (platformUnderruns > 0) {
        m_state.platformUnderrunEvents += platformUnderruns;
        setRateLimitedDiagnostic("Platform audio sink underrun");
    }

    const std::size_t sinkBufferedFrames = m_sink->bufferedFrames();
    if (sinkBufferedFrames >= targetSinkBufferedFrames) {
        return;
    }
    const std::size_t writableFrames = std::min(
        {m_sink->writableFrames(),
         maximumWriteFrames,
         targetSinkBufferedFrames - sinkBufferedFrames});
    if (writableFrames == 0) {
        return;
    }

    if (m_pendingPcmOffsetBytes == m_pendingPcm.size() * sizeof(std::int16_t)) {
        const std::size_t availableFrames = std::min(
            writableFrames, m_buffer.size() / audioChannelCount);
        const auto available =
            m_buffer.take(availableFrames * audioChannelCount);
        if (available.empty()) {
            if (!m_inUnderrun) {
                ++m_state.underrunEvents;
                setRateLimitedDiagnostic(
                    "Audio buffer underrun; silence was inserted");
            }
            m_inUnderrun = true;
        } else {
            m_inUnderrun = false;
        }

        const float gain = m_state.muted
                               ? 0.0F
                               : static_cast<float>(m_state.volumePercent) / 100.0F;
        m_pendingPcm.resize(writableFrames * audioChannelCount);
        for (std::size_t index = 0;
             index < m_pendingPcm.size();
             ++index) {
            const float sample = index < available.size() ? available[index] : 0.0F;
            const float scaled = std::clamp(sample * gain, -1.0F, 1.0F);
            m_pendingPcm[index] = static_cast<std::int16_t>(
                std::lrint(scaled * std::numeric_limits<std::int16_t>::max()));
        }
        m_pendingPcmOffsetBytes = 0;
    }

    std::size_t remainingBytes = std::min(
        writableFrames * audioFrameBytes,
        m_pendingPcm.size() * sizeof(std::int16_t) - m_pendingPcmOffsetBytes);
    for (std::size_t attempt = 0;
         remainingBytes > 0 && attempt < maximumWriteAttempts;
         ++attempt) {
        const auto pending = std::as_bytes(
            std::span<const std::int16_t>(m_pendingPcm))
                                 .subspan(m_pendingPcmOffsetBytes)
                                 .first(remainingBytes);
        const std::size_t writtenBytes = m_sink->write(pending);
        if (writtenBytes > pending.size()) {
            m_sink->close();
            m_buffer.clear();
            m_pendingPcm.clear();
            m_pendingPcmOffsetBytes = 0;
            m_pendingWriteRemainderBytes = 0;
            m_playbackStarted = false;
            m_state.running = false;
            m_state.statusText = "Audio output device returned an invalid write count";
            return;
        }
        if (writtenBytes == 0) {
            return;
        }
        m_pendingPcmOffsetBytes += writtenBytes;
        const std::size_t completeSamples =
            (m_pendingWriteRemainderBytes + writtenBytes) / audioFrameBytes;
        m_pendingWriteRemainderBytes =
            (m_pendingWriteRemainderBytes + writtenBytes) % audioFrameBytes;
        m_state.writtenSamples += completeSamples;
        remainingBytes -= writtenBytes;
    }
}

bool AudioOutputService::setVolumePercent(int volumePercent)
{
    if (volumePercent < 0 || volumePercent > 100) {
        m_state.statusText = "Audio volume must be between 0 and 100 percent";
        return false;
    }
    m_state.volumePercent = volumePercent;
    return true;
}

void AudioOutputService::setMuted(bool muted)
{
    m_state.muted = muted;
}

const AudioOutputState& AudioOutputService::state() const noexcept
{
    return m_state;
}

std::size_t AudioOutputService::bufferedSampleCount() const
{
    const std::size_t pendingBytes =
        m_pendingPcm.size() * sizeof(std::int16_t) - m_pendingPcmOffsetBytes;
    return m_buffer.size() / audioChannelCount +
           (pendingBytes + audioFrameBytes - 1) / audioFrameBytes;
}

std::size_t AudioOutputService::sinkBufferedSampleCount() const noexcept
{
    return m_playbackStarted ? m_sink->bufferedFrames() : 0;
}

std::size_t AudioOutputService::availableBufferCapacity() const
{
    const std::size_t pendingBytes =
        m_pendingPcm.size() * sizeof(std::int16_t) - m_pendingPcmOffsetBytes;
    const std::size_t pendingFrames =
        (pendingBytes + audioFrameBytes - 1) / audioFrameBytes;
    const std::size_t usedFrames =
        m_buffer.size() / audioChannelCount + pendingFrames;
    const std::size_t capacityFrames =
        m_buffer.capacity() / audioChannelCount;
    return usedFrames < capacityFrames
               ? capacityFrames - usedFrames
               : 0;
}

bool AudioOutputService::openSelectedDevice()
{
    m_playbackStarted = false;
    if (!selectedDeviceExists()) {
        m_state.ready = false;
        m_state.running = false;
        m_state.statusText = "No audio output device is selected";
        return false;
    }
    const AudioSinkOpenResult result = m_sink->open(
        m_state.selectedDeviceIdentifier, radio::receiverAudioSampleRate);
    if (!result.succeeded()) {
        m_state.running = false;
        m_state.statusText = result.message.empty()
                                 ? "Opening the audio output device failed"
                                 : result.message;
        return false;
    }
    m_state.ready = true;
    m_state.running = true;
    m_state.statusText = "Audio output prefill at 48 kHz stereo";
    return true;
}

bool AudioOutputService::selectedDeviceExists() const
{
    return findDevice(m_state.devices, m_state.selectedDeviceIdentifier) != nullptr;
}

void AudioOutputService::setRateLimitedDiagnostic(std::string statusText)
{
    const auto now = std::chrono::steady_clock::now();
    if (m_lastDiagnostic.has_value() &&
        now - *m_lastDiagnostic < diagnosticInterval) {
        return;
    }
    m_lastDiagnostic = now;
    m_state.statusText = std::move(statusText);
}

}  // namespace sdr::platform
