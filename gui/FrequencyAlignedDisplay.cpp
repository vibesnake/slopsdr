// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FrequencyAlignedDisplay.hpp"

#include "SpectrumAmplitudeScale.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sdr::gui {
namespace {

std::size_t rowMemoryBytes(const WaterfallHistoryRow& row) noexcept
{
    return sizeof(WaterfallHistoryRow) +
           row.normalizedMagnitudes.capacity() * sizeof(std::uint16_t) +
           row.linearPowerSums.capacity() * sizeof(float);
}

std::size_t rowMemoryBytes(
    const ViewportWaterfallHistoryRow& row) noexcept
{
    return sizeof(ViewportWaterfallHistoryRow) +
           row.peakMagnitudes.capacity() * sizeof(std::uint16_t) +
           row.meanLinearPowers.capacity() * sizeof(float);
}

std::size_t roundedUpHistoryBins(std::size_t bins) noexcept
{
    constexpr std::size_t granularity = 256;
    if (bins > std::numeric_limits<std::size_t>::max() - (granularity - 1)) {
        return bins;
    }
    return ((bins + granularity - 1) / granularity) * granularity;
}

std::size_t rowMemoryBytes(
    std::size_t sourceBins, std::size_t storedBins) noexcept
{
    const std::size_t bytesPerBin = sizeof(std::uint16_t) +
                                    (storedBins < sourceBins ? sizeof(float) : 0);
    if (storedBins >
        (std::numeric_limits<std::size_t>::max() - sizeof(WaterfallHistoryRow)) /
            bytesPerBin) {
        return std::numeric_limits<std::size_t>::max();
    }
    return sizeof(WaterfallHistoryRow) +
           storedBins * bytesPerBin;
}

float interpolateAt(std::span<const float> values, double position) noexcept
{
    const double bounded = std::clamp(
        position, 0.0, static_cast<double>(values.size() - 1));
    const auto lower = static_cast<std::size_t>(std::floor(bounded));
    const auto upper = std::min(lower + 1, values.size() - 1);
    return std::lerp(
        values[lower],
        values[upper],
        static_cast<float>(bounded - static_cast<double>(lower)));
}

float interpolateAt(
    std::span<const std::uint16_t> values, double position) noexcept
{
    const double bounded = std::clamp(
        position, 0.0, static_cast<double>(values.size() - 1));
    const auto lower = static_cast<std::size_t>(std::floor(bounded));
    const auto upper = std::min(lower + 1, values.size() - 1);
    constexpr float scale = 1.0F / 65'535.0F;
    return std::lerp(
        static_cast<float>(values[lower]) * scale,
        static_cast<float>(values[upper]) * scale,
        static_cast<float>(bounded - static_cast<double>(lower)));
}

template <typename Frame, typename Magnitude>
std::vector<float> projectFrame(
    const Frame& frame,
    std::span<const Magnitude> magnitudes,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel)
{
    if (frame.sequence == 0 || frame.sampleRate == 0 || frame.fftSize < 2 ||
        magnitudes.size() < 2 || !targetAxis.valid() || displayColumns < 2) {
        return {};
    }

    std::vector<float> projected(displayColumns, emptyLevel);
    const radio::FftBinFrequencyMapper sourceAxis(
        frame.centerFrequency,
        frame.captureSpan == 0 ? frame.sampleRate : frame.captureSpan,
        {0.0, static_cast<double>(magnitudes.size() - 1)});
    if (!sourceAxis.valid()) {
        return projected;
    }

    const auto targetRange = targetAxis.visibleRange();
    const double sourceLower = sourceAxis.nominalLowerFrequency();
    const double sourceUpper = sourceAxis.nominalUpperFrequency();
    const double targetStepHz =
        static_cast<double>(targetRange.maximum - targetRange.minimum) /
        static_cast<double>(displayColumns - 1);
    const double sourceBinHz =
        (sourceUpper - sourceLower) /
        static_cast<double>(magnitudes.size() - 1);
    const bool reducing = targetStepHz > sourceBinHz * (1.0 + 1.0e-9);
    const auto plot = targetAxis.plot();

    for (std::size_t column = 0; column < displayColumns; ++column) {
        const double targetPosition =
            plot.left + plot.width * static_cast<double>(column) /
                            static_cast<double>(displayColumns - 1);
        const auto frequency = targetAxis.frequencyForPosition(targetPosition);
        if (!frequency.has_value()) {
            continue;
        }
        const auto sourcePosition = sourceAxis.positionForFrequency(*frequency);
        if (!sourcePosition.has_value()) {
            continue;
        }

        if (!reducing) {
            projected[column] = interpolateAt(magnitudes, *sourcePosition);
            continue;
        }

        const double halfTargetStep = targetStepHz / 2.0;
        const double firstFrequency = std::max(
            *frequency - halfTargetStep,
            sourceLower);
        const double lastFrequency = std::min(
            *frequency + halfTargetStep,
            sourceUpper);
        const double firstPosition =
            sourceAxis.positionForFrequency(firstFrequency).value_or(*sourcePosition);
        const double lastPosition =
            sourceAxis.positionForFrequency(lastFrequency).value_or(*sourcePosition);
        const auto firstBin = static_cast<std::size_t>(std::ceil(firstPosition));
        const auto lastBin = static_cast<std::size_t>(std::floor(lastPosition));
        float peak = interpolateAt(magnitudes, *sourcePosition);
        if (firstBin <= lastBin && firstBin < magnitudes.size()) {
            const auto maximum = *std::max_element(
                magnitudes.begin() + static_cast<std::ptrdiff_t>(firstBin),
                magnitudes.begin() + static_cast<std::ptrdiff_t>(
                    std::min(lastBin + 1, magnitudes.size())));
            if constexpr (std::is_same_v<Magnitude, std::uint16_t>) {
                peak = std::max(peak, static_cast<float>(maximum) / 65'535.0F);
            } else {
                peak = std::max(peak, maximum);
            }
        }
        projected[column] = peak;
    }
    return projected;
}

template <typename Magnitude>
float normalizedMagnitude(Magnitude magnitude) noexcept
{
    if constexpr (std::is_same_v<Magnitude, std::uint16_t>) {
        return static_cast<float>(magnitude) / 65'535.0F;
    } else {
        return magnitude;
    }
}

template <typename Magnitude>
float nearestMagnitude(
    std::span<const Magnitude> magnitudes, double position) noexcept
{
    const double bounded = std::clamp(
        position, 0.0, static_cast<double>(magnitudes.size() - 1));
    constexpr double binBoundaryTolerance = 1.0e-9;
    const auto index = static_cast<std::size_t>(
        std::floor(bounded + 0.5 + binBoundaryTolerance));
    return normalizedMagnitude(magnitudes[index]);
}

template <typename Magnitude>
float directMagnitude(
    std::span<const Magnitude> magnitudes, double position) noexcept
{
    const double bounded = std::clamp(
        position, 0.0, static_cast<double>(magnitudes.size() - 1));
    constexpr double binBoundaryTolerance = 1.0e-9;
    const auto index = static_cast<std::size_t>(
        std::floor(bounded + 0.5 + binBoundaryTolerance));
    return normalizedMagnitude(magnitudes[index]);
}

template <typename Magnitude>
std::vector<float> projectWaterfallRow(
    const WaterfallHistoryRow& frame,
    std::span<const Magnitude> magnitudes,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel)
{
    if (frame.sequence == 0 || frame.sampleRate == 0 || frame.fftSize < 2 ||
        magnitudes.size() < 2 || !targetAxis.valid() || displayColumns < 2) {
        return {};
    }

    std::vector<float> projected(displayColumns, emptyLevel);
    const radio::FftBinFrequencyMapper sourceAxis(
        frame.centerFrequency,
        frame.captureSpan == 0 ? frame.sampleRate : frame.captureSpan,
        {0.0, static_cast<double>(magnitudes.size() - 1)});
    if (!sourceAxis.valid()) {
        return projected;
    }

    const auto targetRange = targetAxis.visibleRange();
    const double sourceLower = sourceAxis.nominalLowerFrequency();
    const double sourceUpper = sourceAxis.nominalUpperFrequency();
    const double targetStepHz =
        static_cast<double>(targetRange.maximum - targetRange.minimum) /
        static_cast<double>(displayColumns - 1);
    const double sourceBinHz =
        (sourceUpper - sourceLower) /
        static_cast<double>(magnitudes.size() - 1);
    const double sourceBinsPerPixel = targetStepHz / sourceBinHz;
    const auto plot = targetAxis.plot();

    for (std::size_t column = 0; column < displayColumns; ++column) {
        const double targetPosition =
            plot.left + plot.width * static_cast<double>(column) /
                            static_cast<double>(displayColumns - 1);
        const auto frequency = targetAxis.frequencyForPosition(targetPosition);
        if (!frequency.has_value()) {
            continue;
        }
        const auto sourcePosition = sourceAxis.positionForFrequency(*frequency);
        if (!sourcePosition.has_value()) {
            continue;
        }

        if (sourceBinsPerPixel > 1.0 + 1.0e-9) {
            const double halfTargetStep = targetStepHz / 2.0;
            const double firstFrequency = std::max(
                *frequency - halfTargetStep,
                sourceLower);
            const double lastFrequency = std::min(
                *frequency + halfTargetStep,
                sourceUpper);
            const double firstPosition =
                sourceAxis.positionForFrequency(firstFrequency).value_or(*sourcePosition);
            const double lastPosition =
                sourceAxis.positionForFrequency(lastFrequency).value_or(*sourcePosition);
            const auto firstBin = static_cast<std::size_t>(std::ceil(firstPosition));
            const auto lastBin = static_cast<std::size_t>(std::floor(lastPosition));
            float peak = nearestMagnitude(magnitudes, *sourcePosition);
            if (firstBin <= lastBin && firstBin < magnitudes.size()) {
                const auto maximum = *std::max_element(
                    magnitudes.begin() + static_cast<std::ptrdiff_t>(firstBin),
                    magnitudes.begin() + static_cast<std::ptrdiff_t>(
                        std::min(lastBin + 1, magnitudes.size())));
                peak = std::max(peak, normalizedMagnitude(maximum));
            }
            projected[column] = peak;
            continue;
        }

        constexpr double directBinTolerance = 0.05;
        if (sourceBinsPerPixel >= 1.0 - directBinTolerance) {
            // Near native resolution maps directly to one retained intensity bin.
            projected[column] = directMagnitude(magnitudes, *sourcePosition);
        } else {
            // Deep zoom holds the nearest bin rather than inventing intensities.
            projected[column] = nearestMagnitude(magnitudes, *sourcePosition);
        }
    }
    return projected;
}

std::size_t contributingSourceBins(
    const WaterfallHistoryRow& frame, std::size_t retainedBin) noexcept
{
    const std::size_t retainedBins = frame.normalizedMagnitudes.size();
    if (retainedBins == 0 || retainedBin >= retainedBins) {
        return 0;
    }
    const std::size_t first = retainedBin * frame.fftSize / retainedBins;
    const std::size_t onePastLast = std::max(
        first + 1, (retainedBin + 1) * frame.fftSize / retainedBins);
    return onePastLast - first;
}

float retainedLinearPowerSum(
    const WaterfallHistoryRow& frame, std::size_t retainedBin) noexcept
{
    if (frame.linearPowerSums.size() == frame.normalizedMagnitudes.size()) {
        return frame.linearPowerSums[retainedBin];
    }
    return linearPowerForNormalizedMagnitude(
               normalizedMagnitude(frame.normalizedMagnitudes[retainedBin])) *
           static_cast<float>(contributingSourceBins(frame, retainedBin));
}

float retainedMeanLinearPower(
    const WaterfallHistoryRow& frame, std::size_t retainedBin) noexcept
{
    const std::size_t count = contributingSourceBins(frame, retainedBin);
    return count == 0
               ? 0.0F
               : retainedLinearPowerSum(frame, retainedBin) /
                     static_cast<float>(count);
}

std::vector<float> projectAverageWaterfallRow(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyPower)
{
    const auto& magnitudes = frame.normalizedMagnitudes;
    if (frame.sequence == 0 || frame.sampleRate == 0 || frame.fftSize < 2 ||
        magnitudes.size() < 2 || !targetAxis.valid() || displayColumns < 2 ||
        (!frame.linearPowerSums.empty() &&
         frame.linearPowerSums.size() != magnitudes.size())) {
        return {};
    }

    std::vector<float> projected(displayColumns, emptyPower);
    const radio::FftBinFrequencyMapper sourceAxis(
        frame.centerFrequency,
        frame.captureSpan == 0 ? frame.sampleRate : frame.captureSpan,
        {0.0, static_cast<double>(magnitudes.size() - 1)});
    if (!sourceAxis.valid()) {
        return projected;
    }

    const auto targetRange = targetAxis.visibleRange();
    const double sourceLower = sourceAxis.nominalLowerFrequency();
    const double sourceUpper = sourceAxis.nominalUpperFrequency();
    const double targetStepHz =
        static_cast<double>(targetRange.maximum - targetRange.minimum) /
        static_cast<double>(displayColumns - 1);
    const double sourceBinHz =
        (sourceUpper - sourceLower) /
        static_cast<double>(magnitudes.size() - 1);
    const double sourceBinsPerPixel = targetStepHz / sourceBinHz;
    const auto plot = targetAxis.plot();

    for (std::size_t column = 0; column < displayColumns; ++column) {
        const double targetPosition =
            plot.left + plot.width * static_cast<double>(column) /
                            static_cast<double>(displayColumns - 1);
        const auto frequency = targetAxis.frequencyForPosition(targetPosition);
        if (!frequency.has_value()) {
            continue;
        }
        const auto sourcePosition = sourceAxis.positionForFrequency(*frequency);
        if (!sourcePosition.has_value()) {
            continue;
        }
        const auto nearest = static_cast<std::size_t>(std::floor(std::clamp(
            *sourcePosition,
            0.0,
            static_cast<double>(magnitudes.size() - 1)) + 0.5 + 1.0e-9));

        if (sourceBinsPerPixel <= 1.0 + 1.0e-9) {
            projected[column] = retainedMeanLinearPower(frame, nearest);
            continue;
        }

        const double halfTargetStep = targetStepHz / 2.0;
        const double firstFrequency = std::max(
            *frequency - halfTargetStep,
            sourceLower);
        const double lastFrequency = std::min(
            *frequency + halfTargetStep,
            sourceUpper);
        const double firstPosition =
            sourceAxis.positionForFrequency(firstFrequency).value_or(*sourcePosition);
        const double lastPosition =
            sourceAxis.positionForFrequency(lastFrequency).value_or(*sourcePosition);
        const auto firstBin = static_cast<std::size_t>(std::ceil(firstPosition));
        const auto lastBin = static_cast<std::size_t>(std::floor(lastPosition));
        if (firstBin > lastBin || firstBin >= magnitudes.size()) {
            projected[column] = retainedMeanLinearPower(frame, nearest);
            continue;
        }

        double powerSum = 0.0;
        std::size_t binCount = 0;
        const std::size_t boundedLast = std::min(lastBin, magnitudes.size() - 1);
        for (std::size_t bin = firstBin; bin <= boundedLast; ++bin) {
            powerSum += retainedLinearPowerSum(frame, bin);
            binCount += contributingSourceBins(frame, bin);
        }
        projected[column] = binCount == 0
                                ? retainedMeanLinearPower(frame, nearest)
                                : static_cast<float>(
                                      powerSum / static_cast<double>(binCount));
    }
    return projected;
}

}  // namespace

WaterfallHistoryPlan selectWaterfallHistoryPlan(
    std::size_t sourceBins,
    std::size_t physicalWidth,
    std::size_t physicalHeight,
    double sourceRowsPerSecond,
    double requestedDurationSeconds,
    std::size_t memoryBudgetBytes) noexcept
{
    WaterfallHistoryPlan plan{
        .sourceBins = sourceBins,
        .requestedDurationSeconds = requestedDurationSeconds,
    };
    if (sourceBins < 2 || physicalWidth == 0 || physicalHeight == 0 ||
        !std::isfinite(sourceRowsPerSecond) || sourceRowsPerSecond <= 0.0 ||
        !std::isfinite(requestedDurationSeconds) ||
        requestedDurationSeconds <= 0.0 || memoryBudgetBytes == 0) {
        return plan;
    }

    constexpr std::size_t normalMaximumBins = 8'192;
    constexpr std::size_t extendedMaximumBins = 16'384;
    constexpr std::size_t minimumUsefulBins = 256;
    const std::size_t displayPreservingBins = std::min(
        sourceBins,
        roundedUpHistoryBins(std::max(minimumUsefulBins, physicalWidth)));
    const std::size_t normalLimit = displayPreservingBins <= normalMaximumBins
                                        ? normalMaximumBins
                                        : std::max(
                                              displayPreservingBins,
                                              extendedMaximumBins);
    const std::size_t preferredBins = std::min(sourceBins, normalLimit);

    const std::size_t stagingRows = waterfallStagingPixelRows(
        physicalHeight, sourceRowsPerSecond, requestedDurationSeconds);
    const double stagingSeconds =
        static_cast<double>(stagingRows) * requestedDurationSeconds /
        static_cast<double>(physicalHeight);
    const double requiredRowSpan =
        (requestedDurationSeconds + stagingSeconds) *
        sourceRowsPerSecond;
    if (!std::isfinite(requiredRowSpan) ||
        requiredRowSpan >
            static_cast<double>(std::numeric_limits<std::size_t>::max() - 1)) {
        return plan;
    }
    plan.requiredRows =
        static_cast<std::size_t>(std::ceil(requiredRowSpan)) + 1;

    const std::size_t perRowBudget = memoryBudgetBytes / plan.requiredRows;
    std::size_t maximumBinsForDuration = 0;
    if (perRowBudget > sizeof(WaterfallHistoryRow)) {
        maximumBinsForDuration =
            (perRowBudget - sizeof(WaterfallHistoryRow)) /
            (sizeof(std::uint16_t) + sizeof(float));
    }
    plan.minimumStoredBins = std::min(
        displayPreservingBins,
        std::max<std::size_t>(2, maximumBinsForDuration));
    if (preferredBins == sourceBins &&
        rowMemoryBytes(sourceBins, sourceBins) <= perRowBudget) {
        plan.storedBins = sourceBins;
    } else if (maximumBinsForDuration >= 2) {
        plan.storedBins = std::min(preferredBins, maximumBinsForDuration);
        if (plan.storedBins >= 256) {
            plan.storedBins = (plan.storedBins / 256U) * 256U;
        }
        plan.storedBins = std::max<std::size_t>(2, plan.storedBins);
    }

    const std::size_t bytesPerRow = rowMemoryBytes(sourceBins, plan.storedBins);
    if (plan.storedBins < 2 ||
        bytesPerRow == std::numeric_limits<std::size_t>::max()) {
        return plan;
    }
    plan.maximumRowsWithinBudget = memoryBudgetBytes / bytesPerRow;
    if (plan.requiredRows <=
        std::numeric_limits<std::size_t>::max() / bytesPerRow) {
        plan.requiredMemoryBytes = plan.requiredRows * bytesPerRow;
    } else {
        plan.requiredMemoryBytes = std::numeric_limits<std::size_t>::max();
    }
    plan.fitsMemoryBudget =
        plan.requiredRows <= plan.maximumRowsWithinBudget;
    const std::size_t retainedRows =
        std::min(plan.requiredRows, plan.maximumRowsWithinBudget);
    const double stagingRowEquivalent =
        stagingSeconds * sourceRowsPerSecond;
    plan.retainedCapacitySeconds = std::clamp(
        (std::max(
             0.0,
             static_cast<double>(retainedRows > 0 ? retainedRows - 1 : 0) -
                 stagingRowEquivalent)) /
            sourceRowsPerSecond,
        0.0,
        requestedDurationSeconds);
    return plan;
}

std::size_t waterfallStagingPixelRows(
    std::size_t visiblePixelRows,
    double rowsPerSecond,
    double visibleHistorySeconds) noexcept
{
    if (visiblePixelRows == 0 || !std::isfinite(rowsPerSecond) ||
        rowsPerSecond <= 0.0 ||
        !std::isfinite(visibleHistorySeconds) ||
        visibleHistorySeconds <= 0.0) {
        return 0;
    }
    constexpr double minimumStagingSeconds = 0.1;
    constexpr double minimumStagingRowIntervals = 2.0;
    const double stagingSeconds = std::max(
        minimumStagingSeconds,
        minimumStagingRowIntervals / rowsPerSecond);
    return std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::ceil(
            static_cast<double>(visiblePixelRows) * stagingSeconds /
            visibleHistorySeconds)));
}

WaterfallRasterGeometry waterfallRasterGeometry(
    double logicalWidth,
    double logicalHeight,
    double devicePixelRatio,
    double rowsPerSecond,
    double visibleHistorySeconds) noexcept
{
    const double boundedRatio =
        std::isfinite(devicePixelRatio) ? std::max(1.0, devicePixelRatio) : 1.0;
    const std::size_t physicalWidth =
        displayColumnCountForWidth(logicalWidth, boundedRatio);
    const std::size_t visibleRows = static_cast<std::size_t>(std::max(
        1.0,
        std::ceil(
            std::max(0.0, std::isfinite(logicalHeight) ? logicalHeight : 0.0) *
            boundedRatio)));
    const std::size_t stagingRows = waterfallStagingPixelRows(
        visibleRows, rowsPerSecond, visibleHistorySeconds);
    return {
        .physicalWidth = physicalWidth,
        .visiblePixelRows = visibleRows,
        .stagingPixelRows = stagingRows,
    };
}

std::uint64_t waterfallStagingDurationNanoseconds(
    std::size_t visiblePhysicalRows,
    std::size_t stagingPhysicalRows,
    double visibleHistorySeconds) noexcept
{
    if (visiblePhysicalRows == 0 || stagingPhysicalRows == 0 ||
        !std::isfinite(visibleHistorySeconds) || visibleHistorySeconds <= 0.0) {
        return 0;
    }
    const long double duration =
        static_cast<long double>(stagingPhysicalRows) *
        static_cast<long double>(visibleHistorySeconds) * 1'000'000'000.0L /
        static_cast<long double>(visiblePhysicalRows);
    if (duration >=
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::llround(duration));
}

std::uint64_t waterfallRenderTimestamp(
    std::uint64_t initialRenderTimestampNanoseconds,
    std::uint64_t renderClockOriginNanoseconds,
    std::uint64_t renderClockNowNanoseconds) noexcept
{
    if (initialRenderTimestampNanoseconds == 0 ||
        renderClockOriginNanoseconds == 0) {
        return 0;
    }
    const std::uint64_t elapsed =
        renderClockNowNanoseconds > renderClockOriginNanoseconds
            ? renderClockNowNanoseconds - renderClockOriginNanoseconds
            : 0;
    if (initialRenderTimestampNanoseconds >
        std::numeric_limits<std::uint64_t>::max() - elapsed) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return initialRenderTimestampNanoseconds + elapsed;
}

std::uint64_t observedWaterfallRowIntervalNanoseconds(
    const std::deque<WaterfallHistoryRow>& rows) noexcept
{
    if (rows.size() < 2 ||
        rows.front().timestampNanoseconds <= rows[1].timestampNanoseconds) {
        return 0;
    }
    return rows.front().timestampNanoseconds -
           rows[1].timestampNanoseconds;
}

std::uint64_t clampWaterfallRenderTimestamp(
    std::uint64_t renderTimestampNanoseconds,
    const std::deque<WaterfallHistoryRow>& rows) noexcept
{
    if (rows.empty()) {
        return renderTimestampNanoseconds;
    }
    const std::uint64_t newest = rows.front().timestampNanoseconds;
    const std::uint64_t interval =
        observedWaterfallRowIntervalNanoseconds(rows);
    const std::uint64_t maximum =
        interval > std::numeric_limits<std::uint64_t>::max() - newest
            ? std::numeric_limits<std::uint64_t>::max()
            : newest + interval;
    return std::min(renderTimestampNanoseconds, maximum);
}

std::vector<WaterfallVerticalSample> mapWaterfallRowsToPixels(
    const std::deque<WaterfallHistoryRow>& rows,
    std::uint64_t anchorTimestampNanoseconds,
    double visibleHistorySeconds,
    std::size_t visiblePixelRows)
{
    std::vector<WaterfallVerticalSample> mapping(visiblePixelRows);
    if (rows.empty() || visiblePixelRows == 0 ||
        !std::isfinite(visibleHistorySeconds) ||
        visibleHistorySeconds <= 0.0 || anchorTimestampNanoseconds == 0) {
        return mapping;
    }
    const double nanosecondsPerPixel =
        visibleHistorySeconds * 1.0e9 /
        static_cast<double>(visiblePixelRows);
    auto age = [anchorTimestampNanoseconds](const WaterfallHistoryRow& row) {
        return row.timestampNanoseconds > anchorTimestampNanoseconds
                   ? 0.0
                   : static_cast<double>(
                         anchorTimestampNanoseconds - row.timestampNanoseconds);
    };
    std::vector<double> ages;
    ages.reserve(rows.size());
    for (const auto& row : rows) {
        ages.push_back(age(row));
    }
    for (std::size_t pixel = 0; pixel < visiblePixelRows; ++pixel) {
        const double intervalStart =
            nanosecondsPerPixel * static_cast<double>(pixel);
        const double intervalEnd =
            nanosecondsPerPixel * static_cast<double>(pixel + 1);
        const double center = (intervalStart + intervalEnd) * 0.5;
        const auto firstIterator =
            std::lower_bound(ages.begin(), ages.end(), intervalStart);
        const auto onePastLastIterator =
            std::lower_bound(firstIterator, ages.end(), intervalEnd);
        auto& sample = mapping[pixel];
        if (firstIterator != onePastLastIterator) {
            const std::size_t first = static_cast<std::size_t>(
                firstIterator - ages.begin());
            const std::size_t last = static_cast<std::size_t>(
                onePastLastIterator - ages.begin() - 1);
            sample.firstRow = first;
            sample.lastRow = last;
            sample.reduce = last > first;
            sample.hasData = true;
            continue;
        }

        if (center < ages.front()) {
            sample.firstRow = 0;
            sample.lastRow = 0;
            sample.newerRow = 0;
            sample.olderRow = 0;
            sample.hasData = true;
            continue;
        }
        if (center > ages.back()) {
            const std::size_t oldest = rows.size() - 1;
            sample.firstRow = oldest;
            sample.lastRow = oldest;
            sample.newerRow = oldest;
            sample.olderRow = oldest;
            sample.hasData = true;
            continue;
        }
        const auto olderIterator =
            std::lower_bound(ages.begin(), ages.end(), center);
        if (olderIterator != ages.begin() && olderIterator != ages.end()) {
            sample.olderRow = static_cast<std::size_t>(
                olderIterator - ages.begin());
            sample.newerRow = sample.olderRow - 1;
            const double newerAge = ages[sample.newerRow];
            const double olderAge = ages[sample.olderRow];
            sample.firstRow = center - newerAge <= olderAge - center
                                  ? sample.newerRow
                                  : sample.olderRow;
            if (olderAge > newerAge) {
                sample.interpolation = static_cast<float>(std::clamp(
                    (center - newerAge) / (olderAge - newerAge), 0.0, 1.0));
                sample.interpolate = true;
            }
            sample.hasData = true;
        }
    }
    return mapping;
}

double waterfallFractionalScrollPixels(
    std::uint64_t elapsedNanoseconds,
    double visibleHistorySeconds,
    double pixelHeight) noexcept
{
    if (!std::isfinite(visibleHistorySeconds) || visibleHistorySeconds <= 0.0 ||
        !std::isfinite(pixelHeight) || pixelHeight <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(elapsedNanoseconds) * pixelHeight /
           (visibleHistorySeconds * 1.0e9);
}

std::size_t displayColumnCountForWidth(
    double logicalWidth,
    double devicePixelRatio) noexcept
{
    if (!std::isfinite(logicalWidth) || !std::isfinite(devicePixelRatio)) {
        return 2;
    }
    return static_cast<std::size_t>(std::max(
        2.0, std::ceil(logicalWidth * std::max(1.0, devicePixelRatio))));
}

float linearPowerForNormalizedMagnitude(float normalizedMagnitude) noexcept
{
    const float dbfs = dbfsForNormalizedSpectrum(normalizedMagnitude);
    return std::pow(10.0F, dbfs / 10.0F);
}

float normalizedMagnitudeForLinearPower(float linearPower) noexcept
{
    if (!std::isfinite(linearPower) || linearPower <= 0.0F) {
        return 0.0F;
    }
    const float dbfs = 10.0F * std::log10(linearPower);
    return std::clamp(
        (dbfs - normalizedSpectrumFloorDbfs) /
            (normalizedSpectrumCeilingDbfs - normalizedSpectrumFloorDbfs),
        0.0F,
        1.0F);
}

bool sameWaterfallViewport(
    const WaterfallViewportDescriptor& left,
    const WaterfallViewportDescriptor& right) noexcept
{
    return left.generation == right.generation &&
           left.visibleRange == right.visibleRange &&
           left.physicalWidth == right.physicalWidth &&
           left.devicePixelRatio == right.devicePixelRatio &&
           left.captureCenterFrequency == right.captureCenterFrequency &&
           left.captureSampleRate == right.captureSampleRate &&
           left.captureSpan == right.captureSpan &&
           left.captureFftSize == right.captureFftSize &&
           left.tuningGeneration == right.tuningGeneration;
}

bool viewportWaterfallRowMatches(
    const ViewportWaterfallHistoryRow& row,
    const WaterfallViewportDescriptor& viewport) noexcept
{
    return row.viewportGeneration == viewport.generation &&
           row.visibleRange == viewport.visibleRange &&
           row.physicalWidth == viewport.physicalWidth &&
           row.devicePixelRatio == viewport.devicePixelRatio &&
           row.captureCenterFrequency == viewport.captureCenterFrequency &&
           row.captureSampleRate == viewport.captureSampleRate &&
           row.captureSpan == viewport.captureSpan &&
           row.captureFftSize == viewport.captureFftSize &&
           row.tuningGeneration == viewport.tuningGeneration &&
           row.peakMagnitudes.size() == viewport.physicalWidth &&
           row.meanLinearPowers.size() == viewport.physicalWidth;
}

std::optional<ViewportWaterfallHistoryRow>
projectFrameToWaterfallViewport(
    const radio::SpectrumFrame& frame,
    const WaterfallViewportDescriptor& viewport)
{
    if (!radio::hasConsistentMetadata(frame) || viewport.generation == 0 ||
        viewport.visibleRange.maximum <= viewport.visibleRange.minimum ||
        viewport.physicalWidth < 2 ||
        !std::isfinite(viewport.devicePixelRatio) ||
        viewport.devicePixelRatio < 1.0 ||
        viewport.captureCenterFrequency != frame.centerFrequency ||
        viewport.captureSampleRate != frame.sampleRate ||
        viewport.captureSpan != radio::captureSpan(frame) ||
        viewport.captureFftSize != frame.fftSize ||
        viewport.tuningGeneration != frame.tuningGeneration) {
        return std::nullopt;
    }

    const auto magnitudes = std::span<const float>(
        frame.normalizedMagnitudes.data(), frame.normalizedMagnitudes.size());
    const radio::FftBinFrequencyMapper sourceAxis(
        frame.centerFrequency,
        radio::captureSpan(frame),
        {0.0, static_cast<double>(magnitudes.size() - 1)});
    if (!sourceAxis.valid() ||
        !sourceAxis.positionForFrequency(
            static_cast<double>(viewport.visibleRange.minimum)).has_value() ||
        !sourceAxis.positionForFrequency(
            static_cast<double>(viewport.visibleRange.maximum)).has_value()) {
        return std::nullopt;
    }

    ViewportWaterfallHistoryRow projected{
        .sequence = frame.sequence,
        .timestampNanoseconds = frame.timestampNanoseconds,
        .viewportGeneration = viewport.generation,
        .visibleRange = viewport.visibleRange,
        .physicalWidth = viewport.physicalWidth,
        .devicePixelRatio = viewport.devicePixelRatio,
        .captureCenterFrequency = frame.centerFrequency,
        .captureSampleRate = frame.sampleRate,
        .captureSpan = radio::captureSpan(frame),
        .captureFftSize = frame.fftSize,
        .tuningGeneration = frame.tuningGeneration,
        .peakMagnitudes = std::vector<std::uint16_t>(
            viewport.physicalWidth),
        .meanLinearPowers = std::vector<float>(viewport.physicalWidth),
    };

    const double sourceLower = sourceAxis.nominalLowerFrequency();
    const double sourceUpper = sourceAxis.nominalUpperFrequency();
    const double targetSpan = static_cast<double>(
        viewport.visibleRange.maximum - viewport.visibleRange.minimum);
    const double targetStep =
        targetSpan / static_cast<double>(viewport.physicalWidth - 1);
    const double sourceStep =
        (sourceUpper - sourceLower) /
        static_cast<double>(magnitudes.size() - 1);
    const bool reducing = targetStep > sourceStep * (1.0 + 1.0e-9);

    for (std::size_t column = 0; column < viewport.physicalWidth; ++column) {
        const double frequency =
            static_cast<double>(viewport.visibleRange.minimum) +
            targetSpan * static_cast<double>(column) /
                static_cast<double>(viewport.physicalWidth - 1);
        const double sourcePosition =
            sourceAxis.positionForFrequency(frequency).value();
        const auto nearest = static_cast<std::size_t>(std::floor(std::clamp(
            sourcePosition,
            0.0,
            static_cast<double>(magnitudes.size() - 1)) + 0.5 + 1.0e-9));
        float peak = std::clamp(magnitudes[nearest], 0.0F, 1.0F);
        double powerSum = linearPowerForNormalizedMagnitude(peak);
        std::size_t powerCount = 1;

        if (reducing) {
            const double firstFrequency =
                std::max(frequency - targetStep / 2.0, sourceLower);
            const double lastFrequency =
                std::min(frequency + targetStep / 2.0, sourceUpper);
            const auto firstBin = static_cast<std::size_t>(std::ceil(
                sourceAxis.positionForFrequency(firstFrequency)
                    .value_or(sourcePosition)));
            const auto lastBin = static_cast<std::size_t>(std::floor(
                sourceAxis.positionForFrequency(lastFrequency)
                    .value_or(sourcePosition)));
            if (firstBin <= lastBin && firstBin < magnitudes.size()) {
                powerSum = 0.0;
                powerCount = 0;
                const std::size_t boundedLast =
                    std::min(lastBin, magnitudes.size() - 1);
                for (std::size_t source = firstBin;
                     source <= boundedLast;
                     ++source) {
                    const float magnitude =
                        std::clamp(magnitudes[source], 0.0F, 1.0F);
                    peak = std::max(peak, magnitude);
                    powerSum += linearPowerForNormalizedMagnitude(magnitude);
                    ++powerCount;
                }
            }
        }

        projected.peakMagnitudes[column] = static_cast<std::uint16_t>(
            std::lround(peak * 65'535.0F));
        projected.meanLinearPowers[column] =
            powerCount == 0
                ? linearPowerForNormalizedMagnitude(peak)
                : static_cast<float>(
                      powerSum / static_cast<double>(powerCount));
    }
    return projected;
}

void combineWaterfallFrames(
    std::span<float> accumulated,
    std::span<const float> additional,
    WaterfallAggregation aggregation) noexcept
{
    const std::size_t count = std::min(accumulated.size(), additional.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (aggregation == WaterfallAggregation::Original) {
            accumulated[index] = std::max(accumulated[index], additional[index]);
        } else {
            accumulated[index] += additional[index];
        }
    }
}

void finishWaterfallFrameAggregation(
    std::span<float> accumulated,
    std::size_t contributingFrames,
    WaterfallAggregation aggregation) noexcept
{
    if (aggregation != WaterfallAggregation::Average ||
        contributingFrames == 0) {
        return;
    }
    const float divisor = static_cast<float>(contributingFrames);
    for (float& power : accumulated) {
        power /= divisor;
    }
}

std::vector<float> projectFrameToFrequencyAxis(
    const radio::SpectrumFrame& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel)
{
    const auto magnitudes = std::span<const float>(
        frame.normalizedMagnitudes.data(), frame.normalizedMagnitudes.size());
    return projectFrame(frame, magnitudes, targetAxis, displayColumns, emptyLevel);
}

void projectMaximumHoldToFrequencyAxis(
    std::span<const float> holdDbfs,
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    const radio::FrequencyAxisMapper& targetAxis,
    float emptyDbfs,
    std::span<float> projected)
{
    if (projected.size() < 2) {
        return;
    }

    std::fill(projected.begin(), projected.end(), emptyDbfs);
    if (holdDbfs.size() < 2 || sampleRate == 0 || !targetAxis.valid()) {
        return;
    }
    const radio::FftBinFrequencyMapper sourceAxis(
        centerFrequency,
        sampleRate,
        {0.0, static_cast<double>(holdDbfs.size() - 1)});
    if (!sourceAxis.valid()) {
        return;
    }

    const auto targetRange = targetAxis.visibleRange();
    const double sourceLower = sourceAxis.nominalLowerFrequency();
    const double sourceUpper = sourceAxis.nominalUpperFrequency();
    const double targetStepHz =
        static_cast<double>(targetRange.maximum - targetRange.minimum) /
        static_cast<double>(projected.size() - 1);
    const double sourceBinHz =
        (sourceUpper - sourceLower) /
        static_cast<double>(holdDbfs.size() - 1);
    const bool reducing = targetStepHz > sourceBinHz * (1.0 + 1.0e-9);
    const auto plot = targetAxis.plot();

    for (std::size_t column = 0; column < projected.size(); ++column) {
        const double targetPosition =
            plot.left + plot.width * static_cast<double>(column) /
                            static_cast<double>(projected.size() - 1);
        const auto frequency = targetAxis.frequencyForPosition(targetPosition);
        if (!frequency.has_value()) {
            continue;
        }
        const auto sourcePosition = sourceAxis.positionForFrequency(*frequency);
        if (!sourcePosition.has_value()) {
            continue;
        }

        float value = interpolateAt(holdDbfs, *sourcePosition);
        if (reducing) {
            const double halfTargetStep = targetStepHz / 2.0;
            const double firstFrequency = std::max(
                *frequency - halfTargetStep, sourceLower);
            const double lastFrequency = std::min(
                *frequency + halfTargetStep, sourceUpper);
            const double firstPosition =
                sourceAxis.positionForFrequency(firstFrequency)
                    .value_or(*sourcePosition);
            const double lastPosition =
                sourceAxis.positionForFrequency(lastFrequency)
                    .value_or(*sourcePosition);
            const auto firstBin =
                static_cast<std::size_t>(std::ceil(firstPosition));
            const auto lastBin =
                static_cast<std::size_t>(std::floor(lastPosition));
            if (firstBin <= lastBin && firstBin < holdDbfs.size()) {
                const auto first =
                    holdDbfs.begin() + static_cast<std::ptrdiff_t>(firstBin);
                const auto last =
                    holdDbfs.begin() + static_cast<std::ptrdiff_t>(
                                           std::min(lastBin + 1, holdDbfs.size()));
                const float reduced = *std::max_element(first, last);
                value = std::max(value, reduced);
            }
        }
        projected[column] = value;
    }
}

std::vector<float> projectFrameToFrequencyAxis(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel)
{
    const auto magnitudes = std::span<const std::uint16_t>(
        frame.normalizedMagnitudes.data(), frame.normalizedMagnitudes.size());
    return projectFrame(frame, magnitudes, targetAxis, displayColumns, emptyLevel);
}

std::vector<float> projectWaterfallRowToFrequencyAxis(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyLevel)
{
    const auto magnitudes = std::span<const std::uint16_t>(
        frame.normalizedMagnitudes.data(), frame.normalizedMagnitudes.size());
    return projectWaterfallRow(
        frame, magnitudes, targetAxis, displayColumns, emptyLevel);
}

std::vector<float> projectAverageWaterfallRowToFrequencyAxis(
    const WaterfallHistoryRow& frame,
    const radio::FrequencyAxisMapper& targetAxis,
    std::size_t displayColumns,
    float emptyPower)
{
    return projectAverageWaterfallRow(
        frame, targetAxis, displayColumns, emptyPower);
}

WaterfallHistoryBuffer::WaterfallHistoryBuffer(
    std::size_t capacity, std::size_t memoryBudgetBytes)
    : m_capacity(capacity)
    , m_memoryBudgetBytes(memoryBudgetBytes)
{
    if (capacity == 0 || memoryBudgetBytes == 0) {
        throw std::invalid_argument("Waterfall history limits must be positive");
    }
}

void WaterfallHistoryBuffer::setCapacity(std::size_t capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Waterfall history capacity must be positive");
    }
    m_capacity = capacity;
    while (m_rows.size() > m_capacity) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
}

void WaterfallHistoryBuffer::setMemoryBudgetBytes(std::size_t memoryBudgetBytes)
{
    if (memoryBudgetBytes == 0) {
        throw std::invalid_argument("Waterfall history memory budget must be positive");
    }
    m_memoryBudgetBytes = memoryBudgetBytes;
    while (m_memoryUsageBytes > m_memoryBudgetBytes && !m_rows.empty()) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
}

void WaterfallHistoryBuffer::setRetentionDurationSeconds(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        throw std::invalid_argument("Waterfall history duration must be positive");
    }
    m_retentionDurationSeconds = seconds;
    if (m_rows.empty()) {
        return;
    }
    const auto newest = m_rows.front().timestampNanoseconds;
    const auto maximumAge = static_cast<std::uint64_t>(seconds * 1.0e9);
    while (!m_rows.empty() &&
           newest - std::min(newest, m_rows.back().timestampNanoseconds) >
               maximumAge) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
}

bool WaterfallHistoryBuffer::append(radio::SpectrumFrame frame)
{
    if (!radio::hasConsistentMetadata(frame)) {
        return false;
    }
    WaterfallHistoryRow compact{
        .sequence = frame.sequence,
        .timestampNanoseconds = frame.timestampNanoseconds,
        .centerFrequency = frame.centerFrequency,
        .sampleRate = frame.sampleRate,
        .captureSpan = radio::captureSpan(frame),
        .fftSize = frame.fftSize,
        .tuningGeneration = frame.tuningGeneration,
        .normalizedMagnitudes = {},
        .linearPowerSums = {},
    };
    const std::size_t storedBins =
        std::min(m_storedBinCount, frame.normalizedMagnitudes.size());
    compact.normalizedMagnitudes.resize(storedBins);
    if (storedBins < frame.normalizedMagnitudes.size()) {
        compact.linearPowerSums.resize(storedBins);
    }
    for (std::size_t bin = 0; bin < storedBins; ++bin) {
        const std::size_t first =
            bin * frame.normalizedMagnitudes.size() / storedBins;
        const std::size_t onePastLast = std::max(
            first + 1,
            (bin + 1) * frame.normalizedMagnitudes.size() / storedBins);
        float peak = 0.0F;
        double powerSum = 0.0;
        for (std::size_t source = first; source < onePastLast; ++source) {
            const float magnitude = std::clamp(
                frame.normalizedMagnitudes[source], 0.0F, 1.0F);
            peak = std::max(peak, magnitude);
            powerSum += static_cast<double>(
                linearPowerForNormalizedMagnitude(magnitude));
        }
        compact.normalizedMagnitudes[bin] = static_cast<std::uint16_t>(
            std::lround(peak * 65'535.0F));
        if (!compact.linearPowerSums.empty()) {
            compact.linearPowerSums[bin] = static_cast<float>(powerSum);
        }
    }
    const std::size_t rowBytes = rowMemoryBytes(compact);
    if (rowBytes > m_memoryBudgetBytes) {
        return false;
    }
    while ((!m_rows.empty() && m_rows.size() >= m_capacity) ||
           (!m_rows.empty() && m_memoryUsageBytes + rowBytes > m_memoryBudgetBytes)) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
    m_rows.push_front(std::move(compact));
    m_memoryUsageBytes += rowBytes;
    const auto newest = m_rows.front().timestampNanoseconds;
    const auto maximumAge = static_cast<std::uint64_t>(
        m_retentionDurationSeconds * 1.0e9);
    while (!m_rows.empty() &&
           newest - std::min(newest, m_rows.back().timestampNanoseconds) >
               maximumAge) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
    return true;
}

void WaterfallHistoryBuffer::setStoredBinCount(std::size_t storedBinCount)
{
    if (storedBinCount < 2) {
        throw std::invalid_argument(
            "Waterfall stored history width must contain at least two bins");
    }
    m_storedBinCount = storedBinCount;
}

void WaterfallHistoryBuffer::clear()
{
    m_rows.clear();
    m_memoryUsageBytes = 0;
}

std::size_t WaterfallHistoryBuffer::capacity() const noexcept
{
    return m_capacity;
}

std::size_t WaterfallHistoryBuffer::size() const noexcept
{
    return m_rows.size();
}

std::size_t WaterfallHistoryBuffer::memoryBudgetBytes() const noexcept
{
    return m_memoryBudgetBytes;
}

std::size_t WaterfallHistoryBuffer::memoryUsageBytes() const noexcept
{
    return m_memoryUsageBytes;
}

double WaterfallHistoryBuffer::retentionDurationSeconds() const noexcept
{
    return m_retentionDurationSeconds;
}

double WaterfallHistoryBuffer::retainedDurationSeconds() const noexcept
{
    if (m_rows.size() < 2) {
        return 0.0;
    }
    return static_cast<double>(
               m_rows.front().timestampNanoseconds -
               std::min(
                   m_rows.front().timestampNanoseconds,
                   m_rows.back().timestampNanoseconds)) /
           1.0e9;
}

std::size_t WaterfallHistoryBuffer::storedBinCount() const noexcept
{
    return m_storedBinCount;
}

const std::deque<WaterfallHistoryRow>& WaterfallHistoryBuffer::rows() const noexcept
{
    return m_rows;
}

ViewportWaterfallHistoryBuffer::ViewportWaterfallHistoryBuffer(
    std::size_t capacity, std::size_t memoryBudgetBytes)
    : m_capacity(capacity)
    , m_memoryBudgetBytes(memoryBudgetBytes)
{
    if (capacity == 0 || memoryBudgetBytes == 0) {
        throw std::invalid_argument(
            "Viewport waterfall history limits must be positive");
    }
}

void ViewportWaterfallHistoryBuffer::setCapacity(std::size_t capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument(
            "Viewport waterfall history capacity must be positive");
    }
    m_capacity = capacity;
    while (m_rows.size() > m_capacity) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
}

void ViewportWaterfallHistoryBuffer::setMemoryBudgetBytes(
    std::size_t memoryBudgetBytes)
{
    if (memoryBudgetBytes == 0) {
        throw std::invalid_argument(
            "Viewport waterfall history memory budget must be positive");
    }
    m_memoryBudgetBytes = memoryBudgetBytes;
    while (m_memoryUsageBytes > m_memoryBudgetBytes && !m_rows.empty()) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
}

bool ViewportWaterfallHistoryBuffer::append(
    ViewportWaterfallHistoryRow row)
{
    if (row.sequence == 0 || row.viewportGeneration == 0 ||
        row.visibleRange.maximum <= row.visibleRange.minimum ||
        row.physicalWidth < 2 || !std::isfinite(row.devicePixelRatio) ||
        row.devicePixelRatio < 1.0 ||
        row.peakMagnitudes.size() != row.physicalWidth ||
        row.meanLinearPowers.size() != row.physicalWidth) {
        return false;
    }
    const std::size_t bytes = rowMemoryBytes(row);
    if (bytes > m_memoryBudgetBytes) {
        return false;
    }
    while ((!m_rows.empty() && m_rows.size() >= m_capacity) ||
           (!m_rows.empty() &&
            m_memoryUsageBytes + bytes > m_memoryBudgetBytes)) {
        m_memoryUsageBytes -= rowMemoryBytes(m_rows.back());
        m_rows.pop_back();
    }
    m_rows.push_front(std::move(row));
    m_memoryUsageBytes += bytes;
    return true;
}

void ViewportWaterfallHistoryBuffer::clear()
{
    m_rows.clear();
    m_memoryUsageBytes = 0;
}

std::size_t ViewportWaterfallHistoryBuffer::capacity() const noexcept
{
    return m_capacity;
}

std::size_t ViewportWaterfallHistoryBuffer::size() const noexcept
{
    return m_rows.size();
}

std::size_t
ViewportWaterfallHistoryBuffer::memoryBudgetBytes() const noexcept
{
    return m_memoryBudgetBytes;
}

std::size_t
ViewportWaterfallHistoryBuffer::memoryUsageBytes() const noexcept
{
    return m_memoryUsageBytes;
}

const std::deque<ViewportWaterfallHistoryRow>&
ViewportWaterfallHistoryBuffer::rows() const noexcept
{
    return m_rows;
}

}  // namespace sdr::gui
