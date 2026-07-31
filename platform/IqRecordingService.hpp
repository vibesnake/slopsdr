// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <atomic>
#include <complex>
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

struct IqRecordingRequest {
    std::filesystem::path directory;
    std::uint64_t centerFrequencyHz = 0;
    std::uint64_t sampleRate = 0;
    std::string deviceIdentifier;
};

struct IqRecordingState {
    bool active = false;
    bool failed = false;
    std::filesystem::path filePath;
    std::filesystem::path metadataPath;
    std::string statusText = "IQ recording idle";
    std::uint64_t queuedSamples = 0;
    std::uint64_t droppedSamples = 0;
    std::uint64_t writtenSamples = 0;
    std::uint64_t elapsedSeconds = 0;
};

// Optional deterministic writer controls used by service tests. Production
// callers use the default empty hooks.
struct IqRecordingWriterHooks {
    std::function<void()> afterDequeueLocked;
    std::function<void()> beforeWrite;
    bool failWrites = false;
};

class IqRecordingService final
{
public:
    explicit IqRecordingService(
        std::size_t maximumQueuedSamples = 2'400'000,
        IqRecordingWriterHooks writerHooks = {});
    ~IqRecordingService();

    IqRecordingService(const IqRecordingService&) = delete;
    IqRecordingService& operator=(const IqRecordingService&) = delete;

    [[nodiscard]] bool start(const IqRecordingRequest& request);
    void stop() noexcept;
    void enqueue(std::span<const std::complex<float>> samples) noexcept;
    void addDroppedSamples(std::uint64_t samples) noexcept;
    [[nodiscard]] IqRecordingState state() const;

private:
    [[nodiscard]] static std::filesystem::path makeUniquePath(
        const IqRecordingRequest& request);
    static std::string timestampNow();
    static std::string jsonEscape(const std::string& value);
    static void serializeCf32LittleEndian(
        std::vector<char>& destination,
        std::span<const std::complex<float>> samples);
    void finishProducer() noexcept;
    void writerLoop() noexcept;
    void finalize() noexcept;
    void failFromWriter(std::string message, std::uint64_t rejectedSamples) noexcept;

    const std::size_t m_maximumQueuedSamples;
    const IqRecordingWriterHooks m_writerHooks;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::vector<std::complex<float>>> m_queue;
    std::thread m_writer;
    std::ofstream m_file;
    IqRecordingState m_state;
    std::atomic<std::uint64_t> m_droppedSamples{0};
    std::atomic<std::uint64_t> m_producersInFlight{0};
    IqRecordingRequest m_request;
    std::string m_startedAt;
    std::atomic<bool> m_accepting{false};
    bool m_stopRequested = false;
};

}  // namespace sdr::platform
