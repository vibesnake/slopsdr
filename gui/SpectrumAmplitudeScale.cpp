// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumAmplitudeScale.hpp"

#include <algorithm>
#include <cmath>

namespace sdr::gui {
namespace {

constexpr float majorTickStepDb = 20.0F;
constexpr float minorTickStepDb = 10.0F;
constexpr float minimumScaleMargin = 48.0F;
constexpr float maximumScaleMargin = 60.0F;

std::vector<float> ticksForStep(
    float minimumDbfs,
    float maximumDbfs,
    float stepDb,
    bool excludeMajor)
{
    std::vector<float> ticks;
    if (!(maximumDbfs > minimumDbfs) || !(stepDb > 0.0F)) {
        return ticks;
    }

    const int first = static_cast<int>(std::ceil(minimumDbfs / stepDb));
    const int last = static_cast<int>(std::floor(maximumDbfs / stepDb));
    ticks.reserve(static_cast<std::size_t>(std::max(0, last - first + 1)));
    for (int tick = first; tick <= last; ++tick) {
        if (excludeMajor && tick % 2 == 0) {
            continue;
        }
        ticks.push_back(static_cast<float>(tick) * stepDb);
    }
    return ticks;
}

}  // namespace

float dbfsForNormalizedSpectrum(float normalizedMagnitude) noexcept
{
    const float normalized = std::clamp(normalizedMagnitude, 0.0F, 1.0F);
    return normalizedSpectrumFloorDbfs +
           normalized * (normalizedSpectrumCeilingDbfs - normalizedSpectrumFloorDbfs);
}

float spectrumYForDbfs(
    float dbfs,
    float height,
    float minimumDbfs,
    float maximumDbfs) noexcept
{
    if (!(height > 0.0F) || !(maximumDbfs > minimumDbfs)) {
        return 0.0F;
    }
    const float bounded = std::clamp(dbfs, minimumDbfs, maximumDbfs);
    return (maximumDbfs - bounded) * height / (maximumDbfs - minimumDbfs);
}

std::vector<float> majorDbfsTicks(float minimumDbfs, float maximumDbfs)
{
    return ticksForStep(minimumDbfs, maximumDbfs, majorTickStepDb, false);
}

std::vector<float> minorDbfsTicks(float minimumDbfs, float maximumDbfs)
{
    return ticksForStep(minimumDbfs, maximumDbfs, minorTickStepDb, true);
}

std::optional<float> estimateNoiseFloorDbfs(std::span<float> scratch) noexcept
{
    const auto finiteEnd = std::remove_if(
        scratch.begin(),
        scratch.end(),
        [](float magnitude) { return !std::isfinite(magnitude); });
    scratch = scratch.first(static_cast<std::size_t>(finiteEnd - scratch.begin()));
    if (scratch.size() < 16) {
        return std::nullopt;
    }

    constexpr float noisePercentile = 0.20F;
    const std::size_t index = static_cast<std::size_t>(
        std::floor(noisePercentile * static_cast<float>(scratch.size() - 1)));
    std::nth_element(
        scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(index), scratch.end());
    return dbfsForNormalizedSpectrum(scratch[index]);
}

float smoothNoiseFloorDbfs(
    float previousDbfs,
    float estimateDbfs,
    float smoothing) noexcept
{
    const float boundedSmoothing = std::clamp(smoothing, 0.0F, 1.0F);
    return previousDbfs + (estimateDbfs - previousDbfs) * boundedSmoothing;
}

float amplitudeScaleMarginForPanel(
    float panelWidth,
    float devicePixelRatio) noexcept
{
    const float deviceScale = std::max(1.0F, devicePixelRatio);
    const float logicalMargin = std::clamp(
        panelWidth * 0.075F, minimumScaleMargin, maximumScaleMargin);
    return std::round(logicalMargin * deviceScale) / deviceScale;
}

}  // namespace sdr::gui
