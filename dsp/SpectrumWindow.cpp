// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumWindow.hpp"

#include <cmath>
#include <numeric>
#include <stdexcept>

namespace sdr::dsp {

std::vector<float> makeHannWindow(std::size_t fftSize)
{
    if (fftSize < 2) {
        throw std::invalid_argument("Spectrum window requires at least two samples");
    }

    constexpr double twoPi = 6.28318530717958647692;
    std::vector<float> window(fftSize);
    for (std::size_t index = 0; index < fftSize; ++index) {
        window[index] = static_cast<float>(
            0.5 - 0.5 * std::cos(twoPi * static_cast<double>(index) /
                                 static_cast<double>(fftSize)));
    }
    return window;
}

float coherentGain(std::span<const float> window)
{
    if (window.empty()) {
        throw std::invalid_argument("Spectrum window cannot be empty");
    }
    const double sum = std::accumulate(window.begin(), window.end(), 0.0);
    const float gain = static_cast<float>(sum / static_cast<double>(window.size()));
    if (!std::isfinite(gain) || gain <= 0.0F) {
        throw std::invalid_argument("Spectrum window has no coherent gain");
    }
    return gain;
}

}  // namespace sdr::dsp
