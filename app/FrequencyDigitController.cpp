// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FrequencyDigitController.hpp"

#include <limits>
#include <utility>

namespace sdr::app {
namespace {

FrequencyEditResult failure(FrequencyEditError error, std::string message)
{
    return {error, 0, false, std::move(message)};
}

}  // namespace

std::optional<std::uint64_t> FrequencyDigitController::placeValue(
    int digitIndex) noexcept
{
    if (digitIndex < 0 || digitIndex >= digitCount) {
        return std::nullopt;
    }

    std::uint64_t place = 1;
    for (int index = digitIndex + 1; index < digitCount; ++index) {
        place *= 10;
    }
    return place;
}

FrequencyEditResult FrequencyDigitController::adjustDigit(
    std::uint64_t currentFrequency,
    int digitIndex,
    int direction,
    std::span<const radio::FrequencyRange> allowedRanges)
{
    const auto place = placeValue(digitIndex);
    if (!place.has_value()) {
        return failure(
            FrequencyEditError::InvalidDigit,
            "Frequency digit index is outside the displayed range");
    }
    if (direction == 0) {
        return failure(
            FrequencyEditError::InvalidDirection,
            "Frequency digit direction must increment or decrement");
    }

    std::uint64_t requestedFrequency = currentFrequency;
    if (direction > 0) {
        requestedFrequency = currentFrequency >
                                     std::numeric_limits<std::uint64_t>::max() - *place
                                 ? std::numeric_limits<std::uint64_t>::max()
                                 : currentFrequency + *place;
    } else {
        requestedFrequency = currentFrequency < *place
                                 ? 0
                                 : currentFrequency - *place;
    }
    return constrain(requestedFrequency, allowedRanges);
}

FrequencyEditResult FrequencyDigitController::zeroFromDigit(
    std::uint64_t currentFrequency,
    int digitIndex,
    std::span<const radio::FrequencyRange> allowedRanges)
{
    const auto place = placeValue(digitIndex);
    if (!place.has_value()) {
        return failure(
            FrequencyEditError::InvalidDigit,
            "Frequency digit index is outside the displayed range");
    }

    const std::uint64_t zeroingRange = *place * 10;
    const std::uint64_t requestedFrequency =
        (currentFrequency / zeroingRange) * zeroingRange;
    const auto constrained = constrain(requestedFrequency, allowedRanges);
    if (!constrained.succeeded()) {
        return constrained;
    }
    if (constrained.adjustedToLimit) {
        return failure(
            FrequencyEditError::ZeroedFrequencyOutsideLimits,
            "Zeroing these digits would place center frequency outside the available range");
    }
    return constrained;
}

FrequencyEditResult FrequencyDigitController::replaceDigit(
    std::uint64_t currentFrequency,
    int digitIndex,
    int replacementDigit)
{
    const auto place = placeValue(digitIndex);
    if (!place.has_value()) {
        return failure(
            FrequencyEditError::InvalidDigit,
            "Frequency digit index is outside the displayed range");
    }
    if (replacementDigit < 0 || replacementDigit > 9) {
        return failure(
            FrequencyEditError::InvalidReplacementDigit,
            "Frequency digit replacement must be between 0 and 9");
    }

    const std::uint64_t existingDigit = (currentFrequency / *place) % 10;
    return {
        FrequencyEditError::None,
        currentFrequency - existingDigit * *place +
            static_cast<std::uint64_t>(replacementDigit) * *place,
        false,
        "Frequency digit replaced",
    };
}

FrequencyEditResult FrequencyDigitController::constrain(
    std::uint64_t requestedFrequency,
    std::span<const radio::FrequencyRange> allowedRanges)
{
    std::optional<std::uint64_t> closestFrequency;
    std::uint64_t closestDistance = std::numeric_limits<std::uint64_t>::max();

    for (const radio::FrequencyRange range : allowedRanges) {
        if (range.maximum < range.minimum) {
            continue;
        }
        if (range.contains(requestedFrequency)) {
            return {
                FrequencyEditError::None,
                requestedFrequency,
                false,
                "Frequency is within the available range",
            };
        }

        const std::uint64_t candidate = requestedFrequency < range.minimum
                                            ? range.minimum
                                            : range.maximum;
        const std::uint64_t distance = requestedFrequency < candidate
                                           ? candidate - requestedFrequency
                                           : requestedFrequency - candidate;
        if (!closestFrequency.has_value() || distance < closestDistance ||
            (distance == closestDistance && candidate < *closestFrequency)) {
            closestFrequency = candidate;
            closestDistance = distance;
        }
    }

    if (!closestFrequency.has_value()) {
        return failure(
            FrequencyEditError::NoAllowedFrequency,
            "No center frequency is available within receiver and device limits");
    }
    return {
        FrequencyEditError::None,
        *closestFrequency,
        true,
        "Frequency was limited to the nearest available value",
    };
}

}  // namespace sdr::app
