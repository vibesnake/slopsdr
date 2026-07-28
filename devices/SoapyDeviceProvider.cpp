// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SoapyDeviceProvider.hpp"

#include "RtlSdrCapabilities.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sdr::devices {
namespace {

constexpr std::size_t rtlSdrCalibrationBufferBytes = 16'384;

std::string valueFor(
    const SoapySDR::Kwargs& arguments, const std::string& key)
{
    const auto value = arguments.find(key);
    return value == arguments.end() ? std::string{} : value->second;
}

std::string firstValueFor(
    const SoapySDR::Kwargs& primary,
    const SoapySDR::Kwargs& secondary,
    const std::string& key)
{
    const std::string primaryValue = valueFor(primary, key);
    return primaryValue.empty() ? valueFor(secondary, key) : primaryValue;
}

std::string humanReadableDescription(const SoapySDR::Kwargs& arguments)
{
    std::string description = valueFor(arguments, "label");
    const std::string manufacturer = valueFor(arguments, "manufacturer");
    const std::string product = valueFor(arguments, "product");
    const std::string serial = valueFor(arguments, "serial");

    if (description.empty()) {
        description = manufacturer;
        if (!product.empty()) {
            if (!description.empty()) {
                description += ' ';
            }
            description += product;
        }
    }
    if (description.empty()) {
        const std::string driver = valueFor(arguments, "driver");
        description = driver.empty() ? "SDR device" : driver + " SDR device";
    }
    if (!serial.empty() && description.find(serial) == std::string::npos) {
        description += " [" + serial + ']';
    }
    return description;
}

std::string baseIdentifier(
    const SoapySDR::Kwargs& arguments, std::size_t enumerationIndex)
{
    const std::string driver = valueFor(arguments, "driver");
    const std::string serial = valueFor(arguments, "serial");
    if (!driver.empty() && !serial.empty()) {
        return driver + ":serial=" + serial;
    }
    return (driver.empty() ? "soapy" : driver) + ":discovery=" +
           std::to_string(enumerationIndex);
}

std::vector<radio::FrequencyRange> convertRanges(
    const SoapySDR::RangeList& ranges)
{
    std::vector<radio::FrequencyRange> converted;
    converted.reserve(ranges.size());
    for (const SoapySDR::Range& range : ranges) {
        if (!std::isfinite(range.minimum()) || !std::isfinite(range.maximum()) ||
            range.minimum() < 0.0 || range.maximum() < range.minimum() ||
            range.maximum() >
                static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            continue;
        }
        converted.push_back({
            static_cast<std::uint64_t>(range.minimum()),
            static_cast<std::uint64_t>(range.maximum()),
        });
    }
    return converted;
}

std::string formatRanges(const std::vector<radio::FrequencyRange>& ranges)
{
    if (ranges.empty()) {
        return "not reported";
    }

    std::string formatted;
    for (const auto& range : ranges) {
        if (!formatted.empty()) {
            formatted += ", ";
        }
        formatted += std::to_string(range.minimum) + "–" +
                     std::to_string(range.maximum) + " Hz";
    }
    return formatted;
}

std::optional<double> advertisedMultiple(const std::string& description)
{
    constexpr std::string_view marker = "multiples of ";
    const auto position = description.find(marker);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    const auto firstDigit = position + marker.size();
    std::size_t parsedCharacters = 0;
    try {
        const double multiple = std::stod(
            description.substr(firstDigit), &parsedCharacters);
        if (parsedCharacters > 0 && std::isfinite(multiple) && multiple > 0.0) {
            return multiple;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::string> streamBufferLength(
    SoapySDR::Device& device, double sampleRate)
{
    constexpr double targetBufferSeconds = 0.030;
    const auto arguments = device.getStreamArgsInfo(SOAPY_SDR_RX, 0);
    const auto argument = std::ranges::find_if(
        arguments,
        [](const SoapySDR::ArgInfo& info) { return info.key == "bufflen"; });
    if (argument == arguments.end() || !std::isfinite(sampleRate) ||
        sampleRate <= 0.0) {
        return std::nullopt;
    }

    double requested = sampleRate * targetBufferSeconds;
    if (argument->units == "bytes") {
        double fullScale = 0.0;
        const std::string nativeFormat = device.getNativeStreamFormat(
            SOAPY_SDR_RX, 0, fullScale);
        requested *= static_cast<double>(SoapySDR::formatToSize(nativeFormat));
    }

    if (!argument->options.empty()) {
        std::optional<double> closest;
        for (const auto& option : argument->options) {
            try {
                const double value = std::stod(option);
                if (std::isfinite(value) && value > 0.0 &&
                    (!closest || std::abs(value - requested) <
                                     std::abs(*closest - requested))) {
                    closest = value;
                }
            } catch (...) {
            }
        }
        if (closest) {
            requested = *closest;
        }
    } else {
        double step = argument->range.step();
        if (!(std::isfinite(step) && step > 0.0)) {
            step = advertisedMultiple(argument->description).value_or(1.0);
        }
        requested = std::ceil(requested / step) * step;
        if (argument->range.maximum() > argument->range.minimum()) {
            requested = std::clamp(
                requested,
                argument->range.minimum(),
                argument->range.maximum());
        }
    }
    if (!std::isfinite(requested) || requested <= 0.0) {
        return std::nullopt;
    }
    return std::to_string(static_cast<std::uint64_t>(std::llround(requested)));
}

void unmakeDevice(SoapySDR::Device* device) noexcept
{
    if (!device) {
        return;
    }
    try {
        SoapySDR::Device::unmake(device);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SoapySDR device cleanup failed: %s\n", error.what());
    } catch (...) {
        std::fputs("SoapySDR device cleanup failed with an unknown error\n", stderr);
    }
}

using SoapyDeviceHandle =
    std::unique_ptr<SoapySDR::Device, decltype(&unmakeDevice)>;

class SoapyDeviceSession final : public DeviceSession
{
public:
    SoapyDeviceSession(
        const SoapySDR::Kwargs& arguments, bool verboseCapabilityLogging)
        : m_device(SoapySDR::Device::make(arguments), &unmakeDevice)
    {
        if (!m_device) {
            throw std::runtime_error("SoapySDR returned no device handle");
        }

        SoapySDR::Kwargs hardwareInfo;
        std::string driver = valueFor(arguments, "driver");
        std::string hardware = valueFor(arguments, "hardware");
        try {
            driver = m_device->getDriverKey();
            hardware = m_device->getHardwareKey();
            hardwareInfo = m_device->getHardwareInfo();
        } catch (const std::exception& error) {
            m_capabilityQueryError = error.what();
            std::fprintf(
                stderr, "SoapySDR identity query failed: %s\n", error.what());
        } catch (...) {
            m_capabilityQueryError = "unknown identity-query error";
            std::fputs("SoapySDR identity query failed with an unknown error\n", stderr);
        }

        std::vector<radio::FrequencyRange> overallFrequencyRanges;
        std::vector<radio::FrequencyRange> rfFrequencyRanges;
        std::vector<radio::FrequencyRange> correctionFrequencyRanges;
        std::vector<radio::FrequencyRange> sampleRateRanges;
        bool receive = false;
        bool ppmCorrectionSupported = false;
        bool rtlSdrTestModeSupported = false;
        bool gainSupported = false;
        double minimumGainDb = 0.0;
        double maximumGainDb = 0.0;
        double gainStepDb = 0.0;
        bool complexFloat32StreamingSupported = false;
        try {
            receive = m_device->getNumChannels(SOAPY_SDR_RX) > 0;
            if (receive) {
                overallFrequencyRanges = convertRanges(
                    m_device->getFrequencyRange(SOAPY_SDR_RX, 0));
                rfFrequencyRanges = convertRanges(
                    m_device->getFrequencyRange(SOAPY_SDR_RX, 0, "RF"));
                try {
                    correctionFrequencyRanges = convertRanges(
                        m_device->getFrequencyRange(SOAPY_SDR_RX, 0, "CORR"));
                } catch (const std::exception&) {
                    // CORR is optional and never substitutes for RF capability.
                }
                sampleRateRanges = convertRanges(
                    m_device->getSampleRateRange(SOAPY_SDR_RX, 0));
                ppmCorrectionSupported =
                    m_device->hasFrequencyCorrection(SOAPY_SDR_RX, 0);
                const auto formats = m_device->getStreamFormats(SOAPY_SDR_RX, 0);
                complexFloat32StreamingSupported =
                    std::ranges::find(formats, SOAPY_SDR_CF32) != formats.end();
                const bool complexInt8StreamingSupported =
                    std::ranges::find(formats, SOAPY_SDR_CS8) != formats.end();
                if (isRtlSdrDriver(driver) &&
                    complexInt8StreamingSupported) {
                    try {
                        const auto settingInfo = m_device->getSettingInfo();
                        rtlSdrTestModeSupported = std::ranges::any_of(
                            settingInfo,
                            [](const SoapySDR::ArgInfo& setting) {
                                return setting.key == "testmode";
                            });
                    } catch (const std::exception&) {
                        // Test mode is optional and must be explicitly exposed.
                    }
                }
                const auto gains = m_device->listGains(SOAPY_SDR_RX, 0);
                if (!gains.empty()) {
                    const auto gainRange = m_device->getGainRange(SOAPY_SDR_RX, 0);
                    gainSupported = std::isfinite(gainRange.minimum()) &&
                                    std::isfinite(gainRange.maximum()) &&
                                    gainRange.minimum() <= gainRange.maximum();
                    minimumGainDb = gainRange.minimum();
                    maximumGainDb = gainRange.maximum();
                    gainStepDb = std::isfinite(gainRange.step()) &&
                                         gainRange.step() > 0.0
                                     ? gainRange.step()
                                     : 0.0;
                }
            }
        } catch (const std::exception& error) {
            m_capabilityQueryError = error.what();
            std::fprintf(
                stderr, "SoapySDR receive capability query failed: %s\n", error.what());
        } catch (...) {
            m_capabilityQueryError = "unknown receive-capability-query error";
            std::fputs(
                "SoapySDR receive capability query failed with an unknown error\n",
                stderr);
        }

        const RtlSdrIdentity identity{
            driver,
            hardware,
            firstValueFor(arguments, hardwareInfo, "manufacturer"),
            firstValueFor(arguments, hardwareInfo, "product"),
            firstValueFor(arguments, hardwareInfo, "tuner"),
        };
        m_capabilities = detectRtlSdrCapabilities(identity, rfFrequencyRanges);
        m_capabilities.receive = receive;
        m_capabilities.ppmCorrectionSupported = ppmCorrectionSupported;
        m_capabilities.rtlSdrTestModeSupported = rtlSdrTestModeSupported;
        m_capabilities.receiveSampleRateRanges = std::move(sampleRateRanges);
        m_capabilities.gainSupported = gainSupported;
        m_capabilities.minimumGainDb = minimumGainDb;
        m_capabilities.maximumGainDb = maximumGainDb;
        m_capabilities.gainStepDb = gainStepDb;
        m_capabilities.complexFloat32StreamingSupported =
            complexFloat32StreamingSupported;
        if (!m_capabilityQueryError.empty()) {
            if (!m_capabilities.hfLimitation.empty()) {
                m_capabilities.hfLimitation += "; ";
            }
            m_capabilities.hfLimitation +=
                "SoapySDR capability query failed: " + m_capabilityQueryError;
        }
        if (verboseCapabilityLogging) {
            std::fprintf(
                stderr,
                "SoapySDR opened capability ranges: overall=%s; RF=%s; CORR=%s; "
                "selected practical RF=%s; V4 HF=%s\n",
                formatRanges(overallFrequencyRanges).c_str(),
                formatRanges(rfFrequencyRanges).c_str(),
                formatRanges(correctionFrequencyRanges).c_str(),
                formatRanges(m_capabilities.receiveFrequencyRanges).c_str(),
                m_capabilities.driverManagedHfBelow27Mhz
                    ? "enabled (confirmed RTL-SDR Blog V4 RF reaches below 500 kHz)"
                    : (m_capabilities.rtlSdrBlogV4
                           ? "disabled (RF does not reach below 500 kHz)"
                           : "not applicable"));
        }
    }

    ~SoapyDeviceSession() override
    {
        static_cast<void>(stopRtlSdrTestStream());
        static_cast<void>(stopReceiveStream());
    }

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override
    {
        return m_capabilities;
    }

    [[nodiscard]] DeviceOperationResult tuneCenterFrequency(
        std::uint64_t frequency, HfTuningMode mode) override
    {
        try {
            m_device->setFrequency(
                SOAPY_SDR_RX, 0, static_cast<double>(frequency));
            return {
                DeviceError::None,
                true,
                mode == HfTuningMode::DriverManagedRtlSdrBlogV4
                    ? "RTL-SDR Blog V4 tuned using driver-managed HF conversion"
                    : "SDR device center frequency changed",
            };
        } catch (const std::exception& error) {
            std::fprintf(stderr, "SoapySDR tuning failed: %s\n", error.what());
            return {
                DeviceError::TuningFailed,
                false,
                std::string("SoapySDR tuning failed: ") + error.what(),
            };
        } catch (...) {
            std::fputs("SoapySDR tuning failed with an unknown error\n", stderr);
            return {
                DeviceError::TuningFailed,
                false,
                "SoapySDR tuning failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult setPpmCorrection(
        double ppmCorrection) override
    {
        if (!m_capabilities.ppmCorrectionSupported) {
            return {
                DeviceError::PpmCorrectionUnsupported,
                false,
                "The SoapySDR device does not report frequency-correction support",
            };
        }
        try {
            m_device->setFrequencyCorrection(SOAPY_SDR_RX, 0, ppmCorrection);
            const double effectivePpm =
                m_device->getFrequencyCorrection(SOAPY_SDR_RX, 0);
            if (!std::isfinite(effectivePpm)) {
                return {
                    DeviceError::PpmCorrectionFailed,
                    false,
                    "SoapySDR returned an invalid effective PPM correction",
                };
            }
            return {
                DeviceError::None,
                true,
                "SDR device PPM correction applied",
                std::nullopt,
                std::nullopt,
                effectivePpm,
            };
        } catch (const std::exception& error) {
            std::fprintf(stderr, "SoapySDR PPM correction failed: %s\n", error.what());
            return {
                DeviceError::PpmCorrectionFailed,
                false,
                std::string("SoapySDR PPM correction failed: ") + error.what(),
            };
        } catch (...) {
            std::fputs(
                "SoapySDR PPM correction failed with an unknown error\n", stderr);
            return {
                DeviceError::PpmCorrectionFailed,
                false,
                "SoapySDR PPM correction failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult setSampleRate(
        std::uint64_t sampleRate) override
    {
        try {
            m_device->setSampleRate(
                SOAPY_SDR_RX, 0, static_cast<double>(sampleRate));
            const double effectiveRate = m_device->getSampleRate(SOAPY_SDR_RX, 0);
            if (!std::isfinite(effectiveRate) || effectiveRate <= 0.0 ||
                effectiveRate >
                    static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
                return {
                    DeviceError::SampleRateUnsupported,
                    false,
                    "SoapySDR returned an invalid effective sample rate",
                };
            }
            m_effectiveSampleRate = effectiveRate;
            return {
                DeviceError::None,
                true,
                "SDR device sample rate applied",
                static_cast<std::uint64_t>(std::llround(effectiveRate)),
            };
        } catch (const std::exception& error) {
            return {
                DeviceError::SampleRateUnsupported,
                false,
                std::string("SoapySDR sample-rate configuration failed: ") +
                    error.what(),
            };
        } catch (...) {
            return {
                DeviceError::SampleRateUnsupported,
                false,
                "SoapySDR sample-rate configuration failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult setGain(double gainDb) override
    {
        if (!m_capabilities.gainSupported) {
            return {
                DeviceError::GainUnsupported,
                false,
                "The SoapySDR device does not report adjustable receive gain",
            };
        }
        try {
            m_device->setGain(SOAPY_SDR_RX, 0, gainDb);
            const double effectiveGain = m_device->getGain(SOAPY_SDR_RX, 0);
            if (!std::isfinite(effectiveGain)) {
                return {
                    DeviceError::GainUnsupported,
                    false,
                    "SoapySDR returned an invalid effective gain",
                };
            }
            return {
                DeviceError::None,
                true,
                "SDR device gain applied",
                std::nullopt,
                effectiveGain,
            };
        } catch (const std::exception& error) {
            return {
                DeviceError::GainUnsupported,
                false,
                std::string("SoapySDR gain configuration failed: ") + error.what(),
            };
        } catch (...) {
            return {
                DeviceError::GainUnsupported,
                false,
                "SoapySDR gain configuration failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult startReceiveStream() override
    {
        if (m_streamActive) {
            return {DeviceError::None, false, "SoapySDR receive stream is active"};
        }
        try {
            SoapySDR::Kwargs streamArguments;
            if (const auto bufferLength =
                    streamBufferLength(*m_device, m_effectiveSampleRate)) {
                streamArguments.emplace("bufflen", *bufferLength);
            }
            m_stream = m_device->setupStream(
                SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, streamArguments);
            if (!m_stream) {
                return {
                    DeviceError::StreamStartFailed,
                    false,
                    "SoapySDR returned no receive stream",
                };
            }
            const int activation = m_device->activateStream(m_stream);
            if (activation < 0) {
                const std::string message = std::string("SoapySDR stream activation failed: ") +
                                            SoapySDR::errToStr(activation);
                closeReceiveStream();
                return {DeviceError::StreamStartFailed, false, message};
            }
            m_streamActive = true;
            return {DeviceError::None, true, "SoapySDR receive stream started"};
        } catch (const std::exception& error) {
            closeReceiveStream();
            return {
                DeviceError::StreamStartFailed,
                false,
                std::string("SoapySDR stream setup failed: ") + error.what(),
            };
        } catch (...) {
            closeReceiveStream();
            return {
                DeviceError::StreamStartFailed,
                false,
                "SoapySDR stream setup failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult stopReceiveStream() override
    {
        if (!m_stream) {
            return {DeviceError::None, false, "SoapySDR receive stream is stopped"};
        }
        try {
            if (m_streamActive) {
                const int result = m_device->deactivateStream(m_stream);
                if (result < 0 && result != SOAPY_SDR_NOT_SUPPORTED) {
                    const std::string message =
                        std::string("SoapySDR stream deactivation failed: ") +
                        SoapySDR::errToStr(result);
                    closeReceiveStream();
                    return {DeviceError::StreamFailed, false, message};
                }
            }
            closeReceiveStream();
            return {DeviceError::None, true, "SoapySDR receive stream stopped"};
        } catch (const std::exception& error) {
            closeReceiveStream();
            return {
                DeviceError::StreamFailed,
                false,
                std::string("SoapySDR stream shutdown failed: ") + error.what(),
            };
        } catch (...) {
            closeReceiveStream();
            return {
                DeviceError::StreamFailed,
                false,
                "SoapySDR stream shutdown failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceReadResult readReceiveSamples(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds timeout) override
    {
        if (!m_stream || !m_streamActive) {
            return {DeviceReadStatus::Stopped, 0, "SoapySDR receive stream is stopped"};
        }
        void* buffers[]{samples.data()};
        int flags = 0;
        long long timeNanoseconds = 0;
        const auto timeoutMicroseconds = static_cast<long>(timeout.count() * 1'000);
        try {
            const int result = m_device->readStream(
                m_stream,
                buffers,
                samples.size(),
                flags,
                timeNanoseconds,
                timeoutMicroseconds);
            if (result > 0) {
                return {
                    DeviceReadStatus::Samples,
                    static_cast<std::size_t>(result),
                    {},
                };
            }
            if (result == SOAPY_SDR_TIMEOUT || result == SOAPY_SDR_OVERFLOW) {
                return {DeviceReadStatus::Timeout, 0, {}};
            }
            m_streamActive = false;
            return {
                DeviceReadStatus::Disconnected,
                0,
                std::string("SoapySDR receive failed or device disconnected: ") +
                    SoapySDR::errToStr(result),
            };
        } catch (const std::exception& error) {
            m_streamActive = false;
            return {
                DeviceReadStatus::Disconnected,
                0,
                std::string("SoapySDR device disconnected while receiving: ") +
                    error.what(),
            };
        } catch (...) {
            m_streamActive = false;
            return {
                DeviceReadStatus::Disconnected,
                0,
                "SoapySDR device disconnected with an unknown stream error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult startRtlSdrTestStream() override
    {
        if (!m_capabilities.rtlSdrTestModeSupported ||
            !m_capabilities.ppmCorrectionSupported) {
            return {
                DeviceError::PpmCalibrationUnsupported,
                false,
                "SoapySDR does not expose RTL-SDR test mode and frequency correction",
            };
        }
        if (m_stream || m_streamActive) {
            return {
                DeviceError::PpmCalibrationFailed,
                false,
                "The normal SDR receive stream must be stopped before calibration",
            };
        }
        if (m_testStreamActive) {
            return {DeviceError::None, false, "RTL-SDR test stream is active"};
        }
        try {
            // Activating a newly created stream resets the RTL-SDR driver's
            // USB buffer before any counter bytes are measured.
            m_device->writeSetting("testmode", "true");
            m_testModeEnabled = true;
            m_testStream = m_device->setupStream(
                SOAPY_SDR_RX,
                SOAPY_SDR_CS8,
                {0},
                {
                    {"bufflen", std::to_string(rtlSdrCalibrationBufferBytes)},
                    {"buffers", "2"},
                });
            if (!m_testStream) {
                static_cast<void>(stopRtlSdrTestStream());
                return {
                    DeviceError::PpmCalibrationFailed,
                    false,
                    "SoapySDR returned no RTL-SDR test stream",
                };
            }
            const int activation = m_device->activateStream(m_testStream);
            if (activation < 0) {
                const std::string message =
                    std::string("SoapySDR test-stream activation failed: ") +
                    SoapySDR::errToStr(activation);
                static_cast<void>(stopRtlSdrTestStream());
                return {DeviceError::PpmCalibrationFailed, false, message};
            }
            m_testStreamActive = true;
            return {
                DeviceError::None,
                true,
                "RTL-SDR test mode enabled and stream buffer reset",
            };
        } catch (const std::exception& error) {
            static_cast<void>(stopRtlSdrTestStream());
            return {
                DeviceError::PpmCalibrationFailed,
                false,
                std::string("Enabling RTL-SDR test mode failed: ") + error.what(),
            };
        } catch (...) {
            static_cast<void>(stopRtlSdrTestStream());
            return {
                DeviceError::PpmCalibrationFailed,
                false,
                "Enabling RTL-SDR test mode failed with an unknown error",
            };
        }
    }

    [[nodiscard]] DeviceOperationResult stopRtlSdrTestStream() override
    {
        std::string failure;
        if (m_testStream) {
            try {
                if (m_testStreamActive) {
                    const int result = m_device->deactivateStream(m_testStream);
                    if (result < 0 && result != SOAPY_SDR_NOT_SUPPORTED) {
                        failure =
                            std::string("SoapySDR test-stream deactivation failed: ") +
                            SoapySDR::errToStr(result);
                    }
                }
                m_device->closeStream(m_testStream);
            } catch (const std::exception& error) {
                failure = std::string("SoapySDR test-stream cleanup failed: ") +
                          error.what();
            } catch (...) {
                failure = "SoapySDR test-stream cleanup failed";
            }
        }
        m_testStream = nullptr;
        m_testStreamActive = false;
        if (m_testModeEnabled) {
            try {
                m_device->writeSetting("testmode", "false");
                m_testModeEnabled = false;
            } catch (const std::exception& error) {
                if (!failure.empty()) {
                    failure += "; ";
                }
                failure += std::string("Disabling RTL-SDR test mode failed: ") +
                           error.what();
            } catch (...) {
                if (!failure.empty()) {
                    failure += "; ";
                }
                failure += "Disabling RTL-SDR test mode failed";
            }
        }
        if (!failure.empty()) {
            return {DeviceError::PpmCalibrationFailed, true, std::move(failure)};
        }
        return {DeviceError::None, true, "RTL-SDR test mode disabled"};
    }

    [[nodiscard]] DeviceTestReadResult readRtlSdrTestBytes(
        std::span<std::uint8_t> bytes,
        std::chrono::milliseconds timeout) override
    {
        if (!m_testStream || !m_testStreamActive || bytes.size() < 2) {
            return {
                DeviceReadStatus::Stopped,
                0,
                false,
                "RTL-SDR test stream is stopped",
            };
        }
        const std::size_t requestedComplexSamples = bytes.size() / 2;
        void* buffers[]{bytes.data()};
        int flags = 0;
        long long timeNanoseconds = 0;
        const auto timeoutMicroseconds =
            static_cast<long>(timeout.count() * 1'000);
        try {
            const int result = m_device->readStream(
                m_testStream,
                buffers,
                requestedComplexSamples,
                flags,
                timeNanoseconds,
                timeoutMicroseconds);
            if (result > 0) {
                bool droppedData = false;
                std::size_t channelMask = 0;
                int statusFlags = 0;
                long long statusTimeNanoseconds = 0;
                const int streamStatus = m_device->readStreamStatus(
                    m_testStream,
                    channelMask,
                    statusFlags,
                    statusTimeNanoseconds,
                    0);
                if (streamStatus == SOAPY_SDR_OVERFLOW) {
                    droppedData = true;
                } else if (streamStatus < 0 &&
                           streamStatus != SOAPY_SDR_TIMEOUT &&
                           streamStatus != SOAPY_SDR_NOT_SUPPORTED) {
                    m_testStreamActive = false;
                    return {
                        DeviceReadStatus::Disconnected,
                        0,
                        false,
                        std::string("SoapySDR test-stream status failed: ") +
                            SoapySDR::errToStr(streamStatus),
                    };
                }
                return {
                    DeviceReadStatus::Samples,
                    static_cast<std::size_t>(result) * 2,
                    droppedData,
                    {},
                };
            }
            if (result == SOAPY_SDR_TIMEOUT) {
                return {DeviceReadStatus::Timeout, 0, false, {}};
            }
            if (result == SOAPY_SDR_OVERFLOW) {
                return {
                    DeviceReadStatus::Timeout,
                    0,
                    true,
                    "SoapySDR reported calibration-stream overflow",
                };
            }
            m_testStreamActive = false;
            return {
                DeviceReadStatus::Disconnected,
                0,
                false,
                std::string("SoapySDR calibration read failed: ") +
                    SoapySDR::errToStr(result),
            };
        } catch (const std::exception& error) {
            m_testStreamActive = false;
            return {
                DeviceReadStatus::Disconnected,
                0,
                false,
                std::string("SoapySDR disconnected during calibration: ") +
                    error.what(),
            };
        } catch (...) {
            m_testStreamActive = false;
            return {
                DeviceReadStatus::Disconnected,
                0,
                false,
                "SoapySDR disconnected during calibration",
            };
        }
    }

private:
    void closeReceiveStream() noexcept
    {
        if (!m_stream) {
            m_streamActive = false;
            return;
        }
        try {
            m_device->closeStream(m_stream);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "SoapySDR stream cleanup failed: %s\n", error.what());
        } catch (...) {
            std::fputs("SoapySDR stream cleanup failed\n", stderr);
        }
        m_stream = nullptr;
        m_streamActive = false;
    }

    SoapyDeviceHandle m_device;
    SoapySDR::Stream* m_stream = nullptr;
    SoapySDR::Stream* m_testStream = nullptr;
    bool m_streamActive = false;
    bool m_testStreamActive = false;
    bool m_testModeEnabled = false;
    double m_effectiveSampleRate = 0.0;
    DeviceCapabilities m_capabilities;
    std::string m_capabilityQueryError;
};

}  // namespace

class SoapyDeviceProvider::Impl final
{
public:
    explicit Impl(bool enableVerboseCapabilityLogging)
        : verboseCapabilityLogging(enableVerboseCapabilityLogging)
    {
    }

    std::unordered_map<std::string, SoapySDR::Kwargs> argumentsByIdentifier;
    bool verboseCapabilityLogging = false;
};

SoapyDeviceProvider::SoapyDeviceProvider(bool verboseCapabilityLogging)
    : m_impl(std::make_unique<Impl>(verboseCapabilityLogging))
{
}

SoapyDeviceProvider::~SoapyDeviceProvider() = default;

DeviceDiscoveryResult SoapyDeviceProvider::discover()
{
    try {
        const SoapySDR::KwargsList enumerated = SoapySDR::Device::enumerate();
        std::unordered_map<std::string, SoapySDR::Kwargs> newArguments;
        std::vector<DeviceDescriptor> devices;
        devices.reserve(enumerated.size());

        for (std::size_t index = 0; index < enumerated.size(); ++index) {
            const SoapySDR::Kwargs& arguments = enumerated[index];
            std::string identifier = baseIdentifier(arguments, index);
            bool identifierIsStable = !valueFor(arguments, "driver").empty() &&
                                      !valueFor(arguments, "serial").empty();
            if (newArguments.contains(identifier)) {
                identifier += ":duplicate=" + std::to_string(index);
                identifierIsStable = false;
            }

            const RtlSdrIdentity identity{
                valueFor(arguments, "driver"),
                valueFor(arguments, "hardware"),
                valueFor(arguments, "manufacturer"),
                valueFor(arguments, "product"),
                valueFor(arguments, "tuner"),
            };
            DeviceCapabilities capabilities = detectRtlSdrCapabilities(identity, {});
            capabilities.receive = true;
            if (capabilities.rtlSdrBlogV4) {
                capabilities.hfLimitation =
                    "Select this device explicitly to verify driver-managed V4 HF support";
            }

            devices.push_back({
                identifier,
                identifierIsStable,
                humanReadableDescription(arguments),
                valueFor(arguments, "driver"),
                valueFor(arguments, "hardware"),
                valueFor(arguments, "serial"),
                std::move(capabilities),
            });
            newArguments.emplace(identifier, arguments);
        }

        m_impl->argumentsByIdentifier = std::move(newArguments);
        return {
            DeviceError::None,
            std::move(devices),
            enumerated.empty() ? "No SoapySDR devices found"
                               : "SoapySDR device discovery completed",
        };
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SoapySDR device discovery failed: %s\n", error.what());
        return {
            DeviceError::DiscoveryFailed,
            {},
            std::string("SoapySDR device discovery failed: ") + error.what(),
        };
    } catch (...) {
        std::fputs("SoapySDR device discovery failed with an unknown error\n", stderr);
        return {
            DeviceError::DiscoveryFailed,
            {},
            "SoapySDR device discovery failed with an unknown error",
        };
    }
}

DeviceOpenResult SoapyDeviceProvider::open(const std::string& identifier)
{
    const auto arguments = m_impl->argumentsByIdentifier.find(identifier);
    if (arguments == m_impl->argumentsByIdentifier.end()) {
        return {
            DeviceError::DeviceNotFound,
            nullptr,
            "The requested SoapySDR device is not in the current discovery result",
        };
    }

    try {
        return {
            DeviceError::None,
            std::make_unique<SoapyDeviceSession>(
                arguments->second, m_impl->verboseCapabilityLogging),
            "SoapySDR device opened after explicit selection",
        };
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Opening the SoapySDR device failed: %s\n", error.what());
        return {
            DeviceError::DeviceOpenFailed,
            nullptr,
            std::string("Opening the SoapySDR device failed: ") + error.what(),
        };
    } catch (...) {
        std::fputs("Opening the SoapySDR device failed with an unknown error\n", stderr);
        return {
            DeviceError::DeviceOpenFailed,
            nullptr,
            "Opening the SoapySDR device failed with an unknown error",
        };
    }
}

}  // namespace sdr::devices
