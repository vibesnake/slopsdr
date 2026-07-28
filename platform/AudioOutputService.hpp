// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "AudioSampleBuffer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sdr::platform {

struct AudioOutputDevice {
    std::string identifier;
    std::string description;
    bool systemDefault = false;

    friend bool operator==(const AudioOutputDevice&, const AudioOutputDevice&) = default;
};

enum class AudioSinkOpenError {
    None,
    NoDevice,
    UnsupportedFormat,
    OpenFailed,
};

struct AudioSinkOpenResult {
    AudioSinkOpenError error = AudioSinkOpenError::None;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == AudioSinkOpenError::None;
    }
};

class AudioSinkBackend
{
public:
    virtual ~AudioSinkBackend() = default;

    [[nodiscard]] virtual std::vector<AudioOutputDevice> devices() = 0;
    [[nodiscard]] virtual AudioSinkOpenResult open(
        const std::string& identifier,
        std::uint32_t sampleRate) = 0;
    [[nodiscard]] virtual AudioSinkOpenResult startPlayback() = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual std::size_t writableFrames() const noexcept = 0;
    [[nodiscard]] virtual std::size_t bufferedFrames() const noexcept = 0;
    [[nodiscard]] virtual std::size_t write(
        std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual std::uint64_t takePlatformUnderrunEvents() = 0;
    [[nodiscard]] virtual std::optional<std::string> takeRuntimeError() = 0;
};

struct AudioOutputState {
    std::vector<AudioOutputDevice> devices;
    std::string selectedDeviceIdentifier;
    std::string statusText = "Audio output is initializing";
    int volumePercent = 75;
    bool muted = false;
    bool ready = false;
    bool running = false;
    std::uint64_t overflowEvents = 0;
    std::uint64_t droppedSamples = 0;
    std::uint64_t underrunEvents = 0;
    std::uint64_t platformUnderrunEvents = 0;
    std::uint64_t enqueuedSamples = 0;
    std::uint64_t writtenSamples = 0;

    friend bool operator==(const AudioOutputState&, const AudioOutputState&) = default;
};

class AudioOutputService final
{
public:
    explicit AudioOutputService(
        std::unique_ptr<AudioSinkBackend> sink,
        std::size_t bufferCapacity = radio::defaultAudioBufferCapacity);
    ~AudioOutputService();

    AudioOutputService(const AudioOutputService&) = delete;
    AudioOutputService& operator=(const AudioOutputService&) = delete;

    void refreshDevices();
    [[nodiscard]] bool selectDevice(const std::string& identifier);
    [[nodiscard]] bool start();
    void stop() noexcept;
    void flush();
    void enqueue(std::span<const float> samples);
    void enqueueMono(std::span<const float> samples);
    void enqueueStereo(std::span<const float> interleavedSamples);
    void reportUpstreamOverflow(std::uint64_t droppedSamples);
    void process();
    [[nodiscard]] bool setVolumePercent(int volumePercent);
    void setMuted(bool muted);

    [[nodiscard]] const AudioOutputState& state() const noexcept;
    [[nodiscard]] std::size_t bufferedSampleCount() const;
    [[nodiscard]] std::size_t sinkBufferedSampleCount() const noexcept;
    [[nodiscard]] std::size_t availableBufferCapacity() const;

private:
    [[nodiscard]] bool openSelectedDevice();
    [[nodiscard]] bool selectedDeviceExists() const;
    void enqueueInterleavedStereo(std::span<const float> samples);
    void setRateLimitedDiagnostic(std::string statusText);

    std::unique_ptr<AudioSinkBackend> m_sink;
    radio::AudioSampleBuffer m_buffer;
    AudioOutputState m_state;
    bool m_receptionActive = false;
    bool m_playbackStarted = false;
    bool m_initialRefreshComplete = false;
    bool m_inUnderrun = false;
    std::vector<float> m_stereoScratch;
    std::vector<std::int16_t> m_pendingPcm;
    std::size_t m_pendingPcmOffsetBytes = 0;
    std::size_t m_pendingWriteRemainderBytes = 0;
    std::optional<std::chrono::steady_clock::time_point> m_lastDiagnostic;
};

[[nodiscard]] std::unique_ptr<AudioOutputService> makeQtAudioOutputService();

}  // namespace sdr::platform
