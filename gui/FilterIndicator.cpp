// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FilterIndicator.hpp"

#include <algorithm>
#include <cmath>

namespace sdr::gui {
namespace {

double alignedCenter(double position, float devicePixelRatio) noexcept
{
    return std::round(position * devicePixelRatio) / devicePixelRatio;
}

}  // namespace

void FilterWidthLabelLifetime::trigger(
    std::chrono::milliseconds timestamp) noexcept
{
    m_startedAt = timestamp;
}

bool FilterWidthLabelLifetime::visibleAt(
    std::chrono::milliseconds timestamp) const noexcept
{
    return timestamp >= m_startedAt && timestamp - m_startedAt < duration;
}

FilterGate filterGate(
    const sdr::radio::FrequencyAxisMapper& axis,
    std::uint64_t lowerFrequency,
    std::uint64_t listeningFrequency,
    std::uint64_t upperFrequency,
    float height,
    float devicePixelRatio,
    bool activeAdjustment) noexcept
{
    const float dpr = std::max(1.0F, devicePixelRatio);
    const float pixel = 1.0F / dpr;
    const float markerWidth = 7.0F * pixel;
    const float markerHeight = 5.0F * pixel;
    const auto lineFor = [&](std::uint64_t frequency) {
        return FilterGateLine{
            .centerX = alignedCenter(
                axis.positionForFrequency(static_cast<double>(frequency))
                    .value_or(0.0),
                dpr),
            .width = pixel,
        };
    };
    const FilterGateLine lower = lineFor(lowerFrequency);
    const FilterGateLine upper = lineFor(upperFrequency);
    const float physicalPassbandWidth = static_cast<float>(
        std::abs(upper.centerX - lower.centerX) * dpr);
    const FilterGateMode mode = physicalPassbandWidth >= 12.0F
                                    ? FilterGateMode::Wide
                                    : (physicalPassbandWidth >= 5.0F
                                           ? FilterGateMode::Medium
                                           : FilterGateMode::Narrow);
    const FilterGateLinePattern edgeLinePattern = activeAdjustment
                                                      ? FilterGateLinePattern::Full
                                                      : (mode == FilterGateMode::Wide
                                                             ? FilterGateLinePattern::Full
                                                             : (mode == FilterGateMode::Medium
                                                                    ? FilterGateLinePattern::Dashed
                                                                    : FilterGateLinePattern::Stubs));
    const float edgeLineOpacity = activeAdjustment
                                      ? 0.22F
                                      : (mode == FilterGateMode::Wide
                                             ? 0.65F
                                             : (mode == FilterGateMode::Medium
                                                    ? 0.35F
                                                    : 0.65F));
    return {
        .mode = mode,
        .physicalPassbandWidth = physicalPassbandWidth,
        .activeAdjustment = activeAdjustment,
        .edgeLinePattern = edgeLinePattern,
        .edgeLineOpacity = edgeLineOpacity,
        .lowerEdge = lower,
        .listening = lineFor(listeningFrequency),
        .upperEdge = upper,
        .markers = {
            FilterGateTriangle{lower.centerX, markerHeight, 0.0F,
                               markerWidth, true},
            FilterGateTriangle{upper.centerX, markerHeight, 0.0F,
                               markerWidth, true},
            FilterGateTriangle{lower.centerX, std::max(0.0F, height - markerHeight),
                               height, markerWidth, false},
            FilterGateTriangle{upper.centerX, std::max(0.0F, height - markerHeight),
                               height, markerWidth, false},
        },
    };
}

}  // namespace sdr::gui
