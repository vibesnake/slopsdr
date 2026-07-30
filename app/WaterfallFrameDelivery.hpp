// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "SpectrumFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace sdr::app {

struct WaterfallDeliveryMetrics {
    std::uint64_t rowsConsumed = 0;
    std::uint64_t overflowDrops = 0;
    std::uint64_t coalescedRows = 0;
    std::uint64_t staleGenerationDrops = 0;
    std::uint64_t displayUnderruns = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t duplicateRows = 0;
    std::uint64_t nonMonotonicTimestamps = 0;
    std::uint64_t lastProducedIntervalNanoseconds = 0;
};

struct WaterfallPresentationInterval {
    int milliseconds = 1;
    double fractionalMilliseconds = 0.0;
};

[[nodiscard]] WaterfallPresentationInterval nextWaterfallPresentationInterval(
    double requestedRowsPerSecond,
    double achievableRowsPerSecond,
    double fractionalMilliseconds) noexcept;

class WaterfallFrameDelivery final
{
public:
    explicit WaterfallFrameDelivery(std::size_t capacity = 64);

    void setCapacity(std::size_t capacity);
    void reset(std::uint64_t sampleRate, std::size_t fftSize = 0);
    void stop();
    [[nodiscard]] bool enqueue(radio::SpectrumFrame frame);
    // Retain real FIFO rows during normal delivery. A large post-stall backlog
    // collapses to newest data to bound latency and catch-up work.
    [[nodiscard]] std::optional<radio::SpectrumFrame> takeNextRow();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] const WaterfallDeliveryMetrics& metrics() const noexcept;

private:
    std::size_t m_capacity;
    std::deque<radio::SpectrumFrame> m_frames;
    WaterfallDeliveryMetrics m_metrics;
    std::uint64_t m_sampleRate = 0;
    std::size_t m_fftSize = 0;
    std::uint64_t m_captureSpan = 0;
    std::uint64_t m_centerFrequency = 0;
    std::uint64_t m_tuningGeneration = 0;
    std::uint64_t m_lastEnqueuedSequence = 0;
    std::uint64_t m_lastEnqueuedTimestampNanoseconds = 0;
    bool m_active = false;
    bool m_mappingInitialized = false;
};

}  // namespace sdr::app
