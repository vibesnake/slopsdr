// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "DeviceController.hpp"
#include "RtlSdrCapabilities.hpp"

#include <QtTest>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace sdr::devices;

namespace {

struct MockTrace {
    std::vector<std::string> openedIdentifiers;
    std::vector<std::pair<std::uint64_t, HfTuningMode>> tuningRequests;
    std::vector<double> ppmRequests;
    std::vector<std::uint64_t> sampleRateRequests;
    std::vector<double> gainRequests;
    int streamStarts = 0;
    int streamStops = 0;
    bool throwWhenTuning = false;
    bool throwWhenSettingPpm = false;
    bool failWhenOpening = false;
    std::uint64_t effectiveSampleRate = 2'000'000;
    std::optional<double> effectivePpmCorrection;
};

class MockSession final : public DeviceSession
{
public:
    MockSession(DeviceCapabilities capabilities, std::shared_ptr<MockTrace> trace)
        : m_capabilities(std::move(capabilities))
        , m_trace(std::move(trace))
    {
    }

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override
    {
        return m_capabilities;
    }

    [[nodiscard]] DeviceOperationResult tuneCenterFrequency(
        std::uint64_t frequency, HfTuningMode mode) override
    {
        m_trace->tuningRequests.emplace_back(frequency, mode);
        if (m_trace->throwWhenTuning) {
            throw std::runtime_error("mock driver rejected tuning");
        }
        return {DeviceError::None, true, "Mock device tuned"};
    }

    [[nodiscard]] DeviceOperationResult setPpmCorrection(
        double ppmCorrection) override
    {
        m_trace->ppmRequests.push_back(ppmCorrection);
        if (m_trace->throwWhenSettingPpm) {
            throw std::runtime_error("mock driver rejected PPM correction");
        }
        return {
            DeviceError::None,
            true,
            "Mock PPM correction applied",
            std::nullopt,
            std::nullopt,
            m_trace->effectivePpmCorrection.value_or(ppmCorrection),
        };
    }

    [[nodiscard]] DeviceOperationResult setSampleRate(
        std::uint64_t sampleRate) override
    {
        m_trace->sampleRateRequests.push_back(sampleRate);
        return {
            DeviceError::None,
            true,
            "Mock sample rate applied",
            m_trace->effectiveSampleRate,
        };
    }

    [[nodiscard]] DeviceOperationResult setGain(double gainDb) override
    {
        m_trace->gainRequests.push_back(gainDb);
        return {DeviceError::None, true, "Mock gain applied"};
    }

    [[nodiscard]] DeviceOperationResult startReceiveStream() override
    {
        ++m_trace->streamStarts;
        return {DeviceError::None, true, "Mock stream started"};
    }

    [[nodiscard]] DeviceOperationResult stopReceiveStream() override
    {
        ++m_trace->streamStops;
        return {DeviceError::None, true, "Mock stream stopped"};
    }

    [[nodiscard]] DeviceReadResult readReceiveSamples(
        std::span<std::complex<float>>,
        std::chrono::milliseconds) override
    {
        return {DeviceReadStatus::Timeout, 0, {}};
    }

private:
    DeviceCapabilities m_capabilities;
    std::shared_ptr<MockTrace> m_trace;
};

class MockProvider final : public DeviceProvider
{
public:
    explicit MockProvider(std::shared_ptr<MockTrace> trace)
        : m_trace(std::move(trace))
    {
    }

    [[nodiscard]] DeviceDiscoveryResult discover() override
    {
        return {DeviceError::None, descriptors, "Mock discovery completed"};
    }

    [[nodiscard]] DeviceOpenResult open(const std::string& identifier) override
    {
        m_trace->openedIdentifiers.push_back(identifier);
        if (m_trace->failWhenOpening) {
            return {
                DeviceError::DeviceOpenFailed,
                nullptr,
                "Mock device open failed",
            };
        }
        const auto capabilities = openedCapabilities.find(identifier);
        if (capabilities == openedCapabilities.end()) {
            return {DeviceError::DeviceNotFound, nullptr, "Mock device not found"};
        }
        return {
            DeviceError::None,
            std::make_unique<MockSession>(capabilities->second, m_trace),
            "Mock device opened",
        };
    }

    std::vector<DeviceDescriptor> descriptors;
    std::unordered_map<std::string, DeviceCapabilities> openedCapabilities;

private:
    std::shared_ptr<MockTrace> m_trace;
};

DeviceDescriptor descriptor(std::string identifier, std::string description)
{
    return {
        std::move(identifier),
        true,
        std::move(description),
        "mock",
        "mock-hardware",
        "mock-serial",
        {
            .receive = true,
            .rtlSdrBlogV4 = false,
            .driverManagedHfBelow27Mhz = false,
            .receiveFrequencyRanges = {},
            .hfLimitation = {},
            .ppmCorrectionSupported = false,
            .receiveSampleRateRanges = {},
            .gainSupported = false,
            .minimumGainDb = 0.0,
            .maximumGainDb = 0.0,
            .complexFloat32StreamingSupported = false,
        },
    };
}

DeviceCapabilities ordinaryCapabilities()
{
    return {
        .receive = true,
        .rtlSdrBlogV4 = false,
        .driverManagedHfBelow27Mhz = false,
        .receiveFrequencyRanges = {{1'000'000, 2'000'000'000}},
        .hfLimitation = {},
        .ppmCorrectionSupported = false,
        .receiveSampleRateRanges = {{200'000, 10'000'000}},
        .gainSupported = true,
        .minimumGainDb = -10.0,
        .maximumGainDb = 100.0,
        .complexFloat32StreamingSupported = true,
    };
}

DeviceCapabilities v4Capabilities()
{
    return {
        .receive = true,
        .rtlSdrBlogV4 = true,
        .driverManagedHfBelow27Mhz = true,
        .receiveFrequencyRanges = {{500'000, 1'764'000'000}},
        .hfLimitation = {},
        .ppmCorrectionSupported = false,
        .receiveSampleRateRanges = {{200'000, 3'200'000}},
        .gainSupported = true,
        .minimumGainDb = 0.0,
        .maximumGainDb = 49.6,
        .complexFloat32StreamingSupported = true,
    };
}

}  // namespace

class DeviceControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void discoversHumanReadableDevicesWithoutOpeningThem();
    void representsNoDeviceState();
    void requiresExplicitSelection();
    void recognizesSoapyRtlSdrDriverKeyVariants();
    void detectsConfirmedRtlSdrBlogV4Capability();
    void changesV4HfModeAcrossThreshold();
    void leavesNonV4DevicesInNormalModeBelowThreshold();
    void reportsUnavailableV4DriverControl();
    void containsDriverControlFailures();
    void appliesSupportedPpmCorrection();
    void reportsUnsupportedPpmCorrection();
    void containsPpmDriverFailures();
    void reportsDeviceOpenFailures();
    void validatesSampleRateGainAndStreamLifecycle();
    void distinguishesDiscreteAndRangedSampleRateCapabilities();
};

void DeviceControllerTest::recognizesSoapyRtlSdrDriverKeyVariants()
{
    QVERIFY(isRtlSdrDriver("rtlsdr"));
    QVERIFY(isRtlSdrDriver("RTLSDR"));
    QVERIFY(isRtlSdrDriver("RTL-SDR"));
    QVERIFY(!isRtlSdrDriver("airspy"));
}

void DeviceControllerTest::discoversHumanReadableDevicesWithoutOpeningThem()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {
        descriptor("mock:serial=one", "First receiver [one]"),
        descriptor("mock:serial=two", "Second receiver [two]"),
    };
    DeviceController controller(std::move(provider));

    const auto result = controller.discover();
    QVERIFY(result.succeeded());
    QCOMPARE(controller.devices().size(), std::size_t{2});
    QCOMPARE(controller.devices()[0].displayName, std::string("First receiver [one]"));
    QVERIFY(controller.devices()[0].identifierIsStable);
    QVERIFY(trace->openedIdentifiers.empty());
    QVERIFY(!controller.selectedDevice().has_value());
}

void DeviceControllerTest::representsNoDeviceState()
{
    auto trace = std::make_shared<MockTrace>();
    DeviceController controller(std::make_unique<MockProvider>(trace));

    const auto result = controller.discover();
    QVERIFY(result.succeeded());
    QVERIFY(controller.devices().empty());
    QVERIFY(!controller.selectedDevice().has_value());
    QVERIFY(!controller.centerFrequency().has_value());
}

void DeviceControllerTest::requiresExplicitSelection()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {
        descriptor("mock:serial=one", "First receiver"),
        descriptor("mock:serial=two", "Second receiver"),
    };
    provider->openedCapabilities.emplace("mock:serial=one", ordinaryCapabilities());
    provider->openedCapabilities.emplace("mock:serial=two", ordinaryCapabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(trace->openedIdentifiers.empty());

    const auto result = controller.selectDevice("mock:serial=two");
    QVERIFY(result.succeeded());
    QCOMPARE(trace->openedIdentifiers.size(), std::size_t{1});
    QCOMPARE(trace->openedIdentifiers.front(), std::string("mock:serial=two"));
    QCOMPARE(
        controller.selectedDevice()->identifier,
        std::string("mock:serial=two"));
    QVERIFY(!controller.centerFrequency().has_value());
}

void DeviceControllerTest::detectsConfirmedRtlSdrBlogV4Capability()
{
    const RtlSdrIdentity v4{
        "rtlsdr", "R828D", "RTLSDRBlog", "Blog V4", "Rafael Micro R828D"};
    const auto capabilities =
        detectRtlSdrCapabilities(v4, {{0, 1'764'000'000}});
    QVERIFY(capabilities.rtlSdrBlogV4);
    QVERIFY(capabilities.driverManagedHfBelow27Mhz);
    QVERIFY(capabilities.hfLimitation.empty());
    const std::vector<sdr::radio::FrequencyRange> expectedPracticalRanges{
        {500'000, 1'764'000'000},
    };
    QCOMPARE(capabilities.receiveFrequencyRanges, expectedPracticalRanges);

    RtlSdrIdentity genericR828d = v4;
    genericR828d.manufacturer = "Generic";
    const auto genericCapabilities =
        detectRtlSdrCapabilities(genericR828d, {{0, 1'764'000'000}});
    QVERIFY(!genericCapabilities.rtlSdrBlogV4);
    QVERIFY(!genericCapabilities.driverManagedHfBelow27Mhz);

    const auto rfOnlyHighCapabilities =
        detectRtlSdrCapabilities(v4, {{500'000, 1'764'000'000}});
    QVERIFY(rfOnlyHighCapabilities.rtlSdrBlogV4);
    QVERIFY(!rfOnlyHighCapabilities.driverManagedHfBelow27Mhz);
    QVERIFY(!rfOnlyHighCapabilities.hfLimitation.empty());
}

void DeviceControllerTest::changesV4HfModeAcrossThreshold()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    auto discoveredV4 = descriptor("rtlsdr:serial=v4", "RTL-SDR Blog V4");
    discoveredV4.capabilities.rtlSdrBlogV4 = true;
    discoveredV4.capabilities.hfLimitation = "Stale discovery warning";
    provider->descriptors = {std::move(discoveredV4)};
    provider->openedCapabilities.emplace("rtlsdr:serial=v4", v4Capabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("rtlsdr:serial=v4").succeeded());
    QVERIFY(controller.selectedDevice()->capabilities.hfLimitation.empty());
    const auto belowPracticalMinimum = controller.tuneCenterFrequency(499'999);
    QVERIFY(!belowPracticalMinimum.succeeded());
    QCOMPARE(belowPracticalMinimum.error, DeviceError::FrequencyUnsupported);
    QVERIFY(controller.tuneCenterFrequency(500'000).succeeded());
    QVERIFY(controller.rtlSdrBlogV4HfActive());
    QCOMPARE(
        trace->tuningRequests.back().second,
        HfTuningMode::DriverManagedRtlSdrBlogV4);

    QVERIFY(controller.tuneCenterFrequency(7'100'000).succeeded());
    QCOMPARE(trace->tuningRequests.back().first, std::uint64_t{7'100'000});
    QCOMPARE(
        trace->tuningRequests.back().second,
        HfTuningMode::DriverManagedRtlSdrBlogV4);

    QVERIFY(controller.tuneCenterFrequency(rtlSdrBlogV4HfThresholdHz).succeeded());
    QVERIFY(!controller.rtlSdrBlogV4HfActive());
    QCOMPARE(trace->tuningRequests.back().second, HfTuningMode::Normal);
}

void DeviceControllerTest::leavesNonV4DevicesInNormalModeBelowThreshold()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=ordinary", "Ordinary SDR")};
    provider->openedCapabilities.emplace(
        "mock:serial=ordinary", ordinaryCapabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("mock:serial=ordinary").succeeded());
    QVERIFY(controller.tuneCenterFrequency(10'000'000).succeeded());
    QCOMPARE(trace->tuningRequests.back().second, HfTuningMode::Normal);
    QVERIFY(!controller.rtlSdrBlogV4HfActive());
}

void DeviceControllerTest::reportsUnavailableV4DriverControl()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("rtlsdr:serial=v4", "RTL-SDR Blog V4")};
    DeviceCapabilities unsupported = v4Capabilities();
    unsupported.driverManagedHfBelow27Mhz = false;
    unsupported.hfLimitation = "Mock driver does not expose the V4 HF range";
    provider->openedCapabilities.emplace("rtlsdr:serial=v4", unsupported);
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("rtlsdr:serial=v4").succeeded());
    const auto result = controller.tuneCenterFrequency(10'000'000);
    QVERIFY(!result.succeeded());
    QCOMPARE(result.error, DeviceError::HfControlUnavailable);
    QVERIFY(trace->tuningRequests.empty());
}

void DeviceControllerTest::containsDriverControlFailures()
{
    auto trace = std::make_shared<MockTrace>();
    trace->throwWhenTuning = true;
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("rtlsdr:serial=v4", "RTL-SDR Blog V4")};
    provider->openedCapabilities.emplace("rtlsdr:serial=v4", v4Capabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("rtlsdr:serial=v4").succeeded());
    const auto result = controller.tuneCenterFrequency(10'000'000);
    QVERIFY(!result.succeeded());
    QCOMPARE(result.error, DeviceError::TuningFailed);
    QVERIFY(!controller.centerFrequency().has_value());
    QVERIFY(!controller.rtlSdrBlogV4HfActive());
}

void DeviceControllerTest::appliesSupportedPpmCorrection()
{
    auto trace = std::make_shared<MockTrace>();
    trace->effectivePpmCorrection = -14.0;
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=ppm", "PPM receiver")};
    auto capabilities = ordinaryCapabilities();
    capabilities.ppmCorrectionSupported = true;
    provider->openedCapabilities.emplace("mock:serial=ppm", capabilities);
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("mock:serial=ppm").succeeded());
    QVERIFY(controller.setPpmCorrection(-14.5).succeeded());
    QCOMPARE(controller.ppmCorrection(), std::optional<double>{-14.0});
    QCOMPARE(trace->ppmRequests, std::vector<double>{-14.5});
}

void DeviceControllerTest::reportsUnsupportedPpmCorrection()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=no-ppm", "No PPM")};
    provider->openedCapabilities.emplace(
        "mock:serial=no-ppm", ordinaryCapabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("mock:serial=no-ppm").succeeded());
    const auto result = controller.setPpmCorrection(5.0);
    QVERIFY(!result.succeeded());
    QCOMPARE(result.error, DeviceError::PpmCorrectionUnsupported);
    QVERIFY(trace->ppmRequests.empty());
}

void DeviceControllerTest::containsPpmDriverFailures()
{
    auto trace = std::make_shared<MockTrace>();
    trace->throwWhenSettingPpm = true;
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=ppm", "PPM receiver")};
    auto capabilities = ordinaryCapabilities();
    capabilities.ppmCorrectionSupported = true;
    provider->openedCapabilities.emplace("mock:serial=ppm", capabilities);
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("mock:serial=ppm").succeeded());
    const auto result = controller.setPpmCorrection(7.0);
    QVERIFY(!result.succeeded());
    QCOMPARE(result.error, DeviceError::PpmCorrectionFailed);
    QVERIFY(!controller.ppmCorrection().has_value());
}

void DeviceControllerTest::reportsDeviceOpenFailures()
{
    auto trace = std::make_shared<MockTrace>();
    trace->failWhenOpening = true;
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=fail", "Failing SDR")};
    provider->openedCapabilities.emplace(
        "mock:serial=fail", ordinaryCapabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    const auto result = controller.selectDevice("mock:serial=fail");
    QVERIFY(!result.succeeded());
    QCOMPARE(result.error, DeviceError::DeviceOpenFailed);
    QVERIFY(!controller.selectedDevice().has_value());
}

void DeviceControllerTest::validatesSampleRateGainAndStreamLifecycle()
{
    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=stream", "Streaming SDR")};
    provider->openedCapabilities.emplace(
        "mock:serial=stream", ordinaryCapabilities());
    DeviceController controller(std::move(provider));

    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("mock:serial=stream").succeeded());
    trace->effectiveSampleRate = 2'400'000;
    const auto configuredRate = controller.setSampleRate(2'000'000);
    QVERIFY(configuredRate.succeeded());
    QCOMPARE(controller.sampleRate(), std::optional<std::uint64_t>{2'000'000});
    QCOMPARE(
        controller.effectiveSampleRate(),
        std::optional<std::uint64_t>{2'400'000});
    const auto unsupportedRate = controller.setSampleRate(20'000'000);
    QCOMPARE(unsupportedRate.error, DeviceError::SampleRateUnsupported);

    QVERIFY(controller.setGain(20.0).succeeded());
    QCOMPARE(controller.gain(), std::optional<double>{20.0});
    const auto unsupportedGain = controller.setGain(101.0);
    QCOMPARE(unsupportedGain.error, DeviceError::GainUnsupported);

    QVERIFY(controller.startReceiveStream().succeeded());
    QVERIFY(controller.receiveStreamActive());
    std::array<std::complex<float>, 16> samples;
    const auto read = controller.readReceiveSamples(
        samples, std::chrono::milliseconds(1));
    QCOMPARE(read.status, DeviceReadStatus::Timeout);
    QVERIFY(controller.stopReceiveStream().succeeded());
    QVERIFY(!controller.receiveStreamActive());
}

void DeviceControllerTest::distinguishesDiscreteAndRangedSampleRateCapabilities()
{
    DeviceCapabilities discrete = ordinaryCapabilities();
    discrete.receiveSampleRateRanges = {
        {1'024'000, 1'024'000},
        {2'400'000, 2'400'000},
    };
    QVERIFY(supportsReceiveSampleRate(discrete, 1'024'000));
    QVERIFY(!supportsReceiveSampleRate(discrete, 2'000'000));
    QVERIFY(!allowsCustomReceiveSampleRate(discrete));

    DeviceCapabilities ranged = ordinaryCapabilities();
    ranged.receiveSampleRateRanges = {{1'000'000, 3'200'000}};
    QVERIFY(supportsReceiveSampleRate(ranged, 2'345'678));
    QVERIFY(!supportsReceiveSampleRate(ranged, 900'000));
    QVERIFY(allowsCustomReceiveSampleRate(ranged));

    auto trace = std::make_shared<MockTrace>();
    auto provider = std::make_unique<MockProvider>(trace);
    provider->descriptors = {descriptor("mock:serial=discrete", "Discrete SDR")};
    provider->openedCapabilities.emplace("mock:serial=discrete", discrete);
    DeviceController controller(std::move(provider));
    QVERIFY(controller.discover().succeeded());
    QVERIFY(controller.selectDevice("mock:serial=discrete").succeeded());
    QVERIFY(controller.setSampleRate(2'400'000).succeeded());
    QCOMPARE(
        controller.setSampleRate(2'000'000).error,
        DeviceError::SampleRateUnsupported);
}

QTEST_GUILESS_MAIN(DeviceControllerTest)

#include "DeviceControllerTest.moc"
