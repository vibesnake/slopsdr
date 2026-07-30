// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "IqRecordingService.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace sdr::platform {
namespace {

void writeFloatLittleEndian(std::ofstream& file, float value)
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned byte = 0; byte < 4; ++byte) {
        file.put(static_cast<char>((bits >> (byte * 8U)) & 0xffU));
    }
}

}  // namespace

IqRecordingService::IqRecordingService(std::size_t maximumQueuedSamples)
    : m_maximumQueuedSamples(std::max<std::size_t>(1, maximumQueuedSamples))
{
}

IqRecordingService::~IqRecordingService()
{
    stop();
}

bool IqRecordingService::start(const IqRecordingRequest& request)
{
    stop();
    std::error_code error;
    if (request.directory.empty() || request.sampleRate == 0 ||
        !std::filesystem::is_directory(request.directory, error) || error) {
        std::lock_guard lock(m_mutex);
        m_state = {.active = false, .failed = true, .filePath = {},
                   .metadataPath = {},
                   .statusText = "IQ recording folder is unavailable"};
        return false;
    }
    const auto path = makeUniquePath(request);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::lock_guard lock(m_mutex);
        m_state = {.active = false, .failed = true, .filePath = path,
                   .metadataPath = {},
                   .statusText = "Could not create IQ recording"};
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        m_file = std::move(file);
        m_request = request;
        m_startedAt = timestampNow();
        m_stopRequested = false;
        m_accepting = true;
        auto metadata = path;
        metadata.replace_extension(".json");
        m_state = {.active = true, .filePath = path, .metadataPath = metadata,
                   .statusText = "Recording full-bandwidth IQ"};
    }
    m_writer = std::thread(&IqRecordingService::writerLoop, this);
    return true;
}

void IqRecordingService::stop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_accepting = false;
        m_stopRequested = true;
    }
    m_condition.notify_one();
    if (m_writer.joinable()) m_writer.join();
}

void IqRecordingService::enqueue(
    std::span<const std::complex<float>> samples) noexcept
{
    if (samples.empty()) return;
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_accepting || !m_state.active) {
        if (lock.owns_lock() && m_state.active) m_state.droppedSamples += samples.size();
        return;
    }
    if (samples.size() > m_maximumQueuedSamples ||
        m_state.queuedSamples + samples.size() > m_maximumQueuedSamples) {
        m_state.droppedSamples += samples.size();
        return;
    }
    m_queue.emplace_back(samples.begin(), samples.end());
    m_state.queuedSamples += samples.size();
    lock.unlock();
    m_condition.notify_one();
}

void IqRecordingService::addDroppedSamples(std::uint64_t samples) noexcept
{
    std::lock_guard lock(m_mutex);
    if (m_state.active) m_state.droppedSamples += samples;
}

IqRecordingState IqRecordingService::state() const
{
    std::lock_guard lock(m_mutex);
    auto result = m_state;
    if (m_request.sampleRate > 0)
        result.elapsedSeconds = result.writtenSamples / m_request.sampleRate;
    return result;
}

std::filesystem::path IqRecordingService::makeUniquePath(const IqRecordingRequest& request)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t calendar = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&calendar, &local);
    std::ostringstream name;
    name << std::put_time(&local, "%Y%m%d_%H%M%S") << '_'
         << request.centerFrequencyHz << "Hz_" << request.sampleRate
         << "sps_full-iq";
    const auto base = request.directory / name.str();
    auto candidate = base;
    candidate += ".cf32";
    for (unsigned suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = base;
        candidate += "_" + std::to_string(suffix) + ".cf32";
    }
    return candidate;
}

std::string IqRecordingService::timestampNow()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t calendar = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&calendar, &utc);
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << milliseconds.count() << 'Z';
    return result.str();
}

std::string IqRecordingService::jsonEscape(const std::string& value)
{
    std::string escaped;
    for (const char c : value) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return escaped;
}

void IqRecordingService::writerLoop() noexcept
{
    std::vector<std::complex<float>> samples;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
            if (m_queue.empty() && m_stopRequested) break;
            samples = std::move(m_queue.front());
            m_queue.pop_front();
            m_state.queuedSamples -= samples.size();
        }
        std::lock_guard lock(m_mutex);
        if (!m_file.is_open()) {
            failLocked("IQ recording file is unavailable");
            continue;
        }
        for (const auto& sample : samples) {
            writeFloatLittleEndian(m_file, sample.real());
            writeFloatLittleEndian(m_file, sample.imag());
        }
        if (!m_file.good()) {
            failLocked("IQ recording write failed");
            continue;
        }
        m_state.writtenSamples += samples.size();
    }
    std::lock_guard lock(m_mutex);
    finalizeLocked();
}

void IqRecordingService::finalizeLocked() noexcept
{
    if (m_file.is_open()) m_file.close();
    if (m_state.filePath.empty()) return;
    std::ofstream metadata(m_state.metadataPath, std::ios::trunc);
    if (!metadata.is_open()) {
        if (!m_state.failed) m_state.statusText = "IQ recording saved; metadata write failed";
    } else {
        const double duration = m_request.sampleRate == 0 ? 0.0 :
            static_cast<double>(m_state.writtenSamples) /
            static_cast<double>(m_request.sampleRate);
        metadata << "{\n"
                 << "  \"start_timestamp\": \"" << m_startedAt << "\",\n"
                 << "  \"end_timestamp\": \"" << timestampNow() << "\",\n"
                 << "  \"hardware_center_frequency_hz\": " << m_request.centerFrequencyHz << ",\n"
                 << "  \"sample_rate_hz\": " << m_request.sampleRate << ",\n"
                 << "  \"sample_format\": \"cf32\",\n"
                 << "  \"byte_order\": \"little-endian\",\n"
                 << "  \"written_sample_count\": " << m_state.writtenSamples << ",\n"
                 << "  \"duration_seconds\": " << duration << ",\n"
                 << "  \"dropped_sample_count\": " << m_state.droppedSamples << ",\n"
                 << "  \"device_identifier\": \"" << jsonEscape(m_request.deviceIdentifier) << "\"\n"
                 << "}\n";
        if (!metadata.good() && !m_state.failed)
            m_state.statusText = "IQ recording saved; metadata write failed";
    }
    if (m_state.active) {
        m_state.active = false;
        if (!m_state.failed && m_state.statusText == "Recording full-bandwidth IQ")
            m_state.statusText = "IQ recording saved";
    }
    m_accepting = false;
}

void IqRecordingService::failLocked(std::string message) noexcept
{
    m_state.failed = true;
    m_state.statusText = std::move(message);
    m_accepting = false;
    m_stopRequested = true;
    m_queue.clear();
    m_state.queuedSamples = 0;
}

}  // namespace sdr::platform
