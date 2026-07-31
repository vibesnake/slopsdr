// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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

// Optional deterministic writer controls used by service tests. Production
// callers use the default empty hooks.
struct WavRecordingWriterHooks {
    std::function<void()> afterDequeueLocked;
    std::function<void()> beforeWrite;
    std::function<void()> beforeProducerExit;
    bool failWrites = false;
};

class WavRecordingService final
{
public:
    explicit WavRecordingService(
        std::size_t maximumQueuedFrames = 48'000 * 2,
        std::uint64_t maximumDataBytes = 0xffff'ffdbU,
        WavRecordingWriterHooks writerHooks = {});
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
    void finishProducer() noexcept;
    void writerLoop() noexcept;
    void failFromWriter(std::string message, std::uint64_t rejectedFrames) noexcept;

    const std::size_t m_maximumQueuedFrames;
    const std::uint64_t m_maximumDataBytes;
    const WavRecordingWriterHooks m_writerHooks;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::vector<float>> m_queue;
    std::thread m_writer;
    std::ofstream m_file;
    WavRecordingState m_state;
    std::atomic<std::uint64_t> m_droppedFrames{0};
    std::atomic<std::uint64_t> m_producersInFlight{0};
    std::uint64_t m_dataBytes = 0;
    std::deque<std::vector<float>> m_preRoll;
    std::size_t m_preRollFrames = 0;
    std::size_t m_maximumPreRollFrames = 0;
    std::size_t m_tailDurationFrames = 0;
    std::size_t m_tailFramesRemaining = 0;
    bool m_skipQuietParts = false;
    std::atomic<bool> m_accepting{false};
    bool m_stopRequested = false;
};

}  // namespace sdr::platform
