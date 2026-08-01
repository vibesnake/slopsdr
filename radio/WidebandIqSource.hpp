// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sdr::radio {

enum class ReceiverSourceKind {
    Mock,
    Synthetic,
    Hardware,
    RecordedIq,
    RecordedAudio,
};

struct ReceiverSourceCapabilities {
    ReceiverSourceKind kind = ReceiverSourceKind::Mock;
    bool hardwareTuningSupported = false;
    bool gainControlSupported = false;
    bool ppmCorrectionSupported = false;
    bool automaticPpmCalibrationSupported = false;
    bool sampleRateChangeSupported = true;
    bool rfControlsSupported = true;
    bool scannerSupported = true;
    bool iqRecordingSupported = true;
};

struct RecordedIqSourceConfiguration {
    std::string path;
    std::uint64_t centerFrequency = 0;
    std::uint64_t sampleRate = 0;
    std::string format = "cf32_le";
};

enum class RecordingPlaybackState { Unloaded, Stopped, Playing, Paused, Ended, Error };

struct RecordingTransportState {
    RecordingPlaybackState state = RecordingPlaybackState::Unloaded;
    std::uint64_t positionSamples = 0;
    std::uint64_t totalSamples = 0;
    std::uint64_t sampleRate = 0;
    std::string displayName;
    std::string message;
};

struct WidebandIqCaptureMetadata {
    std::uint64_t centerFrequency = 0;
    std::uint64_t effectiveSampleRate = 0;
};

enum class WidebandIqReadStatus {
    Samples,
    Timeout,
    Stopped,
    Disconnected,
    EndOfFile,
    Failed,
};

struct WidebandIqReadResult {
    WidebandIqReadStatus status = WidebandIqReadStatus::Failed;
    std::size_t sampleCount = 0;
    std::string message;
};

struct WidebandIqSourceOperationResult {
    bool succeeded = false;
    bool stateChanged = false;
    std::string message;
};

class WidebandIqSource
{
public:
    virtual ~WidebandIqSource() = default;

    [[nodiscard]] virtual ReceiverSourceCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual WidebandIqCaptureMetadata captureMetadata() const noexcept = 0;
    [[nodiscard]] virtual WidebandIqSourceOperationResult start() = 0;
    [[nodiscard]] virtual WidebandIqSourceOperationResult stop() = 0;
    [[nodiscard]] virtual WidebandIqReadResult read(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout) = 0;
};

}  // namespace sdr::radio
