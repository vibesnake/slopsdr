// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "DemodulatorRegistry.hpp"

#include <algorithm>

namespace sdr::radio {

namespace {

constexpr std::array<DemodulatorDescriptor, 6> descriptors{{
    {"am", "AM", DemodulationMode::Am},
    {"nfm", "NFM", DemodulationMode::Nfm},
    {"wfm", "WFM", DemodulationMode::Wfm},
    {"usb", "USB", DemodulationMode::Usb},
    {"lsb", "LSB", DemodulationMode::Lsb},
    {"digital-auto", "DMR/P25", DemodulationMode::DigitalDecoderOutput},
}};

}  // namespace

const std::array<DemodulatorDescriptor, 6>&
DemodulatorRegistry::availableDemodulators() noexcept
{
    return descriptors;
}

const DemodulatorDescriptor* DemodulatorRegistry::findById(
    std::string_view id) noexcept
{
    const auto match = std::find_if(
        descriptors.cbegin(),
        descriptors.cend(),
        [id](const DemodulatorDescriptor& descriptor) {
            return descriptor.id == id;
        });
    return match == descriptors.cend() ? nullptr : &*match;
}

const DemodulatorDescriptor* DemodulatorRegistry::findByMode(
    DemodulationMode mode) noexcept
{
    const auto match = std::find_if(
        descriptors.cbegin(),
        descriptors.cend(),
        [mode](const DemodulatorDescriptor& descriptor) {
            return descriptor.mode == mode;
        });
    return match == descriptors.cend() ? nullptr : &*match;
}

std::optional<DemodulationMode> DemodulatorRegistry::resolve(
    std::string_view id) noexcept
{
    if (const auto* descriptor = findById(id)) {
        return descriptor->mode;
    }
    return std::nullopt;
}

}  // namespace sdr::radio
