// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RtlSdrCapabilities.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace sdr::devices {
namespace {

std::string normalized(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char rawCharacter : value) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (std::isalnum(character) != 0) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return result;
}

bool isConfirmedV4(const RtlSdrIdentity& identity)
{
    const std::string hardware = normalized(identity.hardware);
    const std::string tuner = normalized(identity.tuner);
    return isRtlSdrDriver(identity.driver) &&
           normalized(identity.manufacturer) == "rtlsdrblog" &&
           normalized(identity.product) == "blogv4" &&
           (hardware.find("r828d") != std::string::npos ||
            tuner.find("r828d") != std::string::npos);
}

bool advertisesV4HfRange(
    const std::vector<radio::FrequencyRange>& frequencyRanges)
{
    return std::ranges::any_of(frequencyRanges, [](const radio::FrequencyRange& range) {
        return range.minimum < rtlSdrBlogV4PracticalMinimumHz &&
               range.maximum >= rtlSdrBlogV4PracticalMinimumHz;
    });
}

std::vector<radio::FrequencyRange> practicalV4HfRanges(
    const std::vector<radio::FrequencyRange>& frequencyRanges)
{
    std::vector<radio::FrequencyRange> practicalRanges;
    practicalRanges.reserve(frequencyRanges.size());
    for (const auto& range : frequencyRanges) {
        if (range.maximum < rtlSdrBlogV4PracticalMinimumHz) {
            continue;
        }
        practicalRanges.push_back({
            std::max(range.minimum, rtlSdrBlogV4PracticalMinimumHz),
            range.maximum,
        });
    }
    return practicalRanges;
}

}  // namespace

bool isRtlSdrDriver(std::string_view driver)
{
    return normalized(driver) == "rtlsdr";
}

DeviceCapabilities detectRtlSdrCapabilities(
    const RtlSdrIdentity& identity,
    const std::vector<radio::FrequencyRange>& receiveFrequencyRanges)
{
    DeviceCapabilities capabilities;
    capabilities.receive = !receiveFrequencyRanges.empty();
    capabilities.receiveFrequencyRanges = receiveFrequencyRanges;
    capabilities.rtlSdrBlogV4 = isConfirmedV4(identity);

    if (!capabilities.rtlSdrBlogV4) {
        return capabilities;
    }

    capabilities.driverManagedHfBelow27Mhz =
        advertisesV4HfRange(receiveFrequencyRanges);
    if (capabilities.driverManagedHfBelow27Mhz) {
        capabilities.receiveFrequencyRanges =
            practicalV4HfRanges(receiveFrequencyRanges);
    } else {
        capabilities.hfLimitation =
            "RTL-SDR Blog V4 identity is confirmed, but the installed SoapySDR "
            "driver does not advertise an RF range below 500 kHz";
    }
    return capabilities;
}

}  // namespace sdr::devices
