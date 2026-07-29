// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace sdr::app {

enum class FrequencyEditError {
    None,
    InvalidDigit,
    InvalidReplacementDigit,
    InvalidDirection,
    NoAllowedFrequency,
    ZeroedFrequencyOutsideLimits,
};

struct FrequencyEditResult {
    FrequencyEditError error = FrequencyEditError::None;
    std::uint64_t frequency = 0;
    bool adjustedToLimit = false;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == FrequencyEditError::None;
    }
};

class FrequencyDigitController final
{
public:
    static constexpr int digitCount = 10;

    [[nodiscard]] static std::optional<std::uint64_t> placeValue(
        int digitIndex) noexcept;
    [[nodiscard]] static FrequencyEditResult adjustDigit(
        std::uint64_t currentFrequency,
        int digitIndex,
        int direction,
        std::span<const radio::FrequencyRange> allowedRanges);
    [[nodiscard]] static FrequencyEditResult zeroFromDigit(
        std::uint64_t currentFrequency,
        int digitIndex,
        std::span<const radio::FrequencyRange> allowedRanges);
    [[nodiscard]] static FrequencyEditResult replaceDigit(
        std::uint64_t currentFrequency,
        int digitIndex,
        int replacementDigit);
    [[nodiscard]] static FrequencyEditResult constrain(
        std::uint64_t requestedFrequency,
        std::span<const radio::FrequencyRange> allowedRanges);
};

}  // namespace sdr::app
