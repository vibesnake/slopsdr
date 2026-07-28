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
    std::uint32_t requestedRowsPerSecond,
    double achievableRowsPerSecond,
    double fractionalMilliseconds) noexcept;

class WaterfallFrameDelivery final
{
public:
    explicit WaterfallFrameDelivery(
        std::size_t capacity = 64, std::size_t prefillRows = 5);

    void setCapacity(std::size_t capacity);
    void reset(std::uint64_t sampleRate, std::size_t fftSize = 0);
    void stop();
    [[nodiscard]] bool enqueue(radio::SpectrumFrame frame);
    [[nodiscard]] std::optional<radio::SpectrumFrame> takeNextRow();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] const WaterfallDeliveryMetrics& metrics() const noexcept;

private:
    std::size_t m_capacity;
    const std::size_t m_prefillRows;
    std::deque<radio::SpectrumFrame> m_frames;
    WaterfallDeliveryMetrics m_metrics;
    std::uint64_t m_sampleRate = 0;
    std::size_t m_fftSize = 0;
    std::uint64_t m_lastEnqueuedSequence = 0;
    std::uint64_t m_lastEnqueuedTimestampNanoseconds = 0;
    bool m_active = false;
    bool m_prefilled = false;
};

}  // namespace sdr::app
