// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumDisplayReduction.hpp"

#include <algorithm>
#include <cmath>

namespace sdr::gui {

std::vector<float> reduceSpectrumForDisplay(
    std::span<const float> magnitudes,
    std::size_t displayColumns)
{
    if (magnitudes.empty() || displayColumns == 0) {
        return {};
    }
    if (displayColumns == 1) {
        return {*std::max_element(magnitudes.begin(), magnitudes.end())};
    }

    std::vector<float> reduced(displayColumns);
    if (displayColumns >= magnitudes.size()) {
        const double sourceScale = static_cast<double>(magnitudes.size() - 1) /
                                   static_cast<double>(displayColumns - 1);
        for (std::size_t column = 0; column < displayColumns; ++column) {
            const double sourcePosition = static_cast<double>(column) * sourceScale;
            const auto lower = static_cast<std::size_t>(std::floor(sourcePosition));
            const auto upper = std::min(lower + 1, magnitudes.size() - 1);
            const float fraction = static_cast<float>(
                sourcePosition - static_cast<double>(lower));
            reduced[column] = std::lerp(
                magnitudes[lower], magnitudes[upper], fraction);
        }
        return reduced;
    }

    for (std::size_t column = 0; column < displayColumns; ++column) {
        const std::size_t first = column * magnitudes.size() / displayColumns;
        const std::size_t onePastLast = std::max(
            first + 1,
            (column + 1) * magnitudes.size() / displayColumns);
        reduced[column] = *std::max_element(
            magnitudes.begin() + static_cast<std::ptrdiff_t>(first),
            magnitudes.begin() + static_cast<std::ptrdiff_t>(onePastLast));
    }
    return reduced;
}

}  // namespace sdr::gui
