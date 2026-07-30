// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sdr::platform {

struct WavRecordingRequest {
    std::filesystem::path directory;
    std::uint64_t frequencyHz = 0;
    std::string modeName;
};

struct WavRecordingState {
    bool active = false;
    bool failed = false;
    std::filesystem::path filePath;
    std::string statusText = "Recording idle";
    std::uint64_t elapsedSeconds = 0;
    std::uint64_t queuedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t writtenFrames = 0;
};

class WavRecordingService final
{
public:
    explicit WavRecordingService(
        std::size_t maximumQueuedFrames = 48'000 * 2,
        std::uint64_t maximumDataBytes = 0xffff'ffdbU);
    ~WavRecordingService();

    WavRecordingService(const WavRecordingService&) = delete;
    WavRecordingService& operator=(const WavRecordingService&) = delete;

    [[nodiscard]] bool start(const WavRecordingRequest& request);
    void stop() noexcept;
    void enqueueMono(std::span<const float> samples) noexcept;
    void enqueueStereo(std::span<const float> interleavedSamples) noexcept;
    [[nodiscard]] WavRecordingState state() const;

private:
    [[nodiscard]] static std::filesystem::path makeUniquePath(
        const WavRecordingRequest& request);
    static void writeHeader(std::ofstream& file, std::uint32_t dataBytes);
    static void appendPcm16(
        std::vector<std::int16_t>& destination,
        std::span<const float> interleavedSamples);
    void enqueueInterleavedStereo(
        std::span<const float> interleavedSamples) noexcept;
    void writerLoop() noexcept;
    void failLocked(std::string message) noexcept;

    const std::size_t m_maximumQueuedFrames;
    const std::uint64_t m_maximumDataBytes;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::vector<float>> m_queue;
    std::thread m_writer;
    std::ofstream m_file;
    WavRecordingState m_state;
    std::chrono::steady_clock::time_point m_startedAt;
    std::uint64_t m_dataBytes = 0;
    bool m_accepting = false;
    bool m_stopRequested = false;
};

}  // namespace sdr::platform
