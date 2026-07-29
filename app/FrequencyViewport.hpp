// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "FrequencyMapping.hpp"

#include <cstdint>
#include <limits>

namespace sdr::app {

class FrequencyViewport final
{
public:
    enum class PassbandAlignment {
        Centered,
        Upper,
        Lower,
    };

    static constexpr std::uint64_t minimumVisibleFftBins = 32;
    static constexpr double zoomStepFactor = 1.2;

    FrequencyViewport() = default;
    FrequencyViewport(
        std::uint64_t captureCenter,
        std::uint64_t captureSpan,
        std::uint64_t fftSize,
        std::uint64_t filterWidth) noexcept;

    [[nodiscard]] bool configureCapture(
        std::uint64_t captureCenter,
        std::uint64_t captureSpan,
        std::uint64_t listeningFrequency,
        sdr::radio::FrequencyRange advertisedRfRange = {
            0,
            std::numeric_limits<std::uint64_t>::max(),
        },
        bool preserveVisibleCenter = false) noexcept;
    [[nodiscard]] bool configureDetail(
        std::uint64_t fftSize,
        std::uint64_t filterWidth,
        std::uint64_t listeningFrequency,
        bool clampVisibleSpan = true,
        PassbandAlignment alignment = PassbandAlignment::Centered) noexcept;
    [[nodiscard]] bool zoomBySteps(
        std::uint64_t listeningFrequency,
        double wheelSteps) noexcept;
    [[nodiscard]] bool centerOn(std::uint64_t frequency) noexcept;
    [[nodiscard]] bool setPanPosition(double position) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] sdr::radio::FrequencyRange captureRange() const noexcept;
    [[nodiscard]] sdr::radio::FrequencyRange visibleRange() const noexcept;
    [[nodiscard]] std::uint64_t captureSpan() const noexcept;
    [[nodiscard]] std::uint64_t visibleSpan() const noexcept;
    [[nodiscard]] std::uint64_t visibleCenter() const noexcept;
    [[nodiscard]] std::uint64_t minimumVisibleSpan() const noexcept;
    [[nodiscard]] double zoomFactor() const noexcept;
    [[nodiscard]] double panPosition() const noexcept;
    [[nodiscard]] double panPageSize() const noexcept;
    [[nodiscard]] double normalizedPosition(
        std::uint64_t frequency) const noexcept;
    [[nodiscard]] sdr::radio::FrequencyAxisMapper axis(
        sdr::radio::FrequencyPlot plot) const noexcept;

private:
    [[nodiscard]] bool setVisibleSpanAnchored(
        std::uint64_t requestedSpan,
        std::uint64_t listeningFrequency,
        double normalizedAnchor) noexcept;
    [[nodiscard]] bool setVisibleSpanCentered(
        std::uint64_t requestedSpan,
        std::uint64_t centerFrequency) noexcept;
    [[nodiscard]] bool setVisibleLower(std::uint64_t lower) noexcept;
    void updateMinimumSpan() noexcept;

    sdr::radio::FrequencyRange m_captureRange;
    sdr::radio::FrequencyRange m_visibleRange;
    std::uint64_t m_nominalCaptureSpan = 0;
    std::uint64_t m_fftSize = 0;
    std::uint64_t m_filterWidth = 0;
    PassbandAlignment m_passbandAlignment = PassbandAlignment::Centered;
    std::uint64_t m_minimumVisibleSpan = 1;
    bool m_valid = false;
};

}  // namespace sdr::app
