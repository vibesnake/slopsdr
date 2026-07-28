// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sdr::dsp {

inline constexpr std::size_t defaultSpectrumFftSize = 4'096;
inline constexpr std::array<std::size_t, 9> supportedSpectrumFftSizes{
    1'024,
    2'048,
    4'096,
    8'192,
    16'384,
    32'768,
    65'536,
    131'072,
    262'144,
};
inline constexpr std::uint32_t defaultSpectrumDisplayFramesPerSecond = 60;
inline constexpr std::uint32_t maximumSpectrumDisplayFramesPerSecond = 240;
inline constexpr float defaultSpectrumMinimumDbfs = -120.0F;
inline constexpr float defaultSpectrumMaximumDbfs = 0.0F;

struct SpectrumDisplayConfiguration {
    std::size_t fftSize = defaultSpectrumFftSize;
    std::uint32_t targetFramesPerSecond = defaultSpectrumDisplayFramesPerSecond;
    float minimumDbfs = defaultSpectrumMinimumDbfs;
    float maximumDbfs = defaultSpectrumMaximumDbfs;
};

class SpectrumFrameScheduler final
{
public:
    SpectrumFrameScheduler(
        std::uint64_t effectiveSampleRate,
        std::size_t fftSize = defaultSpectrumFftSize,
        std::uint32_t targetFramesPerSecond = defaultSpectrumDisplayFramesPerSecond);

    [[nodiscard]] bool acceptsNextVector() noexcept;
    void reset() noexcept;

    [[nodiscard]] double availableVectorsPerSecond() const noexcept;
    [[nodiscard]] double targetFramesPerSecond() const noexcept;
    [[nodiscard]] bool publishesEveryVector() const noexcept;

private:
    const std::uint64_t m_effectiveSampleRate;
    const std::size_t m_fftSize;
    const std::uint32_t m_targetFramesPerSecond;
    const std::uint64_t m_vectorTimeScaled;
    const bool m_publishEveryVector;
    std::uint64_t m_elapsedTimeScaled = 0;
};

class SpectrumWindowHopScheduler final
{
public:
    SpectrumWindowHopScheduler(
        std::uint64_t effectiveSampleRate,
        std::size_t fftSize = defaultSpectrumFftSize,
        std::uint32_t targetFramesPerSecond = defaultSpectrumDisplayFramesPerSecond);

    [[nodiscard]] std::uint64_t nextHopSize() noexcept;
    void reset() noexcept;

    [[nodiscard]] double nominalHopSize() const noexcept;
    [[nodiscard]] double overlapPercentage() const noexcept;
    [[nodiscard]] double targetFramesPerSecond() const noexcept;
    [[nodiscard]] double achievableFramesPerSecond() const noexcept;

private:
    const std::uint64_t m_effectiveSampleRate;
    const std::size_t m_fftSize;
    const std::uint32_t m_targetFramesPerSecond;
    const std::uint64_t m_integralHop;
    const std::uint64_t m_hopRemainder;
    std::uint64_t m_fractionalRemainder = 0;
};

[[nodiscard]] bool isSupportedSpectrumFftSize(std::size_t fftSize) noexcept;
[[nodiscard]] bool isSupportedSpectrumFrameRate(
    std::uint32_t framesPerSecond) noexcept;
[[nodiscard]] std::uint32_t adaptiveSpectrumFrameRate(
    double visibleHistorySeconds) noexcept;

}  // namespace sdr::dsp
