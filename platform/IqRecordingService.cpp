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

constexpr std::size_t bytesPerComplexSample = sizeof(float) * 2;

}  // namespace

IqRecordingService::IqRecordingService(
    std::size_t maximumQueuedSamples,
    IqRecordingWriterHooks writerHooks)
    : m_maximumQueuedSamples(std::max<std::size_t>(1, maximumQueuedSamples))
    , m_writerHooks(std::move(writerHooks))
{
}

IqRecordingService::~IqRecordingService()
{
    stop();
}

bool IqRecordingService::start(const IqRecordingRequest& request)
{
    stop();
    m_droppedSamples.store(0, std::memory_order_release);
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
        m_accepting.store(true, std::memory_order_release);
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
        m_accepting.store(false, std::memory_order_release);
        m_stopRequested = true;
    }
    m_condition.notify_one();
    if (m_writer.joinable()) m_writer.join();
}

void IqRecordingService::enqueue(
    std::span<const std::complex<float>> samples) noexcept
{
    if (samples.empty()) return;
    m_producersInFlight.fetch_add(1, std::memory_order_acq_rel);
    if (!m_accepting.load(std::memory_order_acquire)) {
        finishProducer();
        return;
    }
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() ||
        !m_accepting.load(std::memory_order_acquire) || !m_state.active) {
        m_droppedSamples.fetch_add(samples.size(), std::memory_order_relaxed);
        if (lock.owns_lock()) lock.unlock();
        finishProducer();
        return;
    }
    if (samples.size() > m_maximumQueuedSamples ||
        m_state.queuedSamples + samples.size() > m_maximumQueuedSamples) {
        m_droppedSamples.fetch_add(samples.size(), std::memory_order_relaxed);
        lock.unlock();
        finishProducer();
        return;
    }
    m_queue.emplace_back(samples.begin(), samples.end());
    m_state.queuedSamples += samples.size();
    lock.unlock();
    finishProducer();
    m_condition.notify_one();
}

void IqRecordingService::addDroppedSamples(std::uint64_t samples) noexcept
{
    if (samples == 0) return;
    m_producersInFlight.fetch_add(1, std::memory_order_acq_rel);
    if (m_accepting.load(std::memory_order_acquire)) {
        m_droppedSamples.fetch_add(samples, std::memory_order_relaxed);
    }
    finishProducer();
}

IqRecordingState IqRecordingService::state() const
{
    std::lock_guard lock(m_mutex);
    auto result = m_state;
    result.droppedSamples = m_droppedSamples.load(std::memory_order_acquire);
    if (m_request.sampleRate > 0)
        result.elapsedSeconds = result.writtenSamples / m_request.sampleRate;
    return result;
}

void IqRecordingService::serializeCf32LittleEndian(
    std::vector<char>& destination,
    std::span<const std::complex<float>> samples)
{
    destination.resize(samples.size() * bytesPerComplexSample);
    std::size_t offset = 0;
    const auto appendFloat = [&destination, &offset](float value) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned byte = 0; byte < sizeof(bits); ++byte) {
            destination[offset++] = static_cast<char>((bits >> (byte * 8U)) & 0xffU);
        }
    };
    for (const auto& sample : samples) {
        appendFloat(sample.real());
        appendFloat(sample.imag());
    }
}

void IqRecordingService::finishProducer() noexcept
{
    if (m_writerHooks.beforeProducerExit) m_writerHooks.beforeProducerExit();
    if (m_producersInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_producersInFlight.notify_all();
    }
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
    candidate += ".raw";
    for (unsigned suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = base;
        candidate += "_" + std::to_string(suffix) + ".raw";
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
    std::vector<char> serialized;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
            if (m_queue.empty() && m_stopRequested) break;
            samples = std::move(m_queue.front());
            m_queue.pop_front();
            m_state.queuedSamples -= samples.size();
            if (m_writerHooks.afterDequeueLocked) {
                m_writerHooks.afterDequeueLocked();
            }
        }
        serializeCf32LittleEndian(serialized, samples);
        if (m_writerHooks.beforeWrite) m_writerHooks.beforeWrite();
        if (m_writerHooks.failWrites) {
            failFromWriter("IQ recording write failed", samples.size());
            continue;
        }
        if (!m_file.is_open()) {
            failFromWriter("IQ recording file is unavailable", samples.size());
            continue;
        }
        m_file.write(serialized.data(),
                     static_cast<std::streamsize>(serialized.size()));
        if (!m_file.good()) {
            failFromWriter("IQ recording write failed", samples.size());
            continue;
        }
        {
            std::lock_guard lock(m_mutex);
            m_state.writtenSamples += samples.size();
        }
    }
    finalize();
}

void IqRecordingService::finalize() noexcept
{
    if (m_file.is_open()) m_file.close();
    IqRecordingState state;
    IqRecordingRequest request;
    std::string startedAt;
    {
        auto producers = m_producersInFlight.load(std::memory_order_acquire);
        while (producers != 0) {
            m_producersInFlight.wait(producers, std::memory_order_acquire);
            producers = m_producersInFlight.load(std::memory_order_acquire);
        }
        std::lock_guard lock(m_mutex);
        state = m_state;
        state.droppedSamples = m_droppedSamples.load(std::memory_order_acquire);
        request = m_request;
        startedAt = m_startedAt;
    }
    if (state.filePath.empty()) return;
    std::ofstream metadata(state.metadataPath, std::ios::trunc);
    bool metadataWritten = false;
    if (metadata.is_open()) {
        const double duration = request.sampleRate == 0 ? 0.0 :
            static_cast<double>(state.writtenSamples) /
            static_cast<double>(request.sampleRate);
        metadata << "{\n"
                 << "  \"start_timestamp\": \"" << startedAt << "\",\n"
                 << "  \"end_timestamp\": \"" << timestampNow() << "\",\n"
                 << "  \"hardware_center_frequency_hz\": " << request.centerFrequencyHz << ",\n"
                 << "  \"sample_rate_hz\": " << request.sampleRate << ",\n"
                 << "  \"sample_format\": \"cf32_le\",\n"
                 << "  \"byte_order\": \"little-endian\",\n"
                 << "  \"written_sample_count\": " << state.writtenSamples << ",\n"
                 << "  \"duration_seconds\": " << duration << ",\n"
                 << "  \"dropped_sample_count\": " << state.droppedSamples << ",\n"
                 << "  \"device_identifier\": \"" << jsonEscape(request.deviceIdentifier) << "\"\n"
                 << "}\n";
        metadataWritten = metadata.good();
    }
    std::lock_guard lock(m_mutex);
    if (!metadataWritten && !m_state.failed) {
        m_state.statusText = "IQ recording saved; metadata write failed";
    }
    if (m_state.active) {
        m_state.active = false;
        if (!m_state.failed && m_state.statusText == "Recording full-bandwidth IQ")
            m_state.statusText = "IQ recording saved";
    }
    m_accepting.store(false, std::memory_order_release);
}

void IqRecordingService::failFromWriter(
    std::string message, std::uint64_t rejectedSamples) noexcept
{
    std::lock_guard lock(m_mutex);
    m_state.failed = true;
    m_state.statusText = std::move(message);
    m_accepting.store(false, std::memory_order_release);
    m_stopRequested = true;
    m_droppedSamples.fetch_add(
        rejectedSamples + m_state.queuedSamples, std::memory_order_relaxed);
    m_queue.clear();
    m_state.queuedSamples = 0;
}

}  // namespace sdr::platform
