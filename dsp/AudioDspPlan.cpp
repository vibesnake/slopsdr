// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioDspPlan.hpp"

#include "AudioSampleBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace sdr::dsp {

namespace {

std::uint32_t checkedSampleRate(std::uint64_t sampleRate)
{
    if (sampleRate == 0 ||
        sampleRate > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Receiver sample rate cannot be converted to audio");
    }
    return static_cast<std::uint32_t>(sampleRate);
}

}  // namespace

AudioRateConversion makeAudioRateConversion(std::uint64_t receiverSampleRate)
{
    const auto inputRate = checkedSampleRate(receiverSampleRate);
    const auto divisor = std::gcd(inputRate, radio::receiverAudioSampleRate);
    return {
        inputRate,
        radio::receiverAudioSampleRate,
        radio::receiverAudioSampleRate / divisor,
        inputRate / divisor,
    };
}

ChannelRatePlan makeChannelRatePlan(
    std::uint64_t effectiveSampleRate,
    radio::DemodulationMode mode,
    std::uint64_t filterWidth)
{
    const auto inputRate = checkedSampleRate(effectiveSampleRate);
    if (filterWidth == 0 || filterWidth >= effectiveSampleRate) {
        throw std::invalid_argument("Channel filter width is out of range");
    }

    const double width = static_cast<double>(filterWidth);
    const double transition = std::max(
        static_cast<double>(inputRate) / 800.0,
        width * 0.15);
    const bool sideband = mode == radio::DemodulationMode::Usb ||
                          mode == radio::DemodulationMode::Lsb;
    const double highestPassbandFrequency = sideband ? width : width / 2.0;
    // Keep a practical resampler transition band and bounded scheduler batch
    // sizes for narrow modes; wider channel/filter requirements still win.
    const double minimumOutputRate = std::max(
        4.0 * static_cast<double>(radio::receiverAudioSampleRate),
        2.0 * (highestPassbandFrequency + transition));

    unsigned maximumDecimation = static_cast<unsigned>(
        std::max(1.0, std::floor(static_cast<double>(inputRate) /
                                 minimumOutputRate)));
    while (maximumDecimation > 1U &&
           inputRate % maximumDecimation != 0U) {
        --maximumDecimation;
    }

    return {
        inputRate,
        inputRate / maximumDecimation,
        maximumDecimation,
        transition,
        minimumOutputRate,
    };
}

double deemphasisAlpha(double sampleRate, double timeConstantSeconds)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
        !std::isfinite(timeConstantSeconds) || timeConstantSeconds <= 0.0) {
        throw std::invalid_argument("Deemphasis parameters must be positive");
    }
    return 1.0 - std::exp(-1.0 / (sampleRate * timeConstantSeconds));
}

}  // namespace sdr::dsp
