// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WidebandIqSources.hpp"

#include "DeviceController.hpp"

#include <cmath>
#include <bit>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
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

namespace {

[[nodiscard]] std::optional<std::uint64_t> jsonUnsigned(
    const std::string& json, const char* key)
{
    const std::regex expression(std::string{"\""} + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(json, match, expression)) return std::nullopt;
    try { return std::stoull(match[1].str()); } catch (...) { return std::nullopt; }
}

[[nodiscard]] std::optional<std::string> jsonString(
    const std::string& json, const char* key)
{
    const std::regex expression(std::string{"\""} + key + "\\\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, expression)) return std::nullopt;
    return match[1].str();
}

void validateRecordedConfiguration(radio::RecordedIqSourceConfiguration& configuration,
    radio::WidebandIqCaptureMetadata& metadata, std::uint64_t& sampleCount)
{
    namespace fs = std::filesystem;
    const fs::path rawPath(configuration.path);
    if (rawPath.extension() != ".raw") {
        throw std::invalid_argument("Recorded IQ source must select a .raw file");
    }
    std::error_code error;
    const auto bytes = fs::file_size(rawPath, error);
    if (error) throw std::invalid_argument("Recorded IQ file cannot be read: " + error.message());
    if (bytes == 0 || bytes % (sizeof(float) * 2) != 0) {
        throw std::invalid_argument("Recorded IQ file is truncated: cf32_le samples require eight bytes");
    }
    fs::path sidecar = rawPath;
    sidecar.replace_extension(".json");
    if (fs::exists(sidecar, error) && !error) {
        std::ifstream input(sidecar, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        const auto center = jsonUnsigned(json, "hardware_center_frequency_hz");
        const auto rate = jsonUnsigned(json, "sample_rate_hz");
        const auto format = jsonString(json, "sample_format");
        const auto byteOrder = jsonString(json, "byte_order");
        const auto written = jsonUnsigned(json, "written_sample_count");
        if (center && *center > 0 && rate && *rate > 0 && format && *format == "cf32_le" &&
            byteOrder && *byteOrder == "little-endian" &&
            (!written || *written == bytes / 8)) {
            configuration.centerFrequency = *center;
            configuration.sampleRate = *rate;
            configuration.format = *format;
        }
    }
    if (configuration.format != "cf32_le") {
        throw std::invalid_argument("Recorded IQ format is unsupported; only cf32_le is supported");
    }
    if (configuration.centerFrequency == 0 || configuration.sampleRate == 0) {
        throw std::invalid_argument("Recorded IQ metadata is missing; enter a center frequency and sample rate");
    }
    metadata = {configuration.centerFrequency, configuration.sampleRate};
    sampleCount = bytes / 8;
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

RecordedIqSource::RecordedIqSource(radio::RecordedIqSourceConfiguration configuration)
    : m_configuration(resolveConfiguration(std::move(configuration)))
{
    validateRecordedConfiguration(m_configuration, m_metadata, m_sampleCount);
}

radio::RecordedIqSourceConfiguration RecordedIqSource::resolveConfiguration(
    radio::RecordedIqSourceConfiguration configuration)
{
    radio::WidebandIqCaptureMetadata ignoredMetadata;
    std::uint64_t ignoredSampleCount = 0;
    validateRecordedConfiguration(configuration, ignoredMetadata, ignoredSampleCount);
    return configuration;
}

radio::ReceiverSourceCapabilities RecordedIqSource::capabilities() const noexcept
{
    return {.kind = radio::ReceiverSourceKind::RecordedIq, .sampleRateChangeSupported = false};
}

radio::WidebandIqCaptureMetadata RecordedIqSource::captureMetadata() const noexcept { return m_metadata; }
std::uint64_t RecordedIqSource::sampleCount() const noexcept { return m_sampleCount; }

radio::WidebandIqSourceOperationResult RecordedIqSource::start()
{
    if (m_running) return {true, false, "Recorded IQ playback is already active"};
    m_file.open(m_configuration.path, std::ios::binary);
    if (!m_file) return {false, false, "Recorded IQ file cannot be opened for playback"};
    m_samplesRead = 0;
    m_nextDeadline = std::chrono::steady_clock::now();
    m_paused = false;
    m_ended = false;
    m_running = true;
    return {true, true, "Recorded IQ playback started"};
}

radio::WidebandIqSourceOperationResult RecordedIqSource::stop()
{
    if (!m_running) return {true, false, "Recorded IQ playback is already stopped"};
    m_running = false;
    m_paused = false;
    m_file.close();
    return {true, true, "Recorded IQ playback stopped"};
}

radio::WidebandIqReadResult RecordedIqSource::read(
    std::span<std::complex<float>> samples, std::chrono::milliseconds timeout)
{
    static_cast<void>(timeout);
    if (!m_running) return {radio::WidebandIqReadStatus::Stopped, 0, "Recorded IQ playback is stopped"};
    if (m_paused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return {radio::WidebandIqReadStatus::Timeout, 0, {}};
    }
    if (m_resumeNeedsDeadline.exchange(false)) {
        m_nextDeadline = std::chrono::steady_clock::now();
    }
    if (samples.empty()) return {radio::WidebandIqReadStatus::Failed, 0, "Recorded IQ source received an empty buffer"};
    if (m_samplesRead == m_sampleCount) {
        m_ended = true;
        return {radio::WidebandIqReadStatus::EndOfFile, 0, "Recorded IQ playback reached end of file"};
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_nextDeadline > now) std::this_thread::sleep_until(m_nextDeadline);
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(samples.size(), m_sampleCount - m_samplesRead));
    std::array<unsigned char, 8> bytes{};
    for (std::size_t index = 0; index < count; ++index) {
        m_file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (m_file.gcount() != static_cast<std::streamsize>(bytes.size())) {
            return {radio::WidebandIqReadStatus::Failed, index, "Recorded IQ file ended mid-sample"};
        }
        const auto decode = [&bytes](std::size_t offset) {
            const std::uint32_t bits = static_cast<std::uint32_t>(bytes[offset]) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
                (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
            return std::bit_cast<float>(bits);
        };
        samples[index] = {decode(0), decode(4)};
    }
    m_samplesRead += count;
    m_nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(static_cast<double>(count) /
            static_cast<double>(m_metadata.effectiveSampleRate)));
    return {radio::WidebandIqReadStatus::Samples, count, {}};
}

std::uint64_t RecordedIqSource::positionSamples() const noexcept { return m_samplesRead; }
bool RecordedIqSource::paused() const noexcept { return m_paused; }
bool RecordedIqSource::ended() const noexcept { return m_ended; }
void RecordedIqSource::setPaused(bool paused) noexcept
{
    const bool wasPaused = m_paused.exchange(paused);
    if (wasPaused && !paused) m_resumeNeedsDeadline = true;
}

}  // namespace sdr::dsp
