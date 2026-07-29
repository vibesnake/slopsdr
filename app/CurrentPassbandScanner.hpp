// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace sdr::app {

enum class CurrentPassbandScanState {
    Stopped,
    Running,
    Paused,
    Holding,
};

struct CurrentPassbandScanSettings {
    std::uint64_t lowerFrequency = 0;
    std::uint64_t upperFrequency = 0;
    std::uint64_t stepSize = 0;
    int dwellMilliseconds = 0;
    int resumeDelayMilliseconds = 0;
};

class CurrentPassbandScanner final
{
public:
    [[nodiscard]] static std::optional<std::string> validate(
        const CurrentPassbandScanSettings& settings,
        radio::FrequencyRange usablePassband) noexcept;

    [[nodiscard]] CurrentPassbandScanState state() const noexcept;
    [[nodiscard]] std::uint64_t currentFrequency() const noexcept;
    [[nodiscard]] bool start(
        CurrentPassbandScanSettings settings,
        radio::FrequencyRange usablePassband,
        std::uint64_t listeningFrequency,
        bool squelchOpen) noexcept;
    void stop() noexcept;
    void pause() noexcept;
    void resume(bool squelchOpen) noexcept;
    void setSquelchOpen(bool squelchOpen) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> advanceAfterDwell() noexcept;
    [[nodiscard]] std::optional<std::uint64_t> advanceAfterResumeDelay(
        bool squelchOpen) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> skip() noexcept;

private:
    [[nodiscard]] std::uint64_t nextFrequency() const noexcept;

    CurrentPassbandScanState m_state = CurrentPassbandScanState::Stopped;
    CurrentPassbandScanSettings m_settings;
    std::uint64_t m_currentFrequency = 0;
};

}  // namespace sdr::app
