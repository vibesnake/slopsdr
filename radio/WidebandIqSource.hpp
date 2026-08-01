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
};

struct ReceiverSourceCapabilities {
    ReceiverSourceKind kind = ReceiverSourceKind::Mock;
    bool hardwareTuningSupported = false;
    bool gainControlSupported = false;
    bool ppmCorrectionSupported = false;
    bool automaticPpmCalibrationSupported = false;
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
