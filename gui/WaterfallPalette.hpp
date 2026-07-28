// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <QColor>

#include <array>

namespace sdr::gui {

inline constexpr auto slopSpectrumPaletteName = "Slop Spectrum";

[[nodiscard]] const std::array<QRgb, 256>& slopSpectrumPalette() noexcept;
[[nodiscard]] int waterfallPaletteIndex(float dbfs, float minimumDbfs, float maximumDbfs) noexcept;
[[nodiscard]] QRgb slopSpectrumColor(float dbfs, float minimumDbfs, float maximumDbfs) noexcept;

}  // namespace sdr::gui
