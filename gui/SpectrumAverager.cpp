// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumAverager.hpp"

#include "SpectrumAmplitudeScale.hpp"

#include <algorithm>
#include <cmath>

namespace sdr::gui {
namespace {

constexpr double minimumTimeConstantSeconds = 0.080;
constexpr double maximumTimeConstantSeconds = 4.0;
constexpr double nanosecondsPerSecond = 1'000'000'000.0;

float linearPowerForNormalizedSpectrum(float normalizedMagnitude) noexcept
{
    const float safeNormalized = std::isfinite(normalizedMagnitude)
                                     ? std::clamp(
                                           normalizedMagnitude, 0.0F, 1.0F)
                                     : 0.0F;
    const float dbfs = normalizedSpectrumFloorDbfs +
                       safeNormalized *
                           (normalizedSpectrumCeilingDbfs -
                            normalizedSpectrumFloorDbfs);
    return std::pow(10.0F, dbfs / 10.0F);
}

float normalizedSpectrumForLinearPower(float linearPower) noexcept
{
    const float floorPower =
        std::pow(10.0F, normalizedSpectrumFloorDbfs / 10.0F);
    const float safePower = std::isfinite(linearPower)
                                ? std::clamp(linearPower, floorPower, 1.0F)
                                : floorPower;
    const float dbfs = 10.0F * std::log10(safePower);
    return std::clamp(
        (dbfs - normalizedSpectrumFloorDbfs) /
            (normalizedSpectrumCeilingDbfs -
             normalizedSpectrumFloorDbfs),
        0.0F,
        1.0F);
}

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
    m_linearPowerAccumulator.clear();
    m_normalizedOutput.clear();
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
        return m_normalizedOutput;
    }
    if (metadata.timestampNanoseconds <= m_metadata.timestampNanoseconds) {
        return m_normalizedOutput;
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

    // Input values are a linear mapping of dBFS. Convert to linear power for
    // the EMA, then convert back only for the display-ready output.
    for (std::size_t index = 0;
         index < normalizedMagnitudes.size();
         ++index) {
        const float currentPower =
            linearPowerForNormalizedSpectrum(normalizedMagnitudes[index]);
        float& accumulated = m_linearPowerAccumulator[index];
        accumulated += (currentPower - accumulated) * newFrameWeight;
        m_normalizedOutput[index] =
            normalizedSpectrumForLinearPower(accumulated);
    }
    m_metadata.timestampNanoseconds = metadata.timestampNanoseconds;
    return m_normalizedOutput;
}

bool SpectrumAverager::initialized() const noexcept
{
    return !m_linearPowerAccumulator.empty();
}

std::size_t SpectrumAverager::binCount() const noexcept
{
    return m_linearPowerAccumulator.size();
}

std::size_t SpectrumAverager::storageValueCount() const noexcept
{
    return m_linearPowerAccumulator.size() + m_normalizedOutput.size();
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
    // strong four-second average without a large insensitive slider region.
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
    m_linearPowerAccumulator.resize(normalizedMagnitudes.size());
    m_normalizedOutput.resize(normalizedMagnitudes.size());
    for (std::size_t index = 0;
         index < normalizedMagnitudes.size();
         ++index) {
        m_linearPowerAccumulator[index] =
            linearPowerForNormalizedSpectrum(normalizedMagnitudes[index]);
        m_normalizedOutput[index] = std::isfinite(normalizedMagnitudes[index])
                                        ? std::clamp(
                                              normalizedMagnitudes[index],
                                              0.0F,
                                              1.0F)
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
