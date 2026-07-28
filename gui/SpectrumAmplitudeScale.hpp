// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <optional>
#include <span>
#include <vector>

namespace sdr::gui {

inline constexpr float normalizedSpectrumFloorDbfs = -120.0F;
inline constexpr float normalizedSpectrumCeilingDbfs = 0.0F;

[[nodiscard]] float dbfsForNormalizedSpectrum(float normalizedMagnitude) noexcept;
[[nodiscard]] float spectrumYForDbfs(
    float dbfs,
    float height,
    float minimumDbfs,
    float maximumDbfs) noexcept;
[[nodiscard]] std::vector<float> majorDbfsTicks(
    float minimumDbfs,
    float maximumDbfs);
[[nodiscard]] std::vector<float> minorDbfsTicks(
    float minimumDbfs,
    float maximumDbfs);
[[nodiscard]] std::optional<float> estimateNoiseFloorDbfs(
    std::span<float> scratch) noexcept;
[[nodiscard]] float smoothNoiseFloorDbfs(
    float previousDbfs,
    float estimateDbfs,
    float smoothing) noexcept;
[[nodiscard]] float amplitudeScaleMarginForPanel(
    float panelWidth,
    float devicePixelRatio) noexcept;

}  // namespace sdr::gui
