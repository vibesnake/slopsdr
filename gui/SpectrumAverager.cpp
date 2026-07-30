// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumAverager.hpp"

#include <algorithm>
#include <cmath>

namespace sdr::gui {
namespace {

constexpr double minimumTimeConstantSeconds = 0.008;
constexpr double maximumTimeConstantSeconds = 1.8;
constexpr double nanosecondsPerSecond = 1'000'000'000.0;

}  // namespace

int SpectrumAverager::strength() const noexcept
{
    return m_strength;
}

void SpectrumAverager::setStrength(int strength) noexcept
{
    const int bounded = std::clamp(
        strength, minimumStrength, maximumStrength);
    if (bounded == m_strength) {
        return;
    }
    const bool crossesDisabledBoundary =
        bounded == minimumStrength || m_strength == minimumStrength;
    m_strength = bounded;
    if (crossesDisabledBoundary) {
        reset();
    }
}

void SpectrumAverager::reset() noexcept
{
    m_metadata = {};
    m_normalizedAccumulator.clear();
}

std::span<const float> SpectrumAverager::process(
    std::span<const float> normalizedMagnitudes,
    SpectrumAveragingMetadata metadata)
{
    if (m_strength == minimumStrength) {
        reset();
        return normalizedMagnitudes;
    }
    if (normalizedMagnitudes.size() < 2 ||
        metadata.sampleRate == 0 ||
        metadata.fftSize != normalizedMagnitudes.size()) {
        reset();
        return normalizedMagnitudes;
    }

    if (!compatible(metadata) ||
        metadata.timestampNanoseconds == 0 ||
        m_metadata.timestampNanoseconds == 0) {
        initialize(normalizedMagnitudes, metadata);
        return m_normalizedAccumulator;
    }
    if (metadata.timestampNanoseconds <= m_metadata.timestampNanoseconds) {
        return m_normalizedAccumulator;
    }

    const double elapsedSeconds =
        static_cast<double>(
            metadata.timestampNanoseconds -
            m_metadata.timestampNanoseconds) /
        nanosecondsPerSecond;
    const double timeConstant =
        timeConstantSecondsForStrength(m_strength);
    const float newFrameWeight = static_cast<float>(
        1.0 - std::exp(-elapsedSeconds / timeConstant));

    // Normalized spectrum values are linear on the displayed dBFS axis, so
    // direct averaging gives symmetric-looking rise and fall behavior.
    for (std::size_t index = 0;
         index < normalizedMagnitudes.size();
         ++index) {
        const float current = std::isfinite(normalizedMagnitudes[index])
                                  ? std::clamp(
                                        normalizedMagnitudes[index],
                                        0.0F,
                                        1.0F)
                                  : 0.0F;
        float& average = m_normalizedAccumulator[index];
        average += (current - average) * newFrameWeight;
    }
    m_metadata.timestampNanoseconds = metadata.timestampNanoseconds;
    return m_normalizedAccumulator;
}

bool SpectrumAverager::initialized() const noexcept
{
    return !m_normalizedAccumulator.empty();
}

std::size_t SpectrumAverager::binCount() const noexcept
{
    return m_normalizedAccumulator.size();
}

std::size_t SpectrumAverager::storageValueCount() const noexcept
{
    return m_normalizedAccumulator.size();
}

double SpectrumAverager::timeConstantSecondsForStrength(
    int strength) noexcept
{
    if (strength <= minimumStrength) {
        return 0.0;
    }
    const int bounded = std::clamp(
        strength, minimumStrength + 1, maximumStrength);
    const double position = static_cast<double>(
                                bounded - (minimumStrength + 1)) /
                            static_cast<double>(
                                maximumStrength -
                                (minimumStrength + 1));
    // Exponential spacing keeps the low end useful while still reaching a
    // strong 1.8-second average without a large insensitive slider region.
    return minimumTimeConstantSeconds *
           std::pow(
               maximumTimeConstantSeconds /
                   minimumTimeConstantSeconds,
               position);
}

void SpectrumAverager::initialize(
    std::span<const float> normalizedMagnitudes,
    SpectrumAveragingMetadata metadata)
{
    m_normalizedAccumulator.resize(normalizedMagnitudes.size());
    for (std::size_t index = 0;
         index < normalizedMagnitudes.size();
         ++index) {
        m_normalizedAccumulator[index] =
            std::isfinite(normalizedMagnitudes[index])
                ? std::clamp(normalizedMagnitudes[index], 0.0F, 1.0F)
                : 0.0F;
    }
    m_metadata = metadata;
}

bool SpectrumAverager::compatible(
    SpectrumAveragingMetadata metadata) const noexcept
{
    return initialized() &&
           metadata.centerFrequency == m_metadata.centerFrequency &&
           metadata.sampleRate == m_metadata.sampleRate &&
           metadata.fftSize == m_metadata.fftSize &&
           metadata.tuningGeneration == m_metadata.tuningGeneration;
}

}  // namespace sdr::gui
