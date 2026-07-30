// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sdr::gui {

struct SpectrumAveragingMetadata {
    std::uint64_t centerFrequency = 0;
    std::uint64_t sampleRate = 0;
    std::size_t fftSize = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::uint64_t tuningGeneration = 0;
};

class SpectrumAverager final
{
public:
    static constexpr int minimumStrength = 0;
    static constexpr int maximumStrength = 100;

    [[nodiscard]] int strength() const noexcept;
    void setStrength(int strength) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::span<const float> process(
        std::span<const float> normalizedMagnitudes,
        SpectrumAveragingMetadata metadata);

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::size_t binCount() const noexcept;
    [[nodiscard]] std::size_t storageValueCount() const noexcept;

    [[nodiscard]] static double timeConstantSecondsForStrength(
        int strength) noexcept;

private:
    void initialize(
        std::span<const float> normalizedMagnitudes,
        SpectrumAveragingMetadata metadata);
    [[nodiscard]] bool compatible(
        SpectrumAveragingMetadata metadata) const noexcept;

    int m_strength = minimumStrength;
    SpectrumAveragingMetadata m_metadata;
    std::vector<float> m_linearPowerAccumulator;
    std::vector<float> m_normalizedOutput;
};

}  // namespace sdr::gui
