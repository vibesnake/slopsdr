// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace sdr::gui {

[[nodiscard]] std::vector<float> reduceSpectrumForDisplay(
    std::span<const float> magnitudes,
    std::size_t displayColumns);

}  // namespace sdr::gui
