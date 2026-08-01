// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>

namespace sdr::radio {

enum class RecordedAudioEncoding { UnsignedPcm8, SignedPcm16, SignedPcm24,
                                   SignedPcm32, Float32 };

struct RecordedAudioMetadata {
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    RecordedAudioEncoding encoding = RecordedAudioEncoding::SignedPcm16;
    std::uint64_t frameCount = 0;
    std::uint16_t bytesPerFrame = 0;
};

enum class RecordedAudioReadStatus { Frames, Timeout, Stopped, EndOfFile, Failed };

struct RecordedAudioReadResult {
    RecordedAudioReadStatus status = RecordedAudioReadStatus::Failed;
    std::size_t frameCount = 0;
    std::string message;
};

struct RecordedAudioSourceOperationResult {
    bool succeeded = false;
    bool stateChanged = false;
    std::string message;
};

// This deliberately has no Qt, GNU Radio, or presentation dependency.  It is
// a bounded, paced reader for ordinary RIFF/WAVE audio, not an RF source.
class RecordedAudioSource final
{
public:
    explicit RecordedAudioSource(std::filesystem::path path);
    ~RecordedAudioSource();

    RecordedAudioSource(const RecordedAudioSource&) = delete;
    RecordedAudioSource& operator=(const RecordedAudioSource&) = delete;

    [[nodiscard]] static bool hasWaveSignature(const std::filesystem::path& path) noexcept;
    [[nodiscard]] const RecordedAudioMetadata& metadata() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] RecordedAudioSourceOperationResult start();
    [[nodiscard]] RecordedAudioSourceOperationResult stop();
    [[nodiscard]] RecordedAudioReadResult read(
        std::span<float> interleavedSamples, std::chrono::milliseconds timeout);
    void setPaused(bool paused) noexcept;

    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] std::uint64_t positionFrames() const noexcept;

private:
    void parse();
    [[nodiscard]] float decodeSample(const unsigned char* bytes) const noexcept;

    std::filesystem::path m_path;
    RecordedAudioMetadata m_metadata;
    std::uint64_t m_dataOffset = 0;
    std::ifstream m_file;
    mutable std::mutex m_fileMutex;
    std::mutex m_waitMutex;
    std::condition_variable m_waitCondition;
    std::chrono::steady_clock::time_point m_nextDeadline;
    std::atomic_bool m_running = false;
    std::atomic_bool m_paused = false;
    std::atomic_bool m_resumeNeedsDeadline = false;
    std::atomic_bool m_ended = false;
    std::atomic<std::uint64_t> m_positionFrames = 0;
};

}  // namespace sdr::radio
