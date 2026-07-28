// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "DeviceController.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace sdr::devices {

DeviceController::DeviceController(std::unique_ptr<DeviceProvider> provider)
    : m_provider(std::move(provider))
{
}

const std::vector<DeviceDescriptor>& DeviceController::devices() const noexcept
{
    return m_devices;
}

const std::optional<DeviceDescriptor>& DeviceController::selectedDevice() const noexcept
{
    return m_selectedDevice;
}

std::optional<std::uint64_t> DeviceController::centerFrequency() const noexcept
{
    return m_centerFrequency;
}

std::optional<double> DeviceController::ppmCorrection() const noexcept
{
    return m_ppmCorrection;
}

std::optional<std::uint64_t> DeviceController::sampleRate() const noexcept
{
    return m_sampleRate;
}

std::optional<std::uint64_t> DeviceController::effectiveSampleRate() const noexcept
{
    return m_effectiveSampleRate;
}

std::optional<double> DeviceController::gain() const noexcept
{
    return m_gainDb;
}

bool DeviceController::receiveStreamActive() const noexcept
{
    return m_receiveStreamActive;
}

bool DeviceController::rtlSdrTestStreamActive() const noexcept
{
    return m_rtlSdrTestStreamActive;
}

bool DeviceController::rtlSdrBlogV4HfActive() const noexcept
{
    return m_rtlSdrBlogV4HfActive;
}

DeviceOperationResult DeviceController::discover()
{
    try {
        DeviceDiscoveryResult result = m_provider->discover();
        if (!result.succeeded()) {
            return failure(result.error, std::move(result.message));
        }

        m_session.reset();
        m_selectedDevice.reset();
        m_centerFrequency.reset();
        m_ppmCorrection.reset();
        m_sampleRate.reset();
        m_effectiveSampleRate.reset();
        m_gainDb.reset();
        m_receiveStreamActive = false;
        m_rtlSdrTestStreamActive = false;
        m_rtlSdrBlogV4HfActive = false;
        m_devices = std::move(result.devices);
        return {
            DeviceError::None,
            true,
            m_devices.empty() ? "No SDR devices found"
                              : "SDR device discovery completed",
        };
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SDR device discovery failed: %s\n", error.what());
        return failure(
            DeviceError::DiscoveryFailed,
            std::string("SDR device discovery failed: ") + error.what());
    } catch (...) {
        std::fputs("SDR device discovery failed with an unknown error\n", stderr);
        return failure(
            DeviceError::DiscoveryFailed,
            "SDR device discovery failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::selectDevice(const std::string& identifier)
{
    const auto descriptor = std::ranges::find_if(
        m_devices,
        [&identifier](const DeviceDescriptor& device) {
            return device.identifier == identifier;
        });
    if (descriptor == m_devices.end()) {
        return failure(DeviceError::DeviceNotFound, "Requested SDR device was not found");
    }

    try {
        DeviceOpenResult result = m_provider->open(identifier);
        if (!result.succeeded()) {
            return failure(result.error, std::move(result.message));
        }

        DeviceDescriptor selected = *descriptor;
        selected.capabilities = result.session->capabilities();
        m_session = std::move(result.session);
        m_selectedDevice = std::move(selected);
        m_centerFrequency.reset();
        m_ppmCorrection.reset();
        m_sampleRate.reset();
        m_effectiveSampleRate.reset();
        m_gainDb.reset();
        m_receiveStreamActive = false;
        m_rtlSdrTestStreamActive = false;
        m_rtlSdrBlogV4HfActive = false;
        return {DeviceError::None, true, "SDR device selected explicitly"};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Opening the selected SDR device failed: %s\n", error.what());
        return failure(
            DeviceError::DeviceOpenFailed,
            std::string("Opening the selected SDR device failed: ") + error.what());
    } catch (...) {
        std::fputs("Opening the selected SDR device failed with an unknown error\n", stderr);
        return failure(
            DeviceError::DeviceOpenFailed,
            "Opening the selected SDR device failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::clearSelection()
{
    const bool stateChanged = m_session != nullptr || m_selectedDevice.has_value();
    m_session.reset();
    m_selectedDevice.reset();
    m_centerFrequency.reset();
    m_ppmCorrection.reset();
    m_sampleRate.reset();
    m_effectiveSampleRate.reset();
    m_gainDb.reset();
    m_receiveStreamActive = false;
    m_rtlSdrTestStreamActive = false;
    m_rtlSdrBlogV4HfActive = false;
    return {
        DeviceError::None,
        stateChanged,
        stateChanged ? "SDR device selection cleared" : "No SDR device is selected",
    };
}

DeviceOperationResult DeviceController::tuneCenterFrequency(std::uint64_t frequency)
{
    if (!m_session || !m_selectedDevice) {
        return failure(DeviceError::DeviceNotSelected, "Select an SDR device before tuning");
    }

    const DeviceCapabilities& capabilities = m_selectedDevice->capabilities;
    if (!capabilities.receiveFrequencyRanges.empty() &&
        std::ranges::none_of(
            capabilities.receiveFrequencyRanges,
            [frequency](const radio::FrequencyRange& range) {
                return range.contains(frequency);
            })) {
        return failure(
            DeviceError::FrequencyUnsupported,
            "The selected SDR device does not support this center frequency");
    }

    const bool requestsV4Hf =
        capabilities.rtlSdrBlogV4 && frequency < rtlSdrBlogV4HfThresholdHz;
    if (requestsV4Hf && !capabilities.driverManagedHfBelow27Mhz) {
        return failure(
            DeviceError::HfControlUnavailable,
            capabilities.hfLimitation.empty()
                ? "The selected RTL-SDR Blog V4 driver does not expose verified HF support"
                : capabilities.hfLimitation);
    }

    const HfTuningMode mode = requestsV4Hf
                                  ? HfTuningMode::DriverManagedRtlSdrBlogV4
                                  : HfTuningMode::Normal;
    try {
        DeviceOperationResult result = m_session->tuneCenterFrequency(frequency, mode);
        if (!result.succeeded()) {
            return result;
        }

        const bool hfActive = mode == HfTuningMode::DriverManagedRtlSdrBlogV4;
        const bool stateChanged = m_centerFrequency != frequency ||
                                  m_rtlSdrBlogV4HfActive != hfActive ||
                                  result.stateChanged;
        m_centerFrequency = frequency;
        m_rtlSdrBlogV4HfActive = hfActive;
        result.stateChanged = stateChanged;
        return result;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Tuning the selected SDR device failed: %s\n", error.what());
        return failure(
            DeviceError::TuningFailed,
            std::string("Tuning the selected SDR device failed: ") + error.what());
    } catch (...) {
        std::fputs("Tuning the selected SDR device failed with an unknown error\n", stderr);
        return failure(
            DeviceError::TuningFailed,
            "Tuning the selected SDR device failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::setPpmCorrection(double ppmCorrection)
{
    if (!m_session || !m_selectedDevice) {
        return failure(
            DeviceError::DeviceNotSelected,
            "Select an SDR device before applying PPM correction");
    }
    if (!m_selectedDevice->capabilities.ppmCorrectionSupported) {
        return failure(
            DeviceError::PpmCorrectionUnsupported,
            "The selected SDR device does not support PPM correction");
    }

    try {
        DeviceOperationResult result = m_session->setPpmCorrection(ppmCorrection);
        if (!result.succeeded()) {
            return result;
        }
        const double effectivePpm =
            result.effectivePpmCorrection.value_or(ppmCorrection);
        result.stateChanged =
            m_ppmCorrection != effectivePpm || result.stateChanged;
        m_ppmCorrection = effectivePpm;
        return result;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Applying device PPM correction failed: %s\n", error.what());
        return failure(
            DeviceError::PpmCorrectionFailed,
            std::string("Applying device PPM correction failed: ") + error.what());
    } catch (...) {
        std::fputs(
            "Applying device PPM correction failed with an unknown error\n", stderr);
        return failure(
            DeviceError::PpmCorrectionFailed,
            "Applying device PPM correction failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::setSampleRate(std::uint64_t sampleRate)
{
    if (!m_session || !m_selectedDevice) {
        return failure(
            DeviceError::DeviceNotSelected,
            "Select an SDR device before setting its sample rate");
    }
    if (m_receiveStreamActive || m_rtlSdrTestStreamActive) {
        return failure(
            DeviceError::StreamFailed,
            "Stop the SDR receive or test stream before changing its sample rate");
    }
    if (!supportsReceiveSampleRate(m_selectedDevice->capabilities, sampleRate)) {
        return failure(
            DeviceError::SampleRateUnsupported,
            "The selected SDR device does not support this sample rate");
    }

    try {
        auto result = m_session->setSampleRate(sampleRate);
        if (!result.succeeded()) {
            return result;
        }
        if (!result.effectiveSampleRate.has_value()) {
            return failure(
                DeviceError::SampleRateUnsupported,
                "The selected SDR driver did not report its effective sample rate");
        }
        result.stateChanged = m_sampleRate != sampleRate || result.stateChanged;
        m_sampleRate = sampleRate;
        m_effectiveSampleRate = *result.effectiveSampleRate;
        return result;
    } catch (const std::exception& error) {
        return failure(
            DeviceError::SampleRateUnsupported,
            std::string("Setting the SDR sample rate failed: ") + error.what());
    } catch (...) {
        return failure(
            DeviceError::SampleRateUnsupported,
            "Setting the SDR sample rate failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::setGain(double gainDb)
{
    if (!m_session || !m_selectedDevice) {
        return failure(DeviceError::DeviceNotSelected, "Select an SDR device first");
    }
    const auto& capabilities = m_selectedDevice->capabilities;
    if (!std::isfinite(gainDb) || !capabilities.gainSupported ||
        gainDb < capabilities.minimumGainDb ||
        gainDb > capabilities.maximumGainDb) {
        return failure(
            DeviceError::GainUnsupported,
            "The selected SDR device does not support this gain");
    }

    try {
        auto result = m_session->setGain(gainDb);
        if (!result.succeeded()) {
            return result;
        }
        if (!result.effectiveGainDb.has_value() ||
            !std::isfinite(*result.effectiveGainDb)) {
            result.effectiveGainDb = gainDb;
        }
        result.stateChanged = m_gainDb != *result.effectiveGainDb ||
                              result.stateChanged;
        m_gainDb = *result.effectiveGainDb;
        return result;
    } catch (const std::exception& error) {
        return failure(
            DeviceError::GainUnsupported,
            std::string("Setting SDR gain failed: ") + error.what());
    } catch (...) {
        return failure(
            DeviceError::GainUnsupported,
            "Setting SDR gain failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::startReceiveStream()
{
    if (!m_session || !m_selectedDevice) {
        return failure(DeviceError::DeviceNotSelected, "Select an SDR device first");
    }
    if (!m_selectedDevice->capabilities.complexFloat32StreamingSupported) {
        return failure(
            DeviceError::StreamStartFailed,
            "The selected SDR device does not support complex-float receive streaming");
    }
    if (m_receiveStreamActive) {
        return {DeviceError::None, false, "SDR receive stream is already active"};
    }
    if (m_rtlSdrTestStreamActive) {
        return failure(
            DeviceError::StreamStartFailed,
            "Stop RTL-SDR test mode before starting normal reception");
    }
    try {
        auto result = m_session->startReceiveStream();
        if (result.succeeded()) {
            m_receiveStreamActive = true;
            result.stateChanged = true;
        }
        return result;
    } catch (const std::exception& error) {
        return failure(
            DeviceError::StreamStartFailed,
            std::string("Starting the SDR receive stream failed: ") + error.what());
    } catch (...) {
        return failure(
            DeviceError::StreamStartFailed,
            "Starting the SDR receive stream failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::stopReceiveStream()
{
    if (!m_session) {
        return failure(DeviceError::DeviceNotSelected, "No SDR device is selected");
    }
    const bool wasActive = m_receiveStreamActive;
    try {
        auto result = m_session->stopReceiveStream();
        m_receiveStreamActive = false;
        if (result.succeeded()) {
            result.stateChanged = wasActive;
        }
        return result;
    } catch (const std::exception& error) {
        m_receiveStreamActive = false;
        return failure(
            DeviceError::StreamFailed,
            std::string("Stopping the SDR receive stream failed: ") + error.what());
    } catch (...) {
        m_receiveStreamActive = false;
        return failure(
            DeviceError::StreamFailed,
            "Stopping the SDR receive stream failed with an unknown error");
    }
}

DeviceReadResult DeviceController::readReceiveSamples(
    std::span<std::complex<float>> samples,
    std::chrono::milliseconds timeout)
{
    if (!m_session || !m_receiveStreamActive) {
        return {DeviceReadStatus::Stopped, 0, "SDR receive stream is stopped"};
    }
    try {
        auto result = m_session->readReceiveSamples(samples, timeout);
        if (result.status == DeviceReadStatus::Samples &&
            result.sampleCount > samples.size()) {
            m_receiveStreamActive = false;
            return {
                DeviceReadStatus::Failed,
                0,
                "SDR driver returned more samples than the supplied buffer",
            };
        }
        if (result.status == DeviceReadStatus::Disconnected ||
            result.status == DeviceReadStatus::Failed) {
            m_receiveStreamActive = false;
        }
        return result;
    } catch (const std::exception& error) {
        m_receiveStreamActive = false;
        return {
            DeviceReadStatus::Disconnected,
            0,
            std::string("SDR device disconnected while receiving: ") + error.what(),
        };
    } catch (...) {
        m_receiveStreamActive = false;
        return {
            DeviceReadStatus::Disconnected,
            0,
            "SDR device disconnected with an unknown stream error",
        };
    }
}

DeviceOperationResult DeviceController::startRtlSdrTestStream()
{
    if (!m_session || !m_selectedDevice) {
        return failure(DeviceError::DeviceNotSelected, "Select an SDR device first");
    }
    if (!m_selectedDevice->capabilities.rtlSdrTestModeSupported ||
        !m_selectedDevice->capabilities.ppmCorrectionSupported) {
        return failure(
            DeviceError::PpmCalibrationUnsupported,
            "Automatic PPM calibration requires RTL-SDR test mode and frequency correction");
    }
    if (m_receiveStreamActive) {
        return failure(
            DeviceError::PpmCalibrationFailed,
            "Stop the normal SDR receive stream before enabling test mode");
    }
    if (m_rtlSdrTestStreamActive) {
        return {DeviceError::None, false, "RTL-SDR test stream is already active"};
    }
    try {
        auto result = m_session->startRtlSdrTestStream();
        if (result.succeeded()) {
            m_rtlSdrTestStreamActive = true;
            result.stateChanged = true;
        }
        return result;
    } catch (const std::exception& error) {
        return failure(
            DeviceError::PpmCalibrationFailed,
            std::string("Starting RTL-SDR test mode failed: ") + error.what());
    } catch (...) {
        return failure(
            DeviceError::PpmCalibrationFailed,
            "Starting RTL-SDR test mode failed with an unknown error");
    }
}

DeviceOperationResult DeviceController::stopRtlSdrTestStream()
{
    if (!m_session) {
        return failure(DeviceError::DeviceNotSelected, "No SDR device is selected");
    }
    const bool wasActive = m_rtlSdrTestStreamActive;
    try {
        auto result = m_session->stopRtlSdrTestStream();
        m_rtlSdrTestStreamActive = false;
        if (result.succeeded()) {
            result.stateChanged = wasActive;
        }
        return result;
    } catch (const std::exception& error) {
        m_rtlSdrTestStreamActive = false;
        return failure(
            DeviceError::PpmCalibrationFailed,
            std::string("Stopping RTL-SDR test mode failed: ") + error.what());
    } catch (...) {
        m_rtlSdrTestStreamActive = false;
        return failure(
            DeviceError::PpmCalibrationFailed,
            "Stopping RTL-SDR test mode failed with an unknown error");
    }
}

DeviceTestReadResult DeviceController::readRtlSdrTestBytes(
    std::span<std::uint8_t> bytes,
    std::chrono::milliseconds timeout)
{
    if (!m_session || !m_rtlSdrTestStreamActive) {
        return {
            DeviceReadStatus::Stopped,
            0,
            false,
            "RTL-SDR test stream is stopped",
        };
    }
    try {
        auto result = m_session->readRtlSdrTestBytes(bytes, timeout);
        if (result.status == DeviceReadStatus::Samples &&
            result.byteCount > bytes.size()) {
            m_rtlSdrTestStreamActive = false;
            return {
                DeviceReadStatus::Failed,
                0,
                false,
                "SDR driver returned more test bytes than the supplied buffer",
            };
        }
        if (result.status == DeviceReadStatus::Disconnected ||
            result.status == DeviceReadStatus::Failed) {
            m_rtlSdrTestStreamActive = false;
        }
        return result;
    } catch (const std::exception& error) {
        m_rtlSdrTestStreamActive = false;
        return {
            DeviceReadStatus::Disconnected,
            0,
            false,
            std::string("SDR device disconnected during calibration: ") +
                error.what(),
        };
    } catch (...) {
        m_rtlSdrTestStreamActive = false;
        return {
            DeviceReadStatus::Disconnected,
            0,
            false,
            "SDR device disconnected during calibration",
        };
    }
}

DeviceOperationResult DeviceController::failure(
    DeviceError error, std::string message)
{
    return {error, false, std::move(message)};
}

}  // namespace sdr::devices
