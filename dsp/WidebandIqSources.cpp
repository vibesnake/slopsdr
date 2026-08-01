// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WidebandIqSources.hpp"

#include "DeviceController.hpp"

#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sdr::dsp {
namespace {

constexpr double syntheticToneOffsetHz = 100'000.0;
constexpr double syntheticToneAmplitude = 0.5;
constexpr double twoPi = 6.28318530717958647692;

radio::WidebandIqSourceOperationResult operation(
    const devices::DeviceOperationResult& result)
{
    return {
        .succeeded = result.succeeded(),
        .stateChanged = result.stateChanged,
        .message = result.message,
    };
}

radio::WidebandIqReadStatus readStatus(devices::DeviceReadStatus status) noexcept
{
    switch (status) {
    case devices::DeviceReadStatus::Samples:
        return radio::WidebandIqReadStatus::Samples;
    case devices::DeviceReadStatus::Timeout:
        return radio::WidebandIqReadStatus::Timeout;
    case devices::DeviceReadStatus::Stopped:
        return radio::WidebandIqReadStatus::Stopped;
    case devices::DeviceReadStatus::Disconnected:
        return radio::WidebandIqReadStatus::Disconnected;
    case devices::DeviceReadStatus::Failed:
        return radio::WidebandIqReadStatus::Failed;
    }
    return radio::WidebandIqReadStatus::Failed;
}

}  // namespace

DeviceControllerIqSource::DeviceControllerIqSource(
    std::shared_ptr<devices::DeviceController> device,
    radio::WidebandIqCaptureMetadata metadata)
    : m_device(std::move(device))
    , m_metadata(metadata)
{
    if (!m_device || !m_device->selectedDevice().has_value()) {
        throw std::invalid_argument("Hardware IQ source requires a selected device");
    }
}

radio::ReceiverSourceCapabilities DeviceControllerIqSource::capabilities() const noexcept
{
    const auto& capabilities = m_device->selectedDevice()->capabilities;
    return {
        .kind = radio::ReceiverSourceKind::Hardware,
        .hardwareTuningSupported = true,
        .gainControlSupported = capabilities.gainSupported,
        .ppmCorrectionSupported = capabilities.ppmCorrectionSupported,
        .automaticPpmCalibrationSupported =
            capabilities.ppmCorrectionSupported && capabilities.rtlSdrTestModeSupported,
    };
}

radio::WidebandIqCaptureMetadata DeviceControllerIqSource::captureMetadata() const noexcept
{
    return m_metadata;
}

radio::WidebandIqSourceOperationResult DeviceControllerIqSource::start()
{
    return operation(m_device->startReceiveStream());
}

radio::WidebandIqSourceOperationResult DeviceControllerIqSource::stop()
{
    return operation(m_device->stopReceiveStream());
}

radio::WidebandIqReadResult DeviceControllerIqSource::read(
    std::span<std::complex<float>> samples,
    std::chrono::milliseconds timeout)
{
    const auto result = m_device->readReceiveSamples(samples, timeout);
    return {
        .status = readStatus(result.status),
        .sampleCount = result.sampleCount,
        .message = result.message,
    };
}

SyntheticIqSource::SyntheticIqSource(radio::WidebandIqCaptureMetadata metadata)
    : m_metadata(metadata)
{
    if (m_metadata.effectiveSampleRate == 0) {
        throw std::invalid_argument("Synthetic IQ source requires a positive sample rate");
    }
}

radio::ReceiverSourceCapabilities SyntheticIqSource::capabilities() const noexcept
{
    return {.kind = radio::ReceiverSourceKind::Synthetic};
}

radio::WidebandIqCaptureMetadata SyntheticIqSource::captureMetadata() const noexcept
{
    return m_metadata;
}

radio::WidebandIqSourceOperationResult SyntheticIqSource::start()
{
    if (m_running) {
        return {true, false, "Synthetic IQ source is already active"};
    }
    m_phase = 0.0;
    m_nextDeadline = std::chrono::steady_clock::now();
    m_running = true;
    return {true, true, "Synthetic IQ source started"};
}

radio::WidebandIqSourceOperationResult SyntheticIqSource::stop()
{
    if (!m_running) {
        return {true, false, "Synthetic IQ source is already stopped"};
    }
    m_running = false;
    return {true, true, "Synthetic IQ source stopped"};
}

radio::WidebandIqReadResult SyntheticIqSource::read(
    std::span<std::complex<float>> samples,
    std::chrono::milliseconds timeout)
{
    static_cast<void>(timeout);
    if (!m_running) {
        return {radio::WidebandIqReadStatus::Stopped, 0, "Synthetic IQ source is stopped"};
    }
    if (samples.empty()) {
        return {radio::WidebandIqReadStatus::Failed, 0, "Synthetic IQ source received an empty buffer"};
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_nextDeadline > now) {
        std::this_thread::sleep_until(m_nextDeadline);
    }
    const double phaseStep = twoPi * syntheticToneOffsetHz /
                             static_cast<double>(m_metadata.effectiveSampleRate);
    for (auto& sample : samples) {
        sample = std::complex<float>(
            static_cast<float>(syntheticToneAmplitude * std::cos(m_phase)),
            static_cast<float>(syntheticToneAmplitude * std::sin(m_phase)));
        m_phase = std::remainder(m_phase + phaseStep, twoPi);
    }
    const auto duration = std::chrono::duration<double>(
        static_cast<double>(samples.size()) /
        static_cast<double>(m_metadata.effectiveSampleRate));
    m_nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        duration);
    return {radio::WidebandIqReadStatus::Samples, samples.size(), {}};
}

}  // namespace sdr::dsp
