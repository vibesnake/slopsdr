// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sdr::devices {

inline constexpr std::uint64_t rtlSdrBlogV4HfThresholdHz = 27'000'000;
inline constexpr std::uint64_t rtlSdrBlogV4PracticalMinimumHz = 500'000;

enum class DeviceError {
    None,
    DiscoveryFailed,
    DeviceNotFound,
    DeviceOpenFailed,
    DeviceNotSelected,
    HfControlUnavailable,
    TuningFailed,
    PpmCorrectionUnsupported,
    PpmCorrectionFailed,
    PpmCalibrationUnsupported,
    PpmCalibrationFailed,
    FrequencyUnsupported,
    SampleRateUnsupported,
    GainUnsupported,
    StreamStartFailed,
    StreamFailed,
};

enum class HfTuningMode {
    Normal,
    DriverManagedRtlSdrBlogV4,
};

struct DeviceCapabilities {
    bool receive = false;
    bool rtlSdrBlogV4 = false;
    bool driverManagedHfBelow27Mhz = false;
    std::vector<radio::FrequencyRange> receiveFrequencyRanges;
    std::string hfLimitation;
    bool ppmCorrectionSupported = false;
    bool rtlSdrTestModeSupported = false;
    std::vector<radio::FrequencyRange> receiveSampleRateRanges;
    bool gainSupported = false;
    double minimumGainDb = 0.0;
    double maximumGainDb = 0.0;
    // Zero means the driver reports a continuous gain range.
    double gainStepDb = 0.0;
    bool complexFloat32StreamingSupported = false;
};

// SoapySDR reports both discrete rates and continuous ranges as a list of
// ranges. A single-value range represents a discrete rate; a wider range
// permits a custom requested rate. Keeping this device capability at the
// adapter boundary avoids imposing any RTL-SDR-specific rate table on the
// radio domain.
[[nodiscard]] inline bool supportsReceiveSampleRate(
    const DeviceCapabilities& capabilities, std::uint64_t sampleRate) noexcept
{
    return capabilities.receiveSampleRateRanges.empty() ||
           std::ranges::any_of(
               capabilities.receiveSampleRateRanges,
               [sampleRate](const radio::FrequencyRange& range) {
                   return range.contains(sampleRate);
               });
}

[[nodiscard]] inline bool allowsCustomReceiveSampleRate(
    const DeviceCapabilities& capabilities) noexcept
{
    return std::ranges::any_of(
        capabilities.receiveSampleRateRanges,
        [](const radio::FrequencyRange& range) {
            return range.minimum < range.maximum;
        });
}

enum class DeviceReadStatus {
    Samples,
    Timeout,
    Stopped,
    Disconnected,
    Failed,
};

struct DeviceReadResult {
    DeviceReadStatus status = DeviceReadStatus::Failed;
    std::size_t sampleCount = 0;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == DeviceReadStatus::Samples ||
               status == DeviceReadStatus::Timeout;
    }
};

struct DeviceTestReadResult {
    DeviceReadStatus status = DeviceReadStatus::Failed;
    std::size_t byteCount = 0;
    bool droppedData = false;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == DeviceReadStatus::Samples ||
               status == DeviceReadStatus::Timeout;
    }
};

struct DeviceDescriptor {
    std::string identifier;
    bool identifierIsStable = false;
    std::string displayName;
    std::string driver;
    std::string hardware;
    std::string serial;
    DeviceCapabilities capabilities;
};

struct DeviceOperationResult {
    DeviceOperationResult(
        DeviceError errorValue,
        bool stateChangedValue,
        std::string messageValue,
        std::optional<std::uint64_t> effectiveSampleRateValue = std::nullopt,
        std::optional<double> effectiveGainDbValue = std::nullopt,
        std::optional<double> effectivePpmCorrectionValue = std::nullopt)
        : error(errorValue)
        , stateChanged(stateChangedValue)
        , message(std::move(messageValue))
        , effectiveSampleRate(effectiveSampleRateValue)
        , effectiveGainDb(effectiveGainDbValue)
        , effectivePpmCorrection(effectivePpmCorrectionValue)
    {
    }

    DeviceError error;
    bool stateChanged;
    std::string message;
    std::optional<std::uint64_t> effectiveSampleRate;
    std::optional<double> effectiveGainDb;
    std::optional<double> effectivePpmCorrection;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == DeviceError::None;
    }
};

struct DeviceDiscoveryResult {
    DeviceError error = DeviceError::None;
    std::vector<DeviceDescriptor> devices;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == DeviceError::None;
    }
};

class DeviceSession
{
public:
    virtual ~DeviceSession() = default;

    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual DeviceOperationResult tuneCenterFrequency(
        std::uint64_t frequency, HfTuningMode mode) = 0;
    [[nodiscard]] virtual DeviceOperationResult setPpmCorrection(
        double ppmCorrection) = 0;
    [[nodiscard]] virtual DeviceOperationResult setSampleRate(
        std::uint64_t sampleRate) = 0;
    [[nodiscard]] virtual DeviceOperationResult setGain(double gainDb) = 0;
    [[nodiscard]] virtual DeviceOperationResult startReceiveStream() = 0;
    [[nodiscard]] virtual DeviceOperationResult stopReceiveStream() = 0;
    [[nodiscard]] virtual DeviceReadResult readReceiveSamples(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual DeviceOperationResult startRtlSdrTestStream()
    {
        return {
            DeviceError::PpmCalibrationUnsupported,
            false,
            "RTL-SDR test mode is unsupported",
        };
    }
    [[nodiscard]] virtual DeviceOperationResult stopRtlSdrTestStream()
    {
        return {DeviceError::None, false, "RTL-SDR test mode is stopped"};
    }
    [[nodiscard]] virtual DeviceTestReadResult readRtlSdrTestBytes(
        std::span<std::uint8_t>,
        std::chrono::milliseconds)
    {
        return {
            DeviceReadStatus::Stopped,
            0,
            false,
            "RTL-SDR test mode is stopped",
        };
    }
};

struct DeviceOpenResult {
    DeviceError error = DeviceError::None;
    std::unique_ptr<DeviceSession> session;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == DeviceError::None && session != nullptr;
    }
};

class DeviceProvider
{
public:
    virtual ~DeviceProvider() = default;

    [[nodiscard]] virtual DeviceDiscoveryResult discover() = 0;
    [[nodiscard]] virtual DeviceOpenResult open(const std::string& identifier) = 0;
};

}  // namespace sdr::devices
