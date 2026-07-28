// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace sdr::dsp {

[[nodiscard]] std::vector<float> makeHannWindow(std::size_t fftSize);
[[nodiscard]] float coherentGain(std::span<const float> window);

}  // namespace sdr::dsp
