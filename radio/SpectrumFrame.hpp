// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace sdr::radio {

struct SpectrumFrame {
    std::uint64_t sequence = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::uint64_t centerFrequency = 0;
    std::uint64_t sampleRate = 0;
    // The capture span is normally equal to the effective complex sample rate,
    // but remains explicit so display mapping never infers it from live state.
    std::uint64_t captureSpan = 0;
    std::size_t fftSize = 0;
    std::uint64_t tuningGeneration = 0;
    std::vector<float> normalizedMagnitudes;
};

[[nodiscard]] bool hasConsistentMetadata(const SpectrumFrame& frame) noexcept;
[[nodiscard]] std::uint64_t captureSpan(const SpectrumFrame& frame) noexcept;
[[nodiscard]] bool isCompatibleSpectrumFrame(
    const SpectrumFrame& frame,
    std::uint64_t sampleRate,
    std::size_t fftSize) noexcept;
[[nodiscard]] bool isCurrentTuningFrame(
    const SpectrumFrame& frame,
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    std::uint64_t tuningGeneration) noexcept;

struct SpectrumProcessingMetrics {
    std::uint64_t inputSamples = 0;
    std::uint64_t vectorsReceived = 0;
    std::uint64_t fftsExecuted = 0;
    std::uint64_t framesPublished = 0;
    std::uint64_t framesDropped = 0;
    std::size_t fftSize = 0;
    std::size_t queueDepth = 0;
    double effectiveSampleRate = 0.0;
    double availableVectorsPerSecond = 0.0;
    double targetFramesPerSecond = 0.0;
    double achievableFramesPerSecond = 0.0;
    double hertzPerBin = 0.0;
    double hopSize = 0.0;
    double overlapPercentage = 0.0;
    double averageProcessingMilliseconds = 0.0;
};

class SpectrumFrameQueue final
{
public:
    explicit SpectrumFrameQueue(std::size_t capacity);

    SpectrumFrameQueue(const SpectrumFrameQueue&) = delete;
    SpectrumFrameQueue& operator=(const SpectrumFrameQueue&) = delete;

    void push(SpectrumFrame frame);
    void clear();
    [[nodiscard]] std::optional<SpectrumFrame> takeOldest();
    [[nodiscard]] std::optional<SpectrumFrame> takeLatest();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::uint64_t droppedFrameCount() const;

private:
    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<SpectrumFrame> m_frames;
    std::uint64_t m_droppedFrameCount = 0;
};

}  // namespace sdr::radio
