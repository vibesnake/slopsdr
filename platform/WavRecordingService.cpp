// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WavRecordingService.hpp"

#include "AudioSampleBuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace sdr::platform {
namespace {

constexpr std::uint16_t stereoChannels = 2;
constexpr std::uint16_t bitsPerSample = 16;
constexpr std::uint32_t bytesPerFrame =
    stereoChannels * (bitsPerSample / 8U);
constexpr std::uint32_t maximumWavDataBytes =
    std::numeric_limits<std::uint32_t>::max() - 36U;

void writeLittleEndian(
    std::ofstream& file, std::uint32_t value, std::size_t bytes)
{
    for (std::size_t index = 0; index < bytes; ++index) {
        file.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

std::string sanitizeName(std::string value)
{
    for (char& character : value) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '-' || character == '_';
        if (!safe) {
            character = '_';
        }
    }
    return value.empty() ? std::string("audio") : value;
}

}  // namespace

WavRecordingService::WavRecordingService(
    std::size_t maximumQueuedFrames,
    std::uint64_t maximumDataBytes)
    : m_maximumQueuedFrames(std::max<std::size_t>(1, maximumQueuedFrames))
    , m_maximumDataBytes(std::min(
          maximumDataBytes,
          static_cast<std::uint64_t>(maximumWavDataBytes)))
{
}

WavRecordingService::~WavRecordingService()
{
    stop();
}

bool WavRecordingService::start(const WavRecordingRequest& request)
{
    stop();
    std::error_code error;
    if (request.directory.empty() ||
        !std::filesystem::is_directory(request.directory, error) || error) {
        std::lock_guard lock(m_mutex);
        m_state = {.active = false,
                   .failed = true,
                   .filePath = {},
                   .statusText = "Recording folder is unavailable"};
        return false;
    }

    const std::filesystem::path path = makeUniquePath(request);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::lock_guard lock(m_mutex);
        m_state = {.failed = true,
                   .filePath = path,
                   .statusText = "Could not create WAV recording"};
        return false;
    }
    writeHeader(file, 0);
    if (!file.good()) {
        std::lock_guard lock(m_mutex);
        m_state = {.failed = true,
                   .filePath = path,
                   .statusText = "Could not write WAV header"};
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        m_file = std::move(file);
        m_dataBytes = 0;
        m_stopRequested = false;
        m_accepting = true;
        m_startedAt = std::chrono::steady_clock::now();
        m_state = {.active = true,
                   .filePath = path,
                   .statusText = "Recording WAV"};
    }
    m_writer = std::thread(&WavRecordingService::writerLoop, this);
    return true;
}

void WavRecordingService::stop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_accepting = false;
        m_stopRequested = true;
    }
    m_condition.notify_one();
    if (m_writer.joinable()) {
        m_writer.join();
    }
}

void WavRecordingService::enqueueMono(std::span<const float> samples) noexcept
{
    if (samples.empty()) {
        return;
    }
    std::vector<float> stereo;
    stereo.reserve(samples.size() * stereoChannels);
    for (const float sample : samples) {
        stereo.push_back(sample);
        stereo.push_back(sample);
    }
    enqueueInterleavedStereo(stereo);
}

void WavRecordingService::enqueueStereo(
    std::span<const float> interleavedSamples) noexcept
{
    if (interleavedSamples.size() < stereoChannels) {
        return;
    }
    enqueueInterleavedStereo(interleavedSamples.first(
        interleavedSamples.size() / stereoChannels * stereoChannels));
}

WavRecordingState WavRecordingService::state() const
{
    std::lock_guard lock(m_mutex);
    WavRecordingState result = m_state;
    if (result.active) {
        result.elapsedSeconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_startedAt)
                .count());
    }
    return result;
}

std::filesystem::path WavRecordingService::makeUniquePath(
    const WavRecordingRequest& request)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t calendar = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&calendar, &local);
    std::ostringstream name;
    name << std::put_time(&local, "%Y%m%d_%H%M%S") << '_'
         << request.frequencyHz << "Hz_"
         << sanitizeName(request.modeName) << "_filtered-audio";
    const std::filesystem::path base = request.directory / name.str();
    std::filesystem::path candidate = base;
    candidate += ".wav";
    for (unsigned suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = base;
        candidate += "_" + std::to_string(suffix) + ".wav";
    }
    return candidate;
}

void WavRecordingService::writeHeader(
    std::ofstream& file, std::uint32_t dataBytes)
{
    file.seekp(0);
    file.write("RIFF", 4);
    writeLittleEndian(file, 36U + dataBytes, 4);
    file.write("WAVEfmt ", 8);
    writeLittleEndian(file, 16, 4);
    writeLittleEndian(file, 1, 2);
    writeLittleEndian(file, stereoChannels, 2);
    writeLittleEndian(file, radio::receiverAudioSampleRate, 4);
    writeLittleEndian(file, radio::receiverAudioSampleRate * bytesPerFrame, 4);
    writeLittleEndian(file, bytesPerFrame, 2);
    writeLittleEndian(file, bitsPerSample, 2);
    file.write("data", 4);
    writeLittleEndian(file, dataBytes, 4);
}

void WavRecordingService::appendPcm16(
    std::vector<std::int16_t>& destination,
    std::span<const float> interleavedSamples)
{
    destination.resize(interleavedSamples.size());
    for (std::size_t index = 0; index < interleavedSamples.size(); ++index) {
        const float sample = std::isfinite(interleavedSamples[index])
                                 ? std::clamp(interleavedSamples[index], -1.0F, 1.0F)
                                 : 0.0F;
        destination[index] = static_cast<std::int16_t>(std::lround(
            sample * static_cast<float>(std::numeric_limits<std::int16_t>::max())));
    }
}

void WavRecordingService::enqueueInterleavedStereo(
    std::span<const float> interleavedSamples) noexcept
{
    std::unique_lock lock(m_mutex, std::try_to_lock);
    const std::uint64_t frames = interleavedSamples.size() / stereoChannels;
    if (!lock.owns_lock() || !m_accepting || !m_state.active) {
        if (lock.owns_lock() && m_state.active) {
            m_state.droppedFrames += frames;
        }
        return;
    }
    if (frames > m_maximumQueuedFrames ||
        m_state.queuedFrames + frames > m_maximumQueuedFrames) {
        m_state.droppedFrames += frames;
        return;
    }
    m_queue.emplace_back(interleavedSamples.begin(), interleavedSamples.end());
    m_state.queuedFrames += frames;
    lock.unlock();
    m_condition.notify_one();
}

void WavRecordingService::writerLoop() noexcept
{
    std::vector<float> samples;
    std::vector<std::int16_t> pcm;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] {
                return m_stopRequested || !m_queue.empty();
            });
            if (m_queue.empty() && m_stopRequested) {
                break;
            }
            samples = std::move(m_queue.front());
            m_queue.pop_front();
            m_state.queuedFrames -= samples.size() / stereoChannels;
        }
        appendPcm16(pcm, samples);
        {
            std::lock_guard lock(m_mutex);
            if (!m_file.is_open()) {
                failLocked("WAV recording file is unavailable");
                continue;
            }
            const std::uint64_t bytes = pcm.size() * sizeof(std::int16_t);
            if (m_dataBytes + bytes > m_maximumDataBytes) {
                failLocked("WAV recording reached its 4 GiB limit");
                continue;
            }
            m_file.write(reinterpret_cast<const char*>(pcm.data()),
                         static_cast<std::streamsize>(bytes));
            if (!m_file.good()) {
                failLocked("WAV recording write failed");
                continue;
            }
            m_dataBytes += bytes;
            m_state.writtenFrames += pcm.size() / stereoChannels;
        }
    }
    std::lock_guard lock(m_mutex);
    if (m_file.is_open()) {
        writeHeader(m_file, static_cast<std::uint32_t>(m_dataBytes));
        m_file.close();
    }
    if (m_state.active) {
        m_state.active = false;
        m_state.statusText = m_state.failed
                                 ? m_state.statusText
                                 : "WAV recording saved";
    }
    m_accepting = false;
}

void WavRecordingService::failLocked(std::string message) noexcept
{
    m_state.failed = true;
    m_state.statusText = std::move(message);
    m_accepting = false;
    m_stopRequested = true;
    m_queue.clear();
    m_state.queuedFrames = 0;
}

}  // namespace sdr::platform
