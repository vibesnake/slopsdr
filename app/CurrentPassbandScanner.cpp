// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "CurrentPassbandScanner.hpp"

#include <algorithm>

namespace sdr::app {

std::optional<std::string> CurrentPassbandScanner::validate(
    const CurrentPassbandScanSettings& settings,
    radio::FrequencyRange usablePassband) noexcept
{
    if (!usablePassband.contains(settings.lowerFrequency) ||
        !usablePassband.contains(settings.upperFrequency)) {
        return "Scan bounds must remain inside the usable capture passband";
    }
    if (settings.lowerFrequency > settings.upperFrequency) {
        return "Lower frequency must not exceed upper frequency";
    }
    if (settings.stepSize == 0) {
        return "Step size must be greater than zero";
    }
    if (settings.dwellMilliseconds <= 0 ||
        settings.resumeDelayMilliseconds < 0) {
        return "Dwell time must be greater than zero and resume delay cannot be negative";
    }
    return std::nullopt;
}

CurrentPassbandScanState CurrentPassbandScanner::state() const noexcept
{
    return m_state;
}

std::uint64_t CurrentPassbandScanner::currentFrequency() const noexcept
{
    return m_currentFrequency;
}

bool CurrentPassbandScanner::start(
    CurrentPassbandScanSettings settings,
    radio::FrequencyRange usablePassband,
    std::uint64_t listeningFrequency,
    bool squelchOpen) noexcept
{
    if (validate(settings, usablePassband).has_value()) {
        return false;
    }
    m_settings = settings;
    m_currentFrequency = std::clamp(
        listeningFrequency, settings.lowerFrequency, settings.upperFrequency);
    m_state = squelchOpen ? CurrentPassbandScanState::Holding
                          : CurrentPassbandScanState::Running;
    return true;
}

void CurrentPassbandScanner::stop() noexcept
{
    m_state = CurrentPassbandScanState::Stopped;
}

void CurrentPassbandScanner::pause() noexcept
{
    if (m_state != CurrentPassbandScanState::Stopped) {
        m_state = CurrentPassbandScanState::Paused;
    }
}

void CurrentPassbandScanner::resume(bool squelchOpen) noexcept
{
    if (m_state == CurrentPassbandScanState::Paused) {
        m_state = squelchOpen ? CurrentPassbandScanState::Holding
                              : CurrentPassbandScanState::Running;
    }
}

void CurrentPassbandScanner::setSquelchOpen(bool squelchOpen) noexcept
{
    if (m_state == CurrentPassbandScanState::Running && squelchOpen) {
        m_state = CurrentPassbandScanState::Holding;
    }
}

std::optional<std::uint64_t> CurrentPassbandScanner::advanceAfterDwell() noexcept
{
    if (m_state != CurrentPassbandScanState::Running) {
        return std::nullopt;
    }
    m_currentFrequency = nextFrequency();
    return m_currentFrequency;
}

std::optional<std::uint64_t>
CurrentPassbandScanner::advanceAfterResumeDelay(bool squelchOpen) noexcept
{
    if (m_state != CurrentPassbandScanState::Holding || squelchOpen) {
        return std::nullopt;
    }
    m_state = CurrentPassbandScanState::Running;
    m_currentFrequency = nextFrequency();
    return m_currentFrequency;
}

std::optional<std::uint64_t> CurrentPassbandScanner::skip() noexcept
{
    if (m_state == CurrentPassbandScanState::Stopped) {
        return std::nullopt;
    }
    m_currentFrequency = nextFrequency();
    if (m_state == CurrentPassbandScanState::Holding) {
        m_state = CurrentPassbandScanState::Running;
    }
    return m_currentFrequency;
}

std::uint64_t CurrentPassbandScanner::nextFrequency() const noexcept
{
    if (m_currentFrequency >= m_settings.upperFrequency ||
        m_settings.stepSize > m_settings.upperFrequency - m_currentFrequency) {
        return m_settings.lowerFrequency;
    }
    return m_currentFrequency + m_settings.stepSize;
}

}  // namespace sdr::app
