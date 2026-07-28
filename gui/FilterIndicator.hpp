// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "FrequencyMapping.hpp"

#include <array>
#include <chrono>
#include <cstdint>

namespace sdr::gui {

enum class FilterGateMode {
    Wide,
    Medium,
    Narrow,
};

enum class FilterGateLinePattern {
    Full,
    Dashed,
    Stubs,
};

struct FilterGateLine
{
    double centerX = 0.0;
    float width = 0.0F;
};

struct FilterGateTriangle
{
    double tipX = 0.0;
    float tipY = 0.0F;
    float baseY = 0.0F;
    float width = 0.0F;
    bool pointsDown = false;
};

struct FilterGate
{
    FilterGateMode mode = FilterGateMode::Wide;
    float physicalPassbandWidth = 0.0F;
    bool activeAdjustment = false;
    FilterGateLinePattern edgeLinePattern = FilterGateLinePattern::Full;
    float edgeLineOpacity = 0.65F;
    FilterGateLine lowerEdge;
    FilterGateLine listening;
    FilterGateLine upperEdge;
    std::array<FilterGateTriangle, 4> markers;
};

struct FilterWidthLabelLifetime
{
    static constexpr auto duration = std::chrono::seconds(1);

    void trigger(std::chrono::milliseconds timestamp) noexcept;
    [[nodiscard]] bool visibleAt(std::chrono::milliseconds timestamp) const noexcept;

private:
    std::chrono::milliseconds m_startedAt{-duration};
};

[[nodiscard]] FilterGate filterGate(
    const sdr::radio::FrequencyAxisMapper& axis,
    std::uint64_t lowerFrequency,
    std::uint64_t listeningFrequency,
    std::uint64_t upperFrequency,
    float height,
    float devicePixelRatio,
    bool activeAdjustment = false) noexcept;

}  // namespace sdr::gui
