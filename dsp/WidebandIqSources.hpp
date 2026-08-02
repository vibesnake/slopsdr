// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "WidebandIqSource.hpp"

#include <chrono>
#include <fstream>
#include <atomic>
#include <memory>
#include <vector>

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

// A single recorded raw capture.  The configuration is deliberately a
// standard-C++ value so the runtime can select it without treating a file as a
// hardware device.
class RecordedIqSource final : public radio::WidebandIqSource
{
public:
    explicit RecordedIqSource(radio::RecordedIqSourceConfiguration configuration);

    [[nodiscard]] static radio::RecordedIqSourceConfiguration resolveConfiguration(
        radio::RecordedIqSourceConfiguration configuration);

    [[nodiscard]] radio::ReceiverSourceCapabilities capabilities() const noexcept override;
    [[nodiscard]] radio::WidebandIqCaptureMetadata captureMetadata() const noexcept override;
    [[nodiscard]] radio::WidebandIqSourceOperationResult start() override;
    [[nodiscard]] radio::WidebandIqSourceOperationResult stop() override;
    [[nodiscard]] radio::WidebandIqReadResult read(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] std::uint64_t sampleCount() const noexcept;
    [[nodiscard]] std::uint64_t positionSamples() const noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool ended() const noexcept;
    void setPaused(bool paused) noexcept;

private:
    radio::RecordedIqSourceConfiguration m_configuration;
    radio::WidebandIqCaptureMetadata m_metadata;
    std::uint64_t m_sampleCount = 0;
    std::atomic<std::uint64_t> m_samplesRead = 0;
    std::vector<unsigned char> m_readBuffer;
    std::ifstream m_file;
    std::chrono::steady_clock::time_point m_nextDeadline;
    std::atomic_bool m_running = false;
    std::atomic_bool m_paused = false;
    std::atomic_bool m_resumeNeedsDeadline = false;
    std::atomic_bool m_ended = false;
};

}  // namespace sdr::dsp
