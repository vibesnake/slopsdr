// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FftFrameProcessor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sdr::dsp {

FftFrameProcessor::FftFrameProcessor(
    std::shared_ptr<radio::SpectrumFrameQueue> outputQueue,
    std::shared_ptr<SpectrumProcessingCounters> counters,
    std::size_t fftSize,
    float windowCoherentGain,
    float minimumDecibels,
    float maximumDecibels)
    : m_outputQueue(std::move(outputQueue))
    , m_counters(std::move(counters))
    , m_fftSize(fftSize)
    , m_magnitudeNormalization(
          static_cast<float>(fftSize) * windowCoherentGain)
    , m_minimumDecibels(minimumDecibels)
    , m_maximumDecibels(maximumDecibels)
{
    if (!m_outputQueue || !m_counters) {
        throw std::invalid_argument("FFT frame processor requires an output queue and counters");
    }
    if (fftSize == 0 || !std::isfinite(windowCoherentGain) ||
        windowCoherentGain <= 0.0F) {
        throw std::invalid_argument("FFT magnitude normalization is invalid");
    }
    if (!std::isfinite(minimumDecibels) || !std::isfinite(maximumDecibels) ||
        maximumDecibels <= minimumDecibels) {
        throw std::invalid_argument("FFT display-frame configuration is invalid");
    }
}

bool FftFrameProcessor::submitMagnitudeFrame(
    std::span<const float> magnitudes,
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    std::uint64_t timestampNanoseconds,
    std::uint64_t tuningGeneration)
{
    const auto processingStarted = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    if (magnitudes.size() != m_fftSize || sampleRate == 0 ||
        timestampNanoseconds == 0) {
        return false;
    }
    m_counters->fftsExecuted.fetch_add(1, std::memory_order_relaxed);

    constexpr float minimumMagnitude = 1.0e-12F;
    const float displayRange = m_maximumDecibels - m_minimumDecibels;
    std::vector<float> normalized;
    normalized.reserve(magnitudes.size());
    for (const float magnitude : magnitudes) {
        const float safeMagnitude = std::isfinite(magnitude)
                                        ? std::max(magnitude, minimumMagnitude)
                                        : minimumMagnitude;
        const float decibels = 20.0F * std::log10(
            safeMagnitude / m_magnitudeNormalization);
        normalized.push_back(std::clamp(
            (decibels - m_minimumDecibels) / displayRange, 0.0F, 1.0F));
    }

    m_outputQueue->push({
        .sequence = m_nextSequence++,
        .timestampNanoseconds = timestampNanoseconds,
        .centerFrequency = centerFrequency,
        .sampleRate = sampleRate,
        .captureSpan = sampleRate,
        .fftSize = m_fftSize,
        .tuningGeneration = tuningGeneration,
        .normalizedMagnitudes = std::move(normalized),
    });
    m_counters->framesPublished.fetch_add(1, std::memory_order_relaxed);
    const auto elapsed = std::chrono::steady_clock::now() - processingStarted;
    m_counters->processingNanoseconds.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        std::memory_order_relaxed);
    return true;
}

}  // namespace sdr::dsp
