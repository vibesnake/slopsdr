// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "SpectrumFrame.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace sdr::dsp {

struct SpectrumProcessingCounters {
    std::atomic<std::uint64_t> inputSamples = 0;
    std::atomic<std::uint64_t> vectorsReceived = 0;
    std::atomic<std::uint64_t> fftsExecuted = 0;
    std::atomic<std::uint64_t> framesPublished = 0;
    std::atomic<std::uint64_t> processingNanoseconds = 0;
};

class FftFrameProcessor final
{
public:
    FftFrameProcessor(
        std::shared_ptr<radio::SpectrumFrameQueue> outputQueue,
        std::shared_ptr<SpectrumProcessingCounters> counters,
        std::size_t fftSize,
        float windowCoherentGain,
        float minimumDecibels = -120.0F,
        float maximumDecibels = 0.0F);

    [[nodiscard]] bool submitMagnitudeFrame(
        std::span<const float> magnitudes,
        std::uint64_t centerFrequency,
        std::uint64_t sampleRate,
        std::uint64_t timestampNanoseconds,
        std::uint64_t tuningGeneration = 0);

private:
    std::shared_ptr<radio::SpectrumFrameQueue> m_outputQueue;
    std::shared_ptr<SpectrumProcessingCounters> m_counters;
    std::size_t m_fftSize;
    float m_magnitudeNormalization;
    float m_minimumDecibels;
    float m_maximumDecibels;
    mutable std::mutex m_mutex;
    std::uint64_t m_nextSequence = 1;
};

}  // namespace sdr::dsp
