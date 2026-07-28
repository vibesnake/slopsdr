// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace sdr::radio {

struct DemodulatorDescriptor {
    std::string_view id;
    std::string_view displayName;
    DemodulationMode mode;
};

class DemodulatorRegistry final
{
public:
    [[nodiscard]] static const std::array<DemodulatorDescriptor, 6>&
    availableDemodulators() noexcept;
    [[nodiscard]] static const DemodulatorDescriptor* findById(
        std::string_view id) noexcept;
    [[nodiscard]] static const DemodulatorDescriptor* findByMode(
        DemodulationMode mode) noexcept;
    [[nodiscard]] static std::optional<DemodulationMode> resolve(
        std::string_view id) noexcept;
};

}  // namespace sdr::radio
