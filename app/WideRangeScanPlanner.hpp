// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "CurrentPassbandScanner.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sdr::app {

struct ScanFilterOffsets {
    std::uint64_t lower = 0;
    std::uint64_t upper = 0;
};

struct WideRangeCaptureGeometry {
    std::uint64_t captureBandwidth = 0;
    std::uint64_t edgeGuard = 0;
    std::vector<radio::FrequencyRange> tuningRanges;
    std::vector<radio::FrequencyRange> centerRanges;
};

struct WideRangeCaptureBlock {
    std::uint64_t centerFrequency = 0;
    std::size_t firstFrequencyIndex = 0;
    std::size_t lastFrequencyIndex = 0;
};

struct WideRangeScanPlan {
    std::vector<std::uint64_t> frequencies;
    std::vector<WideRangeCaptureBlock> blocks;
};

struct WideRangePlanResult {
    std::optional<WideRangeScanPlan> plan;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return plan.has_value();
    }
};

class WideRangeScanPlanner final
{
public:
    static constexpr std::size_t maximumFrequencyCount = 1'000'000;

    [[nodiscard]] static ScanFilterOffsets filterOffsets(
        radio::DemodulationMode mode, std::uint64_t filterWidth) noexcept;
    [[nodiscard]] static WideRangePlanResult plan(
        const CurrentPassbandScanSettings& settings,
        ScanFilterOffsets filter,
        const WideRangeCaptureGeometry& geometry);
    [[nodiscard]] static bool frequencyFits(
        std::uint64_t centerFrequency,
        std::uint64_t listeningFrequency,
        ScanFilterOffsets filter,
        const WideRangeCaptureGeometry& geometry) noexcept;
    [[nodiscard]] static std::optional<std::size_t> frequencyIndex(
        const WideRangeScanPlan& plan,
        std::uint64_t frequency) noexcept;
    [[nodiscard]] static std::optional<std::size_t> blockIndex(
        const WideRangeScanPlan& plan,
        std::size_t frequencyIndex) noexcept;
};

}  // namespace sdr::app
