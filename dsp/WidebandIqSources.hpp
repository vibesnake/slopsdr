// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "WidebandIqSource.hpp"

#include <chrono>
#include <memory>

namespace sdr::devices {
class DeviceController;
}

namespace sdr::dsp {

class DeviceControllerIqSource final : public radio::WidebandIqSource
{
public:
    DeviceControllerIqSource(
        std::shared_ptr<devices::DeviceController> device,
        radio::WidebandIqCaptureMetadata metadata);

    [[nodiscard]] radio::ReceiverSourceCapabilities capabilities() const noexcept override;
    [[nodiscard]] radio::WidebandIqCaptureMetadata captureMetadata() const noexcept override;
    [[nodiscard]] radio::WidebandIqSourceOperationResult start() override;
    [[nodiscard]] radio::WidebandIqSourceOperationResult stop() override;
    [[nodiscard]] radio::WidebandIqReadResult read(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout) override;

private:
    std::shared_ptr<devices::DeviceController> m_device;
    radio::WidebandIqCaptureMetadata m_metadata;
};

class SyntheticIqSource final : public radio::WidebandIqSource
{
public:
    explicit SyntheticIqSource(radio::WidebandIqCaptureMetadata metadata);

    [[nodiscard]] radio::ReceiverSourceCapabilities capabilities() const noexcept override;
    [[nodiscard]] radio::WidebandIqCaptureMetadata captureMetadata() const noexcept override;
    [[nodiscard]] radio::WidebandIqSourceOperationResult start() override;
    [[nodiscard]] radio::WidebandIqSourceOperationResult stop() override;
    [[nodiscard]] radio::WidebandIqReadResult read(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout) override;

private:
    radio::WidebandIqCaptureMetadata m_metadata;
    std::chrono::steady_clock::time_point m_nextDeadline;
    double m_phase = 0.0;
    bool m_running = false;
};

}  // namespace sdr::dsp
