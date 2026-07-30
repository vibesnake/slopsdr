// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "FrequencyMapping.hpp"
#include "SpectrumFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace sdr::gui {

enum class WaterfallAggregation {
    Original,
    Average,
};

[[nodiscard]] std::size_t displayColumnCountForWidth(
    double logicalWidth,
    double devicePixelRatio) noexcept;

[[nodiscard]] std::vector<float> projectFrameToFrequencyAxis(
    const radio::SpectrumFrame& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel = 0.0F);

void projectMaximumHoldToFrequencyAxis(
    std::span<const float> holdDbfs,
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    const radio::FrequencyAxisMapper& targetAxis,
    float emptyDbfs,
    std::span<float> projected);

struct WaterfallHistoryRow {
    std::uint64_t sequence = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::uint64_t centerFrequency = 0;
    std::uint64_t sampleRate = 0;
    std::uint64_t captureSpan = 0;
    std::size_t fftSize = 0;
    std::uint64_t tuningGeneration = 0;
    std::vector<std::uint16_t> normalizedMagnitudes;
    std::vector<float> linearPowerSums;
};

inline constexpr std::size_t viewportWaterfallHistoryMemoryBudgetBytes =
    8U * 1'024U * 1'024U;
inline constexpr std::size_t viewportWaterfallHistoryMaximumRows = 512;

struct WaterfallViewportDescriptor {
    std::uint64_t generation = 0;
    radio::FrequencyRange visibleRange;
    std::size_t physicalWidth = 0;
    double devicePixelRatio = 1.0;
    std::uint64_t captureCenterFrequency = 0;
    std::uint64_t captureSampleRate = 0;
    std::uint64_t captureSpan = 0;
    std::size_t captureFftSize = 0;
    std::uint64_t tuningGeneration = 0;
};

struct ViewportWaterfallHistoryRow {
    std::uint64_t sequence = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::uint64_t viewportGeneration = 0;
    radio::FrequencyRange visibleRange;
    std::size_t physicalWidth = 0;
    double devicePixelRatio = 1.0;
    std::uint64_t captureCenterFrequency = 0;
    std::uint64_t captureSampleRate = 0;
    std::uint64_t captureSpan = 0;
    std::size_t captureFftSize = 0;
    std::uint64_t tuningGeneration = 0;
    std::vector<std::uint16_t> peakMagnitudes;
    std::vector<float> meanLinearPowers;
};

[[nodiscard]] bool sameWaterfallViewport(
    const WaterfallViewportDescriptor& left,
    const WaterfallViewportDescriptor& right) noexcept;

[[nodiscard]] bool viewportWaterfallRowMatches(
    const ViewportWaterfallHistoryRow& row,
    const WaterfallViewportDescriptor& viewport) noexcept;

[[nodiscard]] std::optional<ViewportWaterfallHistoryRow>
projectFrameToWaterfallViewport(
    const radio::SpectrumFrame& frame,
    const WaterfallViewportDescriptor& viewport);

struct WaterfallVerticalSample {
    std::size_t firstRow = 0;
    std::size_t lastRow = 0;
    std::size_t newerRow = 0;
    std::size_t olderRow = 0;
    double intervalStartAgeNanoseconds = 0.0;
    double intervalEndAgeNanoseconds = 0.0;
    float interpolation = 0.0F;
    bool reduce = false;
    bool interpolate = false;
    bool hasData = false;
};

struct WaterfallRasterGeometry {
    std::size_t physicalWidth = 0;
    std::size_t visiblePixelRows = 0;
    std::size_t stagingPixelRows = 0;
};

struct WaterfallHistoryPlan {
    std::size_t sourceBins = 0;
    std::size_t minimumStoredBins = 0;
    std::size_t storedBins = 0;
    std::size_t requiredRows = 0;
    std::size_t maximumRowsWithinBudget = 0;
    std::size_t requiredMemoryBytes = 0;
    double requestedDurationSeconds = 0.0;
    double retainedCapacitySeconds = 0.0;
    bool fitsMemoryBudget = false;
};

[[nodiscard]] WaterfallHistoryPlan selectWaterfallHistoryPlan(
    std::size_t sourceBins,
    std::size_t physicalWidth,
    std::size_t physicalHeight,
    double sourceRowsPerSecond,
    double requestedDurationSeconds,
    std::size_t memoryBudgetBytes) noexcept;

[[nodiscard]] std::size_t waterfallStagingPixelRows(
    std::size_t visiblePixelRows,
    double rowsPerSecond,
    double visibleHistorySeconds) noexcept;

[[nodiscard]] WaterfallRasterGeometry waterfallRasterGeometry(
    double logicalWidth,
    double logicalHeight,
    double devicePixelRatio,
    double rowsPerSecond,
    double visibleHistorySeconds) noexcept;

[[nodiscard]] std::uint64_t waterfallStagingDurationNanoseconds(
    std::size_t visiblePhysicalRows,
    std::size_t stagingPhysicalRows,
    double visibleHistorySeconds) noexcept;

[[nodiscard]] std::uint64_t waterfallRenderTimestamp(
    std::uint64_t initialRenderTimestampNanoseconds,
    std::uint64_t renderClockOriginNanoseconds,
    std::uint64_t renderClockNowNanoseconds) noexcept;

[[nodiscard]] std::uint64_t observedWaterfallRowIntervalNanoseconds(
    const std::deque<WaterfallHistoryRow>& rows) noexcept;

[[nodiscard]] std::uint64_t clampWaterfallRenderTimestamp(
    std::uint64_t renderTimestampNanoseconds,
    const std::deque<WaterfallHistoryRow>& rows) noexcept;

[[nodiscard]] std::vector<WaterfallVerticalSample> mapWaterfallRowsToPixels(
    const std::deque<WaterfallHistoryRow>& rows,
    std::uint64_t anchorTimestampNanoseconds,
    double visibleHistorySeconds,
    std::size_t visiblePixelRows);

[[nodiscard]] double waterfallTemporalWeightNanoseconds(
    const std::deque<WaterfallHistoryRow>& rows,
    std::size_t rowIndex,
    std::uint64_t anchorTimestampNanoseconds,
    double intervalStartAgeNanoseconds,
    double intervalEndAgeNanoseconds) noexcept;

[[nodiscard]] double waterfallFractionalScrollPixels(
    std::uint64_t elapsedNanoseconds,
    double visibleHistorySeconds,
    double pixelHeight) noexcept;

[[nodiscard]] std::vector<float> projectFrameToFrequencyAxis(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel = 0.0F);

// Waterfall rows remain discrete intensity samples at every zoom level.
[[nodiscard]] std::vector<float> projectWaterfallRowToFrequencyAxis(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel = 0.0F);

// Average rows remain in linear power until temporal aggregation is complete.
[[nodiscard]] std::vector<float> projectAverageWaterfallRowToFrequencyAxis(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyPower = 0.0F);

[[nodiscard]] float linearPowerForNormalizedMagnitude(
    float normalizedMagnitude) noexcept;
[[nodiscard]] float normalizedMagnitudeForLinearPower(
    float linearPower) noexcept;

void combineWaterfallFrames(
    std::span<float> accumulated,
    std::span<const float> additional,
    WaterfallAggregation aggregation) noexcept;
void finishWaterfallFrameAggregation(
    std::span<float> accumulated,
    std::size_t contributingFrames,
    WaterfallAggregation aggregation) noexcept;

class WaterfallHistoryBuffer final
{
public:
    explicit WaterfallHistoryBuffer(
        std::size_t capacity,
        std::size_t memoryBudgetBytes = 16U * 1'024U * 1'024U);

    void setCapacity(std::size_t capacity);
    void setMemoryBudgetBytes(std::size_t memoryBudgetBytes);
    void setRetentionDurationSeconds(double seconds);
    void setStoredBinCount(std::size_t storedBinCount);
    [[nodiscard]] bool append(radio::SpectrumFrame frame);
    void clear();

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t memoryBudgetBytes() const noexcept;
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept;
    [[nodiscard]] double retentionDurationSeconds() const noexcept;
    [[nodiscard]] double retainedDurationSeconds() const noexcept;
    [[nodiscard]] std::size_t storedBinCount() const noexcept;
    [[nodiscard]] const std::deque<WaterfallHistoryRow>& rows() const noexcept;

private:
    std::size_t m_capacity;
    std::size_t m_memoryBudgetBytes;
    std::size_t m_memoryUsageBytes = 0;
    std::size_t m_storedBinCount = 2'048;
    double m_retentionDurationSeconds = 10.0;
    std::deque<WaterfallHistoryRow> m_rows;
};

class ViewportWaterfallHistoryBuffer final
{
public:
    explicit ViewportWaterfallHistoryBuffer(
        std::size_t capacity = viewportWaterfallHistoryMaximumRows,
        std::size_t memoryBudgetBytes =
            viewportWaterfallHistoryMemoryBudgetBytes);

    void setCapacity(std::size_t capacity);
    void setMemoryBudgetBytes(std::size_t memoryBudgetBytes);
    [[nodiscard]] bool append(ViewportWaterfallHistoryRow row);
    void clear();

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t memoryBudgetBytes() const noexcept;
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept;
    [[nodiscard]] const std::deque<ViewportWaterfallHistoryRow>& rows()
        const noexcept;

private:
    std::size_t m_capacity;
    std::size_t m_memoryBudgetBytes;
    std::size_t m_memoryUsageBytes = 0;
    std::deque<ViewportWaterfallHistoryRow> m_rows;
};

}  // namespace sdr::gui
