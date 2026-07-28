// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <cstdint>

namespace sdr::dsp {

struct AudioRateConversion {
    std::uint32_t inputSampleRate = 0;
    std::uint32_t outputSampleRate = 0;
    unsigned interpolation = 0;
    unsigned decimation = 0;
};

struct ChannelRatePlan {
    std::uint32_t inputSampleRate = 0;
    std::uint32_t outputSampleRate = 0;
    unsigned decimation = 0;
    double transitionWidthHz = 0.0;
    double minimumOutputSampleRate = 0.0;
};

[[nodiscard]] AudioRateConversion makeAudioRateConversion(
    std::uint64_t receiverSampleRate);
[[nodiscard]] ChannelRatePlan makeChannelRatePlan(
    std::uint64_t effectiveSampleRate,
    radio::DemodulationMode mode,
    std::uint64_t filterWidth);
[[nodiscard]] double deemphasisAlpha(
    double sampleRate, double timeConstantSeconds);

}  // namespace sdr::dsp
