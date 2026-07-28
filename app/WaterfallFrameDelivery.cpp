// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WaterfallFrameDelivery.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sdr::app {

WaterfallPresentationInterval nextWaterfallPresentationInterval(
    std::uint32_t requestedRowsPerSecond,
    double achievableRowsPerSecond,
    double fractionalMilliseconds) noexcept
{
    double presentationRate =
        static_cast<double>(std::max<std::uint32_t>(1, requestedRowsPerSecond));
    if (std::isfinite(achievableRowsPerSecond) &&
        achievableRowsPerSecond > 0.0) {
        presentationRate = std::min(presentationRate, achievableRowsPerSecond);
    }
    const double boundedFraction =
        std::isfinite(fractionalMilliseconds)
            ? std::clamp(fractionalMilliseconds, 0.0, 1.0)
            : 0.0;
    const double exactInterval = 1'000.0 / presentationRate + boundedFraction;
    if (exactInterval >=
        static_cast<double>(std::numeric_limits<int>::max())) {
        return {
            .milliseconds = std::numeric_limits<int>::max(),
            .fractionalMilliseconds = 0.0,
        };
    }
    const int interval = std::max(
        1, static_cast<int>(std::floor(exactInterval)));
    return {
        .milliseconds = interval,
        .fractionalMilliseconds =
            exactInterval - static_cast<double>(interval),
    };
}

WaterfallFrameDelivery::WaterfallFrameDelivery(
    std::size_t capacity, std::size_t prefillRows)
    : m_capacity(capacity)
    , m_prefillRows(std::min(prefillRows, capacity))
{
    if (capacity == 0) {
        throw std::invalid_argument("Waterfall delivery capacity must be positive");
    }
}

void WaterfallFrameDelivery::setCapacity(std::size_t capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Waterfall delivery capacity must be positive");
    }
    m_capacity = capacity;
    while (m_frames.size() > m_capacity) {
        m_frames.pop_front();
        ++m_metrics.overflowDrops;
    }
}

void WaterfallFrameDelivery::reset(
    std::uint64_t sampleRate, std::size_t fftSize)
{
    m_frames.clear();
    m_sampleRate = sampleRate;
    m_fftSize = fftSize;
    m_lastEnqueuedSequence = 0;
    m_lastEnqueuedTimestampNanoseconds = 0;
    m_metrics.lastProducedIntervalNanoseconds = 0;
    m_active = sampleRate > 0;
    m_prefilled = m_prefillRows == 0;
}

void WaterfallFrameDelivery::stop()
{
    m_frames.clear();
    m_active = false;
    m_prefilled = false;
    m_sampleRate = 0;
    m_fftSize = 0;
    m_lastEnqueuedSequence = 0;
    m_lastEnqueuedTimestampNanoseconds = 0;
    m_metrics.lastProducedIntervalNanoseconds = 0;
}

bool WaterfallFrameDelivery::enqueue(radio::SpectrumFrame frame)
{
    if (!m_active || !radio::hasConsistentMetadata(frame) ||
        frame.timestampNanoseconds == 0 ||
        frame.sampleRate != m_sampleRate ||
        (m_fftSize != 0 && frame.fftSize != m_fftSize)) {
        return false;
    }
    if (m_fftSize == 0) {
        m_fftSize = frame.fftSize;
    }
    if (m_lastEnqueuedSequence != 0) {
        if (frame.sequence == m_lastEnqueuedSequence) {
            ++m_metrics.duplicateRows;
            return false;
        }
        if (frame.sequence < m_lastEnqueuedSequence) {
            ++m_metrics.duplicateRows;
            return false;
        }
    }
    if (frame.timestampNanoseconds != 0 &&
        m_lastEnqueuedTimestampNanoseconds != 0) {
        if (frame.timestampNanoseconds <=
            m_lastEnqueuedTimestampNanoseconds) {
            ++m_metrics.nonMonotonicTimestamps;
            return false;
        }
        m_metrics.lastProducedIntervalNanoseconds =
            frame.timestampNanoseconds - m_lastEnqueuedTimestampNanoseconds;
    }
    if (m_lastEnqueuedSequence != 0 &&
        frame.sequence > m_lastEnqueuedSequence + 1) {
        m_metrics.sequenceGaps +=
            frame.sequence - m_lastEnqueuedSequence - 1;
    }
    m_lastEnqueuedSequence = frame.sequence;
    if (frame.timestampNanoseconds != 0) {
        m_lastEnqueuedTimestampNanoseconds = frame.timestampNanoseconds;
    }
    if (m_frames.size() == m_capacity) {
        m_frames.pop_front();
        ++m_metrics.overflowDrops;
    }
    m_frames.push_back(std::move(frame));
    if (!m_prefilled && m_frames.size() >= m_prefillRows) {
        m_prefilled = true;
    }
    return true;
}

std::optional<radio::SpectrumFrame> WaterfallFrameDelivery::takeNextRow()
{
    if (!m_active || !m_prefilled) {
        return std::nullopt;
    }
    if (m_frames.empty()) {
        ++m_metrics.displayUnderruns;
        return std::nullopt;
    }
    radio::SpectrumFrame frame = std::move(m_frames.front());
    m_frames.pop_front();
    ++m_metrics.rowsConsumed;
    return frame;
}

bool WaterfallFrameDelivery::active() const noexcept
{
    return m_active;
}

std::size_t WaterfallFrameDelivery::size() const noexcept
{
    return m_frames.size();
}

std::size_t WaterfallFrameDelivery::capacity() const noexcept
{
    return m_capacity;
}

const WaterfallDeliveryMetrics& WaterfallFrameDelivery::metrics() const noexcept
{
    return m_metrics;
}

}  // namespace sdr::app
