// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <cstdint>
#include <optional>

namespace sdr::radio {

struct FrequencyPlot {
    double left = 0.0;
    double width = 0.0;
};

class FrequencyAxisMapper final
{
public:
    FrequencyAxisMapper(
        std::uint64_t centerFrequency,
        std::uint64_t sampleRate,
        FrequencyPlot plot) noexcept;
    FrequencyAxisMapper(
        FrequencyRange visibleRange,
        FrequencyPlot plot) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] FrequencyRange visibleRange() const noexcept;
    [[nodiscard]] FrequencyPlot plot() const noexcept;
    [[nodiscard]] std::optional<double> frequencyForPosition(
        double horizontalPosition) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> roundedFrequencyForPosition(
        double horizontalPosition) const noexcept;
    [[nodiscard]] std::optional<double> positionForFrequency(
        double frequency) const noexcept;

private:
    FrequencyRange m_visibleRange;
    FrequencyPlot m_plot;
    bool m_valid = false;
};

class FftBinFrequencyMapper final
{
public:
    FftBinFrequencyMapper(
        std::uint64_t centerFrequency,
        std::uint64_t captureSpan,
        FrequencyPlot binPositions) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] double nominalLowerFrequency() const noexcept;
    [[nodiscard]] double nominalUpperFrequency() const noexcept;
    [[nodiscard]] std::optional<double> positionForFrequency(
        double frequency) const noexcept;

private:
    double m_nominalLowerFrequency = 0.0;
    double m_nominalUpperFrequency = 0.0;
    FrequencyPlot m_binPositions;
    bool m_valid = false;
};

[[nodiscard]] std::optional<FrequencyRange> visibleCaptureRange(
    std::uint64_t centerFrequency,
    std::uint64_t captureSpan,
    FrequencyRange advertisedRfRange) noexcept;

[[nodiscard]] std::optional<std::uint64_t> frequencyForPixel(
    double horizontalPosition,
    double displayWidth,
    FrequencyRange visibleRange) noexcept;

[[nodiscard]] std::optional<double> pixelForFrequency(
    std::uint64_t frequency,
    double displayWidth,
    FrequencyRange visibleRange) noexcept;

}  // namespace sdr::radio
