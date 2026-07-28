// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "DeviceAccess.hpp"

namespace sdr::devices {

class DeviceController final
{
public:
    explicit DeviceController(std::unique_ptr<DeviceProvider> provider);

    [[nodiscard]] const std::vector<DeviceDescriptor>& devices() const noexcept;
    [[nodiscard]] const std::optional<DeviceDescriptor>& selectedDevice() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> centerFrequency() const noexcept;
    [[nodiscard]] std::optional<double> ppmCorrection() const noexcept;
    // The requested rate is the user/domain value. The effective rate is what
    // the selected driver confirms after applying that request.
    [[nodiscard]] std::optional<std::uint64_t> sampleRate() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> effectiveSampleRate() const noexcept;
    [[nodiscard]] std::optional<double> gain() const noexcept;
    [[nodiscard]] bool receiveStreamActive() const noexcept;
    [[nodiscard]] bool rtlSdrTestStreamActive() const noexcept;
    [[nodiscard]] bool rtlSdrBlogV4HfActive() const noexcept;

    [[nodiscard]] DeviceOperationResult discover();
    [[nodiscard]] DeviceOperationResult selectDevice(const std::string& identifier);
    [[nodiscard]] DeviceOperationResult clearSelection();
    [[nodiscard]] DeviceOperationResult tuneCenterFrequency(std::uint64_t frequency);
    [[nodiscard]] DeviceOperationResult setPpmCorrection(double ppmCorrection);
    [[nodiscard]] DeviceOperationResult setSampleRate(std::uint64_t sampleRate);
    [[nodiscard]] DeviceOperationResult setGain(double gainDb);
    [[nodiscard]] DeviceOperationResult startReceiveStream();
    [[nodiscard]] DeviceOperationResult stopReceiveStream();
    [[nodiscard]] DeviceReadResult readReceiveSamples(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout);
    [[nodiscard]] DeviceOperationResult startRtlSdrTestStream();
    [[nodiscard]] DeviceOperationResult stopRtlSdrTestStream();
    [[nodiscard]] DeviceTestReadResult readRtlSdrTestBytes(
        std::span<std::uint8_t> bytes,
        std::chrono::milliseconds timeout);

private:
    [[nodiscard]] static DeviceOperationResult failure(
        DeviceError error, std::string message);

    std::unique_ptr<DeviceProvider> m_provider;
    std::vector<DeviceDescriptor> m_devices;
    std::optional<DeviceDescriptor> m_selectedDevice;
    std::unique_ptr<DeviceSession> m_session;
    std::optional<std::uint64_t> m_centerFrequency;
    std::optional<double> m_ppmCorrection;
    std::optional<std::uint64_t> m_sampleRate;
    std::optional<std::uint64_t> m_effectiveSampleRate;
    std::optional<double> m_gainDb;
    bool m_receiveStreamActive = false;
    bool m_rtlSdrTestStreamActive = false;
    bool m_rtlSdrBlogV4HfActive = false;
};

}  // namespace sdr::devices
