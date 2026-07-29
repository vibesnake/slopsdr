// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WideRangeScanPlanner.hpp"

#include <algorithm>
#include <limits>

namespace sdr::app {
namespace {

struct FilterEnvelope {
    std::uint64_t lower = 0;
    std::uint64_t upper = 0;
};

std::optional<FilterEnvelope> filterEnvelope(
    std::uint64_t frequency, ScanFilterOffsets filter) noexcept
{
    if (frequency < filter.lower ||
        frequency > std::numeric_limits<std::uint64_t>::max() - filter.upper) {
        return std::nullopt;
    }
    return FilterEnvelope{frequency - filter.lower, frequency + filter.upper};
}

std::optional<std::uint64_t> centerForEnvelope(
    FilterEnvelope envelope,
    const WideRangeCaptureGeometry& geometry) noexcept
{
    if (geometry.captureBandwidth == 0 ||
        geometry.edgeGuard >= geometry.captureBandwidth / 2) {
        return std::nullopt;
    }
    const std::uint64_t nominalLower = geometry.captureBandwidth / 2;
    const std::uint64_t nominalUpper =
        geometry.captureBandwidth - nominalLower;
    if (geometry.edgeGuard >= nominalLower ||
        geometry.edgeGuard >= nominalUpper) {
        return std::nullopt;
    }
    const std::uint64_t lowerReach = nominalLower - geometry.edgeGuard;
    const std::uint64_t upperReach = nominalUpper - geometry.edgeGuard;
    const std::uint64_t minimumCenter =
        envelope.upper > upperReach ? envelope.upper - upperReach : 0;
    const std::uint64_t maximumCenter =
        envelope.lower > std::numeric_limits<std::uint64_t>::max() - lowerReach
            ? std::numeric_limits<std::uint64_t>::max()
            : envelope.lower + lowerReach;
    if (minimumCenter > maximumCenter) {
        return std::nullopt;
    }

    for (const auto tuningRange : geometry.tuningRanges) {
        if (!tuningRange.contains(envelope.lower) ||
            !tuningRange.contains(envelope.upper)) {
            continue;
        }
        for (const auto centerRange : geometry.centerRanges) {
            const std::uint64_t minimum = std::max(
                {minimumCenter, tuningRange.minimum, centerRange.minimum});
            const std::uint64_t maximum = std::min(
                {maximumCenter, tuningRange.maximum, centerRange.maximum});
            if (minimum > maximum) {
                continue;
            }
            const std::uint64_t midpoint =
                envelope.lower + (envelope.upper - envelope.lower) / 2;
            return std::clamp(midpoint, minimum, maximum);
        }
    }
    return std::nullopt;
}

}  // namespace

ScanFilterOffsets WideRangeScanPlanner::filterOffsets(
    radio::DemodulationMode mode, std::uint64_t filterWidth) noexcept
{
    if (mode == radio::DemodulationMode::Usb) {
        return {0, filterWidth};
    }
    if (mode == radio::DemodulationMode::Lsb) {
        return {filterWidth, 0};
    }
    const std::uint64_t lower = filterWidth / 2;
    return {lower, filterWidth - lower};
}

WideRangePlanResult WideRangeScanPlanner::plan(
    const CurrentPassbandScanSettings& settings,
    ScanFilterOffsets filter,
    const WideRangeCaptureGeometry& geometry)
{
    if (const auto error = CurrentPassbandScanner::validateSettings(settings);
        error.has_value()) {
        return {std::nullopt, *error};
    }
    if (geometry.tuningRanges.empty() || geometry.centerRanges.empty()) {
        return {std::nullopt, "Receiver tuning ranges are unavailable"};
    }
    const bool invalidGuard = geometry.captureBandwidth == 0 ||
                              geometry.edgeGuard >=
                                  geometry.captureBandwidth / 2;
    const std::uint64_t usableBandwidth = invalidGuard
                                              ? 0
                                              : geometry.captureBandwidth -
                                                    geometry.edgeGuard -
                                                    geometry.edgeGuard;
    if (invalidGuard || filter.lower > usableBandwidth ||
        filter.upper > usableBandwidth - filter.lower) {
        return {
            std::nullopt,
            "The active receive filter cannot fit inside the usable capture bandwidth",
        };
    }

    const std::uint64_t span =
        settings.upperFrequency - settings.lowerFrequency;
    const std::uint64_t frequencyIntervals = span / settings.stepSize;
    if (frequencyIntervals >= maximumFrequencyCount) {
        return {
            std::nullopt,
            "Scan range contains too many frequencies for a bounded scan plan",
        };
    }
    const std::uint64_t count = frequencyIntervals + 1;

    WideRangeScanPlan result;
    result.frequencies.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint64_t frequency =
            settings.lowerFrequency + index * settings.stepSize;
        const auto envelope = filterEnvelope(frequency, filter);
        const bool supported = envelope.has_value() && std::any_of(
            geometry.tuningRanges.cbegin(),
            geometry.tuningRanges.cend(),
            [envelope](radio::FrequencyRange range) {
                return range.contains(envelope->lower) &&
                       range.contains(envelope->upper);
            });
        if (!supported) {
            return {
                std::nullopt,
                "A scan frequency or its complete receive filter is outside the receiver tuning ranges",
            };
        }
        result.frequencies.push_back(frequency);
    }

    std::size_t first = 0;
    while (first < result.frequencies.size()) {
        const auto firstEnvelope = filterEnvelope(result.frequencies[first], filter);
        std::optional<std::uint64_t> selectedCenter;
        std::size_t last = first;
        for (std::size_t candidate = first;
             candidate < result.frequencies.size();
             ++candidate) {
            const auto lastEnvelope =
                filterEnvelope(result.frequencies[candidate], filter);
            const auto center = centerForEnvelope(
                {firstEnvelope->lower, lastEnvelope->upper}, geometry);
            if (!center.has_value()) {
                break;
            }
            selectedCenter = center;
            last = candidate;
        }
        if (!selectedCenter.has_value()) {
            return {
                std::nullopt,
                "A scan frequency cannot be centered safely in the capture bandwidth",
            };
        }
        result.blocks.push_back({*selectedCenter, first, last});
        first = last + 1;
    }
    return {std::move(result), {}};
}

bool WideRangeScanPlanner::frequencyFits(
    std::uint64_t centerFrequency,
    std::uint64_t listeningFrequency,
    ScanFilterOffsets filter,
    const WideRangeCaptureGeometry& geometry) noexcept
{
    const auto envelope = filterEnvelope(listeningFrequency, filter);
    if (!envelope.has_value()) {
        return false;
    }
    const auto center = centerForEnvelope(*envelope, geometry);
    if (!center.has_value()) {
        return false;
    }
    const bool supportedCenter = std::any_of(
        geometry.centerRanges.cbegin(),
        geometry.centerRanges.cend(),
        [centerFrequency](radio::FrequencyRange range) {
            return range.contains(centerFrequency);
        });
    const bool sharedTuningRange = std::any_of(
        geometry.tuningRanges.cbegin(),
        geometry.tuningRanges.cend(),
        [centerFrequency, envelope](radio::FrequencyRange range) {
            return range.contains(centerFrequency) &&
                   range.contains(envelope->lower) &&
                   range.contains(envelope->upper);
        });
    if (!supportedCenter || !sharedTuningRange ||
        geometry.edgeGuard >= geometry.captureBandwidth / 2) {
        return false;
    }
    const std::uint64_t lowerReach =
        geometry.captureBandwidth / 2 - geometry.edgeGuard;
    const std::uint64_t upperReach =
        geometry.captureBandwidth - geometry.captureBandwidth / 2 -
        geometry.edgeGuard;
    const std::uint64_t captureLower =
        centerFrequency > lowerReach ? centerFrequency - lowerReach : 0;
    const std::uint64_t captureUpper =
        centerFrequency > std::numeric_limits<std::uint64_t>::max() - upperReach
            ? std::numeric_limits<std::uint64_t>::max()
            : centerFrequency + upperReach;
    return envelope->lower >= captureLower && envelope->upper <= captureUpper;
}

std::optional<std::size_t> WideRangeScanPlanner::frequencyIndex(
    const WideRangeScanPlan& plan, std::uint64_t frequency) noexcept
{
    const auto match = std::lower_bound(
        plan.frequencies.cbegin(), plan.frequencies.cend(), frequency);
    if (match == plan.frequencies.cend() || *match != frequency) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(match - plan.frequencies.cbegin());
}

std::optional<std::size_t> WideRangeScanPlanner::blockIndex(
    const WideRangeScanPlan& plan, std::size_t frequencyIndex) noexcept
{
    const auto match = std::find_if(
        plan.blocks.cbegin(),
        plan.blocks.cend(),
        [frequencyIndex](const WideRangeCaptureBlock& block) {
            return frequencyIndex >= block.firstFrequencyIndex &&
                   frequencyIndex <= block.lastFrequencyIndex;
        });
    if (match == plan.blocks.cend()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(match - plan.blocks.cbegin());
}

}  // namespace sdr::app
