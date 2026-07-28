// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "DeviceAccess.hpp"

#include <string>
#include <string_view>

namespace sdr::devices {

struct RtlSdrIdentity {
    std::string driver;
    std::string hardware;
    std::string manufacturer;
    std::string product;
    std::string tuner;
};

[[nodiscard]] bool isRtlSdrDriver(std::string_view driver);

[[nodiscard]] DeviceCapabilities detectRtlSdrCapabilities(
    const RtlSdrIdentity& identity,
    const std::vector<radio::FrequencyRange>& receiveFrequencyRanges);

}  // namespace sdr::devices
