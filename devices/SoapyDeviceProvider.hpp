// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "DeviceAccess.hpp"

#include <memory>

namespace sdr::devices {

class SoapyDeviceProvider final : public DeviceProvider
{
public:
    explicit SoapyDeviceProvider(bool verboseCapabilityLogging = false);
    ~SoapyDeviceProvider() override;

    SoapyDeviceProvider(const SoapyDeviceProvider&) = delete;
    SoapyDeviceProvider& operator=(const SoapyDeviceProvider&) = delete;

    [[nodiscard]] DeviceDiscoveryResult discover() override;
    [[nodiscard]] DeviceOpenResult open(const std::string& identifier) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sdr::devices
