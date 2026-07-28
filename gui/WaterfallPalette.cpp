// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WaterfallPalette.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sdr::gui {
namespace {

double clampUnit(double value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

double srgbToLinear(double channel) noexcept
{
    const double normalized = channel / 255.0;
    if (normalized <= 0.04045) {
        return normalized / 12.92;
    }
    return std::pow((normalized + 0.055) / 1.055, 2.4);
}

int linearToSrgb(double channel) noexcept
{
    const double bounded = clampUnit(channel);
    const double normalized = bounded <= 0.0031308
                                   ? 12.92 * bounded
                                   : 1.055 * std::pow(bounded, 1.0 / 2.4) -
                                         0.055;
    return static_cast<int>(std::lround(clampUnit(normalized) * 255.0));
}

struct PaletteStop {
    double position;
    std::array<double, 3> srgb;
};

constexpr std::array<PaletteStop, 10> slopSpectrumStops{{
    {0.000, {0.0, 0.0, 0.0}},
    {0.055, {0.0, 0.0, 20.0}},
    {0.150, {0.0, 18.0, 96.0}},
    {0.285, {0.0, 80.0, 255.0}},
    {0.420, {0.0, 215.0, 235.0}},
    {0.555, {0.0, 210.0, 90.0}},
    {0.690, {245.0, 225.0, 0.0}},
    {0.805, {255.0, 120.0, 0.0}},
    {0.915, {230.0, 20.0, 10.0}},
    {1.000, {255.0, 245.0, 220.0}},
}};

std::array<std::array<double, 3>, slopSpectrumStops.size()>
linearSlopSpectrumStops() noexcept
{
    std::array<std::array<double, 3>, slopSpectrumStops.size()> linear{};
    for (std::size_t stopIndex = 0; stopIndex < slopSpectrumStops.size();
         ++stopIndex) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            linear[stopIndex][channel] = srgbToLinear(
                slopSpectrumStops[stopIndex].srgb[channel]);
        }
    }
    return linear;
}

std::array<QRgb, 256> makeSlopSpectrumPalette() noexcept
{
    std::array<QRgb, 256> palette{};
    const auto linearStops = linearSlopSpectrumStops();
    for (std::size_t tableIndex = 0; tableIndex < palette.size(); ++tableIndex) {
        const double t = clampUnit(static_cast<double>(tableIndex) / 255.0);
        std::size_t rightStop = 1;
        while (rightStop + 1 < slopSpectrumStops.size() &&
               t > slopSpectrumStops[rightStop].position) {
            ++rightStop;
        }
        const std::size_t leftStop = rightStop - 1;
        const double segmentFraction = clampUnit(
            (t - slopSpectrumStops[leftStop].position) /
            (slopSpectrumStops[rightStop].position -
             slopSpectrumStops[leftStop].position));
        const double u = segmentFraction * segmentFraction *
                         (3.0 - 2.0 * segmentFraction);
        std::array<int, 3> channels{};
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            const double linear =
                linearStops[leftStop][channel] +
                u * (linearStops[rightStop][channel] -
                     linearStops[leftStop][channel]);
            channels[channel] = linearToSrgb(linear);
        }
        palette[tableIndex] = qRgba(
            channels[0], channels[1], channels[2], 255);
    }
    return palette;
}

}  // namespace

const std::array<QRgb, 256>& slopSpectrumPalette() noexcept
{
    static const std::array<QRgb, 256> palette = makeSlopSpectrumPalette();
    return palette;
}

int waterfallPaletteIndex(float dbfs, float minimumDbfs, float maximumDbfs) noexcept
{
    if (!std::isfinite(dbfs) || !(maximumDbfs > minimumDbfs)) {
        return 0;
    }
    const float colorIndex = (maximumDbfs - dbfs) * 256.0F /
                             (maximumDbfs - minimumDbfs);
    return 255 - std::clamp(static_cast<int>(std::lround(colorIndex)), 0, 255);
}

QRgb slopSpectrumColor(float dbfs, float minimumDbfs, float maximumDbfs) noexcept
{
    return slopSpectrumPalette()[static_cast<std::size_t>(
        waterfallPaletteIndex(dbfs, minimumDbfs, maximumDbfs))];
}

}  // namespace sdr::gui
