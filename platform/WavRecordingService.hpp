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
    bool scannerActivity = false;
    bool skipQuietParts = false;
    std::uint32_t preRollSeconds = 1;
    std::uint32_t tailSeconds = 2;
};

struct WavRecordingState {
    bool active = false;
    bool writing = false;
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
    void enqueueMono(std::span<const float> samples, bool voiceOpen = true) noexcept;
    void enqueueStereo(std::span<const float> interleavedSamples,
        bool voiceOpen = true) noexcept;
    [[nodiscard]] WavRecordingState state() const;

private:
    [[nodiscard]] static std::filesystem::path makeUniquePath(
        const WavRecordingRequest& request);
    static void writeHeader(std::ofstream& file, std::uint32_t dataBytes);
    static void appendPcm16(
        std::vector<std::int16_t>& destination,
        std::span<const float> interleavedSamples);
    void enqueueInterleavedStereo(std::span<const float> interleavedSamples,
        bool voiceOpen) noexcept;
    void appendPreRollLocked(std::span<const float> interleavedSamples);
    void enqueueLocked(std::span<const float> interleavedSamples);
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
    std::uint64_t m_dataBytes = 0;
    std::deque<std::vector<float>> m_preRoll;
    std::size_t m_preRollFrames = 0;
    std::size_t m_maximumPreRollFrames = 0;
    std::size_t m_tailDurationFrames = 0;
    std::size_t m_tailFramesRemaining = 0;
    bool m_skipQuietParts = false;
    bool m_accepting = false;
    bool m_stopRequested = false;
};

}  // namespace sdr::platform
