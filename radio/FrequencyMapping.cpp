// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FrequencyMapping.hpp"

#include <cmath>
#include <limits>

namespace sdr::radio {

FrequencyAxisMapper::FrequencyAxisMapper(
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    FrequencyPlot plot) noexcept
    : m_plot(plot)
{
    const std::uint64_t lowerOffset = sampleRate / 2;
    const std::uint64_t upperOffset = sampleRate - lowerOffset;
    if (sampleRate == 0 || centerFrequency < lowerOffset ||
        centerFrequency >
            std::numeric_limits<std::uint64_t>::max() - upperOffset) {
        return;
    }
    m_visibleRange = {
        centerFrequency - lowerOffset,
        centerFrequency + upperOffset,
    };
    m_valid = std::isfinite(m_plot.left) && std::isfinite(m_plot.width) &&
              m_plot.width > 0.0;
}

FrequencyAxisMapper::FrequencyAxisMapper(
    FrequencyRange visibleRange,
    FrequencyPlot plot) noexcept
    : m_visibleRange(visibleRange)
    , m_plot(plot)
    , m_valid(
          visibleRange.maximum > visibleRange.minimum &&
          std::isfinite(plot.left) && std::isfinite(plot.width) &&
          plot.width > 0.0)
{
}

bool FrequencyAxisMapper::valid() const noexcept
{
    return m_valid;
}

FrequencyRange FrequencyAxisMapper::visibleRange() const noexcept
{
    return m_visibleRange;
}

FrequencyPlot FrequencyAxisMapper::plot() const noexcept
{
    return m_plot;
}

std::optional<double> FrequencyAxisMapper::frequencyForPosition(
    double horizontalPosition) const noexcept
{
    if (!m_valid || !std::isfinite(horizontalPosition) ||
        horizontalPosition < m_plot.left ||
        horizontalPosition > m_plot.left + m_plot.width) {
        return std::nullopt;
    }
    const double normalized =
        (horizontalPosition - m_plot.left) / m_plot.width;
    const double span = static_cast<double>(
        m_visibleRange.maximum - m_visibleRange.minimum);
    return static_cast<double>(m_visibleRange.minimum) + normalized * span;
}

std::optional<std::uint64_t> FrequencyAxisMapper::roundedFrequencyForPosition(
    double horizontalPosition) const noexcept
{
    const auto frequency = frequencyForPosition(horizontalPosition);
    if (!frequency.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::llround(*frequency));
}

std::optional<double> FrequencyAxisMapper::positionForFrequency(
    double frequency) const noexcept
{
    if (!m_valid || !std::isfinite(frequency) ||
        frequency < static_cast<double>(m_visibleRange.minimum) ||
        frequency > static_cast<double>(m_visibleRange.maximum)) {
        return std::nullopt;
    }
    const double span = static_cast<double>(
        m_visibleRange.maximum - m_visibleRange.minimum);
    return m_plot.left +
           (frequency - static_cast<double>(m_visibleRange.minimum)) *
               m_plot.width / span;
}

FftBinFrequencyMapper::FftBinFrequencyMapper(
    std::uint64_t centerFrequency,
    std::uint64_t captureSpan,
    FrequencyPlot binPositions) noexcept
    : m_nominalLowerFrequency(
          static_cast<double>(centerFrequency) -
          static_cast<double>(captureSpan / 2))
    , m_nominalUpperFrequency(
          m_nominalLowerFrequency + static_cast<double>(captureSpan))
    , m_binPositions(binPositions)
    , m_valid(
          captureSpan > 0 &&
          std::isfinite(m_nominalLowerFrequency) &&
          std::isfinite(m_nominalUpperFrequency) &&
          m_nominalUpperFrequency > m_nominalLowerFrequency &&
          std::isfinite(binPositions.left) &&
          std::isfinite(binPositions.width) &&
          binPositions.width > 0.0)
{
}

bool FftBinFrequencyMapper::valid() const noexcept
{
    return m_valid;
}

double FftBinFrequencyMapper::nominalLowerFrequency() const noexcept
{
    return m_nominalLowerFrequency;
}

double FftBinFrequencyMapper::nominalUpperFrequency() const noexcept
{
    return m_nominalUpperFrequency;
}

std::optional<double> FftBinFrequencyMapper::positionForFrequency(
    double frequency) const noexcept
{
    if (!m_valid || !std::isfinite(frequency) ||
        frequency < m_nominalLowerFrequency ||
        frequency > m_nominalUpperFrequency) {
        return std::nullopt;
    }
    return m_binPositions.left +
           (frequency - m_nominalLowerFrequency) * m_binPositions.width /
               (m_nominalUpperFrequency - m_nominalLowerFrequency);
}

std::optional<FrequencyRange> visibleCaptureRange(
    std::uint64_t centerFrequency,
    std::uint64_t captureSpan,
    FrequencyRange advertisedRfRange) noexcept
{
    const FftBinFrequencyMapper nominalCapture(
        centerFrequency, captureSpan, {0.0, 1.0});
    if (!nominalCapture.valid() ||
        advertisedRfRange.maximum <= advertisedRfRange.minimum) {
        return std::nullopt;
    }

    const double lower = std::max(
        nominalCapture.nominalLowerFrequency(),
        static_cast<double>(advertisedRfRange.minimum));
    const double upper = std::min(
        nominalCapture.nominalUpperFrequency(),
        static_cast<double>(advertisedRfRange.maximum));
    if (!std::isfinite(lower) || !std::isfinite(upper) || upper <= lower ||
        lower < 0.0 ||
        upper > static_cast<double>(
                    std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    const FrequencyRange intersection{
        static_cast<std::uint64_t>(std::ceil(lower)),
        static_cast<std::uint64_t>(std::floor(upper)),
    };
    return intersection.maximum > intersection.minimum
               ? std::optional<FrequencyRange>{intersection}
               : std::nullopt;
}

std::optional<std::uint64_t> frequencyForPixel(
    double horizontalPosition,
    double displayWidth,
    FrequencyRange visibleRange) noexcept
{
    return FrequencyAxisMapper(visibleRange, {0.0, displayWidth})
        .roundedFrequencyForPosition(horizontalPosition);
}

std::optional<double> pixelForFrequency(
    std::uint64_t frequency,
    double displayWidth,
    FrequencyRange visibleRange) noexcept
{
    return FrequencyAxisMapper(visibleRange, {0.0, displayWidth})
        .positionForFrequency(static_cast<double>(frequency));
}

}  // namespace sdr::radio
