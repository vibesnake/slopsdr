// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FrequencyViewport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sdr::app {

FrequencyViewport::FrequencyViewport(
    std::uint64_t captureCenter,
    std::uint64_t captureSpan,
    std::uint64_t fftSize,
    std::uint64_t filterWidth) noexcept
    : m_fftSize(fftSize)
    , m_filterWidth(filterWidth)
{
    (void)configureCapture(captureCenter, captureSpan, captureCenter);
    updateMinimumSpan();
}

bool FrequencyViewport::configureCapture(
    std::uint64_t captureCenter,
    std::uint64_t captureSpan,
    std::uint64_t listeningFrequency,
    sdr::radio::FrequencyRange advertisedRfRange) noexcept
{
    const auto captureRange = sdr::radio::visibleCaptureRange(
        captureCenter, captureSpan, advertisedRfRange);
    if (!captureRange.has_value()) {
        return false;
    }
    const auto previousRange = m_visibleRange;
    const double previousZoomFactor = zoomFactor();
    m_captureRange = *captureRange;
    m_nominalCaptureSpan = captureSpan;
    m_valid = true;
    updateMinimumSpan();
    if (previousZoomFactor <= 1.0) {
        m_visibleRange = m_captureRange;
        return m_visibleRange.minimum != previousRange.minimum ||
               m_visibleRange.maximum != previousRange.maximum;
    }
    const std::uint64_t preservedSpan = std::clamp(
        static_cast<std::uint64_t>(std::llround(
            static_cast<double>(this->captureSpan()) / previousZoomFactor)),
        m_minimumVisibleSpan,
        this->captureSpan());
    const auto oldVisible = m_visibleRange;
    m_visibleRange = m_captureRange;
    (void)setVisibleSpanAnchored(preservedSpan, listeningFrequency, 0.5);
    return m_visibleRange.minimum != oldVisible.minimum ||
           m_visibleRange.maximum != oldVisible.maximum;
}

bool FrequencyViewport::configureDetail(
    std::uint64_t fftSize,
    std::uint64_t filterWidth,
    std::uint64_t listeningFrequency,
    bool clampVisibleSpan,
    PassbandAlignment alignment) noexcept
{
    m_fftSize = fftSize;
    m_filterWidth = filterWidth;
    m_passbandAlignment = alignment;
    updateMinimumSpan();
    if (!clampVisibleSpan || !m_valid ||
        visibleSpan() >= m_minimumVisibleSpan) {
        return false;
    }
    return setVisibleSpanAnchored(
        m_minimumVisibleSpan,
        listeningFrequency,
        normalizedPosition(listeningFrequency));
}

bool FrequencyViewport::zoomBySteps(
    std::uint64_t listeningFrequency,
    double wheelSteps) noexcept
{
    if (!m_valid || !std::isfinite(wheelSteps) || wheelSteps == 0.0) {
        return false;
    }
    const double requested = static_cast<double>(visibleSpan()) /
                             std::pow(zoomStepFactor, wheelSteps);
    const std::uint64_t requestedSpan = static_cast<std::uint64_t>(std::llround(
        std::clamp(
            requested,
            static_cast<double>(m_minimumVisibleSpan),
            static_cast<double>(captureSpan()))));
    return setVisibleSpanAnchored(
        requestedSpan,
        listeningFrequency,
        normalizedPosition(listeningFrequency));
}

bool FrequencyViewport::centerOn(std::uint64_t frequency) noexcept
{
    if (!m_valid || visibleSpan() >= captureSpan()) {
        return false;
    }
    const double requestedLower =
        static_cast<double>(frequency) -
        static_cast<double>(visibleSpan()) / 2.0;
    const double maximumLower =
        static_cast<double>(m_captureRange.maximum - visibleSpan());
    const std::uint64_t lower = static_cast<std::uint64_t>(std::llround(
        std::clamp(
            requestedLower,
            static_cast<double>(m_captureRange.minimum),
            maximumLower)));
    const sdr::radio::FrequencyRange updated{
        lower,
        lower + visibleSpan(),
    };
    if (updated.minimum == m_visibleRange.minimum) {
        return false;
    }
    m_visibleRange = updated;
    return true;
}

bool FrequencyViewport::reset() noexcept
{
    if (!m_valid ||
        (m_visibleRange.minimum == m_captureRange.minimum &&
         m_visibleRange.maximum == m_captureRange.maximum)) {
        return false;
    }
    m_visibleRange = m_captureRange;
    return true;
}

bool FrequencyViewport::valid() const noexcept
{
    return m_valid;
}

sdr::radio::FrequencyRange FrequencyViewport::captureRange() const noexcept
{
    return m_captureRange;
}

sdr::radio::FrequencyRange FrequencyViewport::visibleRange() const noexcept
{
    return m_visibleRange;
}

std::uint64_t FrequencyViewport::captureSpan() const noexcept
{
    return m_valid ? m_captureRange.maximum - m_captureRange.minimum : 0;
}

std::uint64_t FrequencyViewport::visibleSpan() const noexcept
{
    return m_valid ? m_visibleRange.maximum - m_visibleRange.minimum : 0;
}

std::uint64_t FrequencyViewport::visibleCenter() const noexcept
{
    return m_valid ? m_visibleRange.minimum + visibleSpan() / 2 : 0;
}

std::uint64_t FrequencyViewport::minimumVisibleSpan() const noexcept
{
    return m_minimumVisibleSpan;
}

double FrequencyViewport::zoomFactor() const noexcept
{
    return visibleSpan() == 0
               ? 1.0
               : static_cast<double>(captureSpan()) /
                     static_cast<double>(visibleSpan());
}

double FrequencyViewport::normalizedPosition(std::uint64_t frequency) const noexcept
{
    if (!m_valid || visibleSpan() == 0) {
        return 0.5;
    }
    if (frequency == visibleCenter()) {
        return 0.5;
    }
    return (static_cast<double>(frequency) -
            static_cast<double>(m_visibleRange.minimum)) /
           static_cast<double>(visibleSpan());
}

sdr::radio::FrequencyAxisMapper FrequencyViewport::axis(
    sdr::radio::FrequencyPlot plot) const noexcept
{
    return {m_visibleRange, plot};
}

bool FrequencyViewport::setVisibleSpanAnchored(
    std::uint64_t requestedSpan,
    std::uint64_t listeningFrequency,
    double normalizedAnchor) noexcept
{
    if (!m_valid) {
        return false;
    }
    const std::uint64_t span = std::clamp(
        requestedSpan, m_minimumVisibleSpan, captureSpan());
    const double anchor = std::clamp(normalizedAnchor, 0.0, 1.0);
    const double requestedLower = static_cast<double>(listeningFrequency) -
                                  anchor * static_cast<double>(span);
    const double maximumLower = static_cast<double>(m_captureRange.maximum - span);
    std::uint64_t lower = static_cast<std::uint64_t>(std::llround(std::clamp(
        requestedLower,
        static_cast<double>(m_captureRange.minimum),
        maximumLower)));
    std::uint64_t upper = lower + span;

    if (m_filterWidth <= span) {
        std::uint64_t lowerOffset = m_filterWidth / 2;
        std::uint64_t upperOffset = m_filterWidth - lowerOffset;
        if (m_passbandAlignment == PassbandAlignment::Upper) {
            lowerOffset = 0;
            upperOffset = m_filterWidth;
        } else if (m_passbandAlignment == PassbandAlignment::Lower) {
            lowerOffset = m_filterWidth;
            upperOffset = 0;
        }
        const std::uint64_t filterLower = listeningFrequency < lowerOffset
                                              ? 0
                                              : listeningFrequency - lowerOffset;
        const std::uint64_t filterUpper =
            listeningFrequency >
                    std::numeric_limits<std::uint64_t>::max() - upperOffset
                ? std::numeric_limits<std::uint64_t>::max()
                : listeningFrequency + upperOffset;
        if (filterLower < lower) {
            lower = std::max(m_captureRange.minimum, filterLower);
            upper = lower + span;
        }
        if (filterUpper > upper) {
            upper = std::min(m_captureRange.maximum, filterUpper);
            lower = upper - span;
        }
    }

    const sdr::radio::FrequencyRange updated{lower, upper};
    if (updated.minimum == m_visibleRange.minimum &&
        updated.maximum == m_visibleRange.maximum) {
        return false;
    }
    m_visibleRange = updated;
    return true;
}

void FrequencyViewport::updateMinimumSpan() noexcept
{
    if (!m_valid) {
        m_minimumVisibleSpan = 1;
        return;
    }
    const std::uint64_t binLimitedSpan = m_fftSize == 0
                                             ? captureSpan()
                                             : static_cast<std::uint64_t>(std::ceil(
                                                   static_cast<double>(
                                                       m_nominalCaptureSpan) *
                                                   minimumVisibleFftBins /
                                                   static_cast<double>(m_fftSize)));
    m_minimumVisibleSpan = std::clamp(
        std::max(m_filterWidth, binLimitedSpan),
        std::uint64_t{1},
        captureSpan());
}

}  // namespace sdr::app
