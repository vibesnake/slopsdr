// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <span>

namespace sdr::radio {

[[nodiscard]] std::optional<double> estimateOneShotSquelchThreshold(
    std::span<const double> signalStrengthSamplesDb,
    const ReceiverLimits& limits) noexcept;

class ReceiverStateModel final
{
public:
    explicit ReceiverStateModel(ReceiverLimits limits = {});

    [[nodiscard]] const ReceiverLimits& limits() const noexcept;
    [[nodiscard]] const ReceiverState& state() const noexcept;

    [[nodiscard]] OperationResult startReception();
    [[nodiscard]] OperationResult stopReception();
    [[nodiscard]] OperationResult setCenterFrequency(std::uint64_t frequency);
    [[nodiscard]] OperationResult setListeningFrequency(std::uint64_t frequency);
    [[nodiscard]] OperationResult tuneListeningFrequency(double normalizedPosition);
    [[nodiscard]] OperationResult shiftCenterFrequency(std::int64_t requestedStep);
    [[nodiscard]] OperationResult setSampleRate(std::uint64_t sampleRate);
    [[nodiscard]] OperationResult setFilterWidth(std::uint64_t filterWidth);
    [[nodiscard]] OperationResult setGain(double gainDb);
    [[nodiscard]] OperationResult setPpmCorrection(double ppmCorrection);
    [[nodiscard]] OperationResult setDemodulationMode(DemodulationMode mode);
    [[nodiscard]] OperationResult setSquelchLevel(double squelchLevelDb);
    [[nodiscard]] OperationResult enableManualSquelch();
    [[nodiscard]] OperationResult disableSquelch();

private:
    [[nodiscard]] static OperationResult success(
        bool stateChanged, std::string message, bool adjusted = false);
    [[nodiscard]] static OperationResult failure(
        ReceiverError error, std::string message);

    ReceiverLimits m_limits;
    ReceiverState m_state;
};

}  // namespace sdr::radio
