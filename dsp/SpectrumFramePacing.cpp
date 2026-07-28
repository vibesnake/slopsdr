// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumFramePacing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sdr::dsp {

SpectrumFrameScheduler::SpectrumFrameScheduler(
    std::uint64_t effectiveSampleRate,
    std::size_t fftSize,
    std::uint32_t targetFramesPerSecond)
    : m_effectiveSampleRate(effectiveSampleRate)
    , m_fftSize(fftSize)
    , m_targetFramesPerSecond(targetFramesPerSecond)
    , m_vectorTimeScaled([fftSize, targetFramesPerSecond] {
        if (fftSize == 0 || targetFramesPerSecond == 0 ||
            fftSize > std::numeric_limits<std::uint64_t>::max() /
                          targetFramesPerSecond) {
            throw std::invalid_argument("Spectrum frame scheduler configuration is invalid");
        }
        return static_cast<std::uint64_t>(fftSize) * targetFramesPerSecond;
    }())
    , m_publishEveryVector(m_vectorTimeScaled >= effectiveSampleRate)
{
    if (effectiveSampleRate == 0) {
        throw std::invalid_argument("Spectrum frame scheduler requires a positive sample rate");
    }
}

bool SpectrumFrameScheduler::acceptsNextVector() noexcept
{
    if (m_publishEveryVector) {
        return true;
    }

    if (m_elapsedTimeScaled <
        m_effectiveSampleRate - m_vectorTimeScaled) {
        m_elapsedTimeScaled += m_vectorTimeScaled;
        return false;
    }
    m_elapsedTimeScaled = m_elapsedTimeScaled + m_vectorTimeScaled -
                          m_effectiveSampleRate;
    return true;
}

void SpectrumFrameScheduler::reset() noexcept
{
    m_elapsedTimeScaled = 0;
}

double SpectrumFrameScheduler::availableVectorsPerSecond() const noexcept
{
    return static_cast<double>(m_effectiveSampleRate) /
           static_cast<double>(m_fftSize);
}

double SpectrumFrameScheduler::targetFramesPerSecond() const noexcept
{
    return static_cast<double>(m_targetFramesPerSecond);
}

bool SpectrumFrameScheduler::publishesEveryVector() const noexcept
{
    return m_publishEveryVector;
}

SpectrumWindowHopScheduler::SpectrumWindowHopScheduler(
    std::uint64_t effectiveSampleRate,
    std::size_t fftSize,
    std::uint32_t targetFramesPerSecond)
    : m_effectiveSampleRate(effectiveSampleRate)
    , m_fftSize(fftSize)
    , m_targetFramesPerSecond(targetFramesPerSecond)
    , m_integralHop(
          targetFramesPerSecond == 0
              ? 0
              : std::max<std::uint64_t>(
                    fftSize, effectiveSampleRate / targetFramesPerSecond))
    , m_hopRemainder(
          targetFramesPerSecond == 0 ||
                  fftSize > effectiveSampleRate / targetFramesPerSecond
              ? 0
              : effectiveSampleRate % targetFramesPerSecond)
{
    if (effectiveSampleRate == 0 || fftSize == 0 ||
        targetFramesPerSecond == 0) {
        throw std::invalid_argument(
            "Spectrum window-hop scheduler configuration is invalid");
    }
}

std::uint64_t SpectrumWindowHopScheduler::nextHopSize() noexcept
{
    std::uint64_t hop = m_integralHop;
    m_fractionalRemainder += m_hopRemainder;
    if (m_fractionalRemainder >= m_targetFramesPerSecond) {
        m_fractionalRemainder -= m_targetFramesPerSecond;
        ++hop;
    }
    return std::max<std::uint64_t>(1, hop);
}

void SpectrumWindowHopScheduler::reset() noexcept
{
    m_fractionalRemainder = 0;
}

double SpectrumWindowHopScheduler::nominalHopSize() const noexcept
{
    return std::max(
        static_cast<double>(m_fftSize),
        static_cast<double>(m_effectiveSampleRate) /
            static_cast<double>(m_targetFramesPerSecond));
}

double SpectrumWindowHopScheduler::overlapPercentage() const noexcept
{
    return 100.0 * std::max(
                       0.0,
                       1.0 - nominalHopSize() / static_cast<double>(m_fftSize));
}

double SpectrumWindowHopScheduler::targetFramesPerSecond() const noexcept
{
    return static_cast<double>(m_targetFramesPerSecond);
}

double SpectrumWindowHopScheduler::achievableFramesPerSecond() const noexcept
{
    return std::min(
        targetFramesPerSecond(),
        static_cast<double>(m_effectiveSampleRate) /
            static_cast<double>(m_fftSize));
}

bool isSupportedSpectrumFftSize(std::size_t fftSize) noexcept
{
    return std::ranges::find(supportedSpectrumFftSizes, fftSize) !=
           supportedSpectrumFftSizes.end();
}

bool isSupportedSpectrumFrameRate(std::uint32_t framesPerSecond) noexcept
{
    return framesPerSecond > 0 &&
           framesPerSecond <= maximumSpectrumDisplayFramesPerSecond;
}

std::uint32_t adaptiveSpectrumFrameRate(double visibleHistorySeconds) noexcept
{
    constexpr double nominalTemporalSamples = 600.0;
    if (!std::isfinite(visibleHistorySeconds) || visibleHistorySeconds <= 0.0) {
        return defaultSpectrumDisplayFramesPerSecond;
    }
    return std::clamp(
        static_cast<std::uint32_t>(
            std::ceil(nominalTemporalSamples / visibleHistorySeconds)),
        std::uint32_t{1},
        maximumSpectrumDisplayFramesPerSecond);
}

}  // namespace sdr::dsp
