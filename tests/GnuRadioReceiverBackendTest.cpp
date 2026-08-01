// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "GnuRadioReceiverBackend.hpp"
#include "DeviceController.hpp"
#include "FlowgraphLifecycle.hpp"
#include "WidebandIqSources.hpp"

#include <QElapsedTimer>
#include <QtTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using sdr::dsp::GnuRadioReceiverBackend;

namespace {

using namespace sdr::devices;

struct HardwareTrace {
    enum class ModulatedSignal {
        OffsetCarrier,
        AmTone,
        NfmTone,
        WfmTone,
        UsbTone,
        LsbTone,
        Noise,
    };

    std::vector<std::string> openedIdentifiers;
    std::vector<std::pair<std::uint64_t, HfTuningMode>> tuningRequests;
    std::vector<std::uint64_t> sampleRates;
    std::vector<double> gains;
    std::vector<double> ppmCorrections;
    int streamStarts = 0;
    int streamStops = 0;
    int testStreamStarts = 0;
    int testStreamStops = 0;
    int reads = 0;
    int transientReadTimeouts = 0;
    std::uint64_t effectiveSampleRate = 2'000'000;
    std::atomic<ModulatedSignal> modulatedSignal =
        ModulatedSignal::OffsetCarrier;
    float signalAmplitude = 1.0F;
    double signalOffsetHz = 0.0;
    bool timeoutEveryRead = false;
    bool failStreamStart = false;
    bool failStreamStop = false;
    bool failSampleRateChange = false;
    bool disconnectOnRead = false;
};

class StreamingMockSession final : public DeviceSession
{
public:
    StreamingMockSession(
        DeviceCapabilities capabilities, std::shared_ptr<HardwareTrace> trace)
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
        return {DeviceError::None, true, "Mock hardware tuned"};
    }

    [[nodiscard]] DeviceOperationResult setPpmCorrection(double ppm) override
    {
        m_trace->ppmCorrections.push_back(ppm);
        return {DeviceError::None, true, "Mock hardware PPM applied"};
    }

    [[nodiscard]] DeviceOperationResult setSampleRate(
        std::uint64_t sampleRate) override
    {
        if (m_trace->failSampleRateChange) {
            return {
                DeviceError::SampleRateUnsupported,
                false,
                "Mock hardware rejected sample-rate change",
            };
        }
        m_sampleRate = m_trace->effectiveSampleRate;
        m_trace->sampleRates.push_back(sampleRate);
        return {
            DeviceError::None,
            true,
            "Mock hardware sample rate applied",
            m_sampleRate,
        };
    }

    [[nodiscard]] DeviceOperationResult setGain(double gainDb) override
    {
        m_trace->gains.push_back(gainDb);
        return {DeviceError::None, true, "Mock hardware gain applied"};
    }

    [[nodiscard]] DeviceOperationResult startReceiveStream() override
    {
        ++m_trace->streamStarts;
        if (m_trace->failStreamStart) {
            return {DeviceError::StreamStartFailed, false, "Mock stream start failed"};
        }
        return {DeviceError::None, true, "Mock hardware stream started"};
    }

    [[nodiscard]] DeviceOperationResult stopReceiveStream() override
    {
        ++m_trace->streamStops;
        if (m_trace->failStreamStop) {
            return {DeviceError::StreamFailed, false, "Mock stream stop failed"};
        }
        return {DeviceError::None, true, "Mock hardware stream stopped"};
    }

    [[nodiscard]] DeviceReadResult readReceiveSamples(
        std::span<std::complex<float>> samples,
        std::chrono::milliseconds) override
    {
        ++m_trace->reads;
        if (m_trace->timeoutEveryRead || m_trace->transientReadTimeouts > 0) {
            if (m_trace->transientReadTimeouts > 0) {
                --m_trace->transientReadTimeouts;
            }
            return {DeviceReadStatus::Timeout, 0, {}};
        }
        if (m_trace->disconnectOnRead) {
            return {
                DeviceReadStatus::Disconnected,
                0,
                "Mock SDR device disconnected",
            };
        }

        constexpr double twoPi = 6.283185307179586;
        constexpr double toneOffsetHz = 100'000.0;
        constexpr double audioToneHz = 1'000.0;
        const double audioPhaseStep = twoPi * audioToneHz /
                                      static_cast<double>(m_sampleRate);
        const double carrierPhaseStep = twoPi * m_trace->signalOffsetHz /
                                        static_cast<double>(m_sampleRate);
        for (auto& sample : samples) {
            const double audio = std::sin(m_audioPhase);
            switch (m_trace->modulatedSignal.load(std::memory_order_relaxed)) {
            case HardwareTrace::ModulatedSignal::OffsetCarrier:
                sample = std::complex<float>(
                    static_cast<float>(0.5 * std::cos(m_phase)),
                    static_cast<float>(0.5 * std::sin(m_phase)));
                m_phase += twoPi * toneOffsetHz /
                           static_cast<double>(m_sampleRate);
                break;
            case HardwareTrace::ModulatedSignal::AmTone:
                sample = std::polar(
                    m_trace->signalAmplitude *
                        static_cast<float>(1.0 + 0.5 * audio),
                    static_cast<float>(m_carrierPhase));
                break;
            case HardwareTrace::ModulatedSignal::NfmTone:
                m_phase += twoPi * 2'500.0 * audio /
                           static_cast<double>(m_sampleRate);
                sample = std::polar(
                    m_trace->signalAmplitude,
                    static_cast<float>(m_carrierPhase + m_phase));
                break;
            case HardwareTrace::ModulatedSignal::WfmTone:
                m_phase += twoPi * 25'000.0 * audio /
                           static_cast<double>(m_sampleRate);
                sample = std::polar(
                    m_trace->signalAmplitude,
                    static_cast<float>(m_carrierPhase + m_phase));
                break;
            case HardwareTrace::ModulatedSignal::UsbTone:
                sample = std::polar(
                    m_trace->signalAmplitude,
                    static_cast<float>(m_carrierPhase + m_audioPhase));
                break;
            case HardwareTrace::ModulatedSignal::LsbTone:
                sample = std::polar(
                    m_trace->signalAmplitude,
                    static_cast<float>(m_carrierPhase - m_audioPhase));
                break;
            case HardwareTrace::ModulatedSignal::Noise: {
                const auto nextNoiseComponent = [this] {
                    m_noiseState = m_noiseState * 1'664'525U + 1'013'904'223U;
                    return static_cast<float>(m_noiseState >> 8U) /
                               static_cast<float>(0x00FF'FFFFU) *
                               2.0F -
                           1.0F;
                };
                sample = m_trace->signalAmplitude * std::complex<float>{
                    nextNoiseComponent(), nextNoiseComponent()};
                break;
            }
            }
            m_phase = std::remainder(m_phase, twoPi);
            m_carrierPhase = std::remainder(
                m_carrierPhase + carrierPhaseStep, twoPi);
            m_audioPhase += audioPhaseStep;
            if (m_audioPhase >= twoPi) {
                m_audioPhase -= twoPi;
            }
        }
        return {DeviceReadStatus::Samples, samples.size(), {}};
    }

    [[nodiscard]] DeviceOperationResult startRtlSdrTestStream() override
    {
        ++m_trace->testStreamStarts;
        return {DeviceError::None, true, "Mock RTL-SDR test stream started"};
    }

    [[nodiscard]] DeviceOperationResult stopRtlSdrTestStream() override
    {
        ++m_trace->testStreamStops;
        return {DeviceError::None, true, "Mock RTL-SDR test stream stopped"};
    }

private:
    DeviceCapabilities m_capabilities;
    std::shared_ptr<HardwareTrace> m_trace;
    std::uint64_t m_sampleRate = 2'000'000;
    double m_phase = 0.0;
    double m_carrierPhase = 0.0;
    double m_audioPhase = 0.0;
    std::uint32_t m_noiseState = 0x1234'5678U;
};

class StreamingMockProvider final : public DeviceProvider
{
public:
    StreamingMockProvider(
        DeviceDescriptor descriptor, std::shared_ptr<HardwareTrace> trace)
        : m_descriptor(std::move(descriptor))
        , m_trace(std::move(trace))
    {
    }

    [[nodiscard]] DeviceDiscoveryResult discover() override
    {
        return {DeviceError::None, {m_descriptor}, "Mock hardware discovered"};
    }

    [[nodiscard]] DeviceOpenResult open(const std::string& identifier) override
    {
        m_trace->openedIdentifiers.push_back(identifier);
        if (identifier != m_descriptor.identifier) {
            return {DeviceError::DeviceNotFound, nullptr, "Mock hardware not found"};
        }
        return {
            DeviceError::None,
            std::make_unique<StreamingMockSession>(
                m_descriptor.capabilities, m_trace),
            "Mock hardware opened",
        };
    }

private:
    DeviceDescriptor m_descriptor;
    std::shared_ptr<HardwareTrace> m_trace;
};

DeviceCapabilities streamingCapabilities(bool rtlSdrBlogV4 = false)
{
    return {
        .receive = true,
        .rtlSdrBlogV4 = rtlSdrBlogV4,
        .driverManagedHfBelow27Mhz = rtlSdrBlogV4,
        .receiveFrequencyRanges = {{
            rtlSdrBlogV4 ? std::uint64_t{500'000} : std::uint64_t{0},
            1'500'000'000,
        }},
        .hfLimitation = {},
        .ppmCorrectionSupported = true,
        .rtlSdrTestModeSupported = false,
        .receiveSampleRateRanges = {{1'000'000, 3'200'000}},
        .gainSupported = true,
        .minimumGainDb = -10.0,
        .maximumGainDb = 50.0,
        .complexFloat32StreamingSupported = true,
    };
}

std::unique_ptr<DeviceController> explicitlySelectedController(
    std::shared_ptr<HardwareTrace> trace,
    DeviceCapabilities capabilities = streamingCapabilities())
{
    DeviceDescriptor descriptor{
        "mock:serial=selected",
        true,
        "Explicit mock SDR",
        "mock",
        "mock-hardware",
        "selected",
        std::move(capabilities),
    };
    auto controller = std::make_unique<DeviceController>(
        std::make_unique<StreamingMockProvider>(descriptor, trace));
    if (!controller->discover().succeeded() ||
        !controller->selectDevice("mock:serial=selected").succeeded()) {
        return nullptr;
    }
    return controller;
}

std::vector<float> waitForAudioSamples(
    GnuRadioReceiverBackend& receiver, bool requireSignal = false)
{
    std::vector<float> samples;
    for (int attempt = 0; attempt < 30; ++attempt) {
        QTest::qWait(10);
        samples = receiver.takeAudioSamples(4'096);
        if (!samples.empty() &&
            (!requireSignal || std::ranges::any_of(samples, [](float sample) {
                return std::abs(sample) > 0.01F;
            }))) {
            return samples;
        }
    }
    return samples;
}

std::vector<float> waitForDecoderSamples(
    GnuRadioReceiverBackend& receiver)
{
    std::vector<float> samples;
    for (int attempt = 0; attempt < 40; ++attempt) {
        QTest::qWait(10);
        samples = receiver.takeDecoderInputSamples(4'096);
        if (!samples.empty()) {
            return samples;
        }
    }
    return samples;
}

std::vector<float> collectAudioSamples(GnuRadioReceiverBackend& receiver)
{
    std::vector<float> samples;
    for (int attempt = 0; attempt < 120 && samples.size() < 16'384; ++attempt) {
        QTest::qWait(10);
        auto chunk = receiver.takeAudioSamples(4'096);
        samples.insert(samples.end(), chunk.begin(), chunk.end());
    }
    return samples;
}

struct ToneMeasurement {
    double frequencyHz = 0.0;
    double rms = 0.0;
};

ToneMeasurement measureAudioTone(
    const std::vector<float>& samples, std::size_t settlingSamples = 1'024)
{
    constexpr double audioSampleRate = 48'000.0;
    if (samples.size() <= settlingSamples + 2) {
        return {};
    }

    const auto first = samples.begin() + static_cast<std::ptrdiff_t>(settlingSamples);
    const double mean = std::accumulate(first, samples.end(), 0.0) /
                        static_cast<double>(samples.end() - first);
    double sumSquares = 0.0;
    std::size_t risingCrossings = 0;
    double previous = static_cast<double>(*first) - mean;
    for (auto it = first + 1; it != samples.end(); ++it) {
        const double current = static_cast<double>(*it) - mean;
        sumSquares += current * current;
        if (previous <= 0.0 && current > 0.0) {
            ++risingCrossings;
        }
        previous = current;
    }

    const double count = static_cast<double>(samples.end() - first - 1);
    return {
        static_cast<double>(risingCrossings) * audioSampleRate / count,
        std::sqrt(sumSquares / count),
    };
}

}  // namespace

class GnuRadioReceiverBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void constructsAndDestroysWhileStopped();
    void startsStopsAndWaitsRepeatedly();
    void stopsAndWaitsDuringDestruction();
    void cleansUpAfterSimulatedSchedulerStartFailure();
    void destroysSafelyAfterPartialInitialization();
    void exposesSourceCapabilitiesAndAdapters();
    void tracksCenterListeningOffset();
    void rebuildsRunningFlowgraphForSampleRateChange();
    void repeatsCaptureBandwidthChangesWithoutStaleFrames();
    void stopsSafelyWhenRunningSampleRateReconfigurationFails();
    void producesDisplayReadySpectrumFrames();
    void tagsFramesWithAcquisitionMetadataAcrossRetunes();
    void supportsConfiguredSpectrumFftSizes();
    void generatesWindowHopsBeforeExecutingSpectrumFfts();
    void changesSpectrumFftSizeWhileAudioContinues();
    void changesWaterfallRowRateWhileAudioContinues();
    void updatesModesFiltersAndSquelchWhileRunning();
    void producesUnsquelchedDigitalDiscriminatorWithoutStoppingSpectrum();
    void updatesFilterTapsWithoutRestartingRunningFlowgraph();
    void reportsPpmUnsupportedWithoutHardware();
    void measuresPpmWithDriverCorrectionReset();
    void demodulatesSyntheticSignals();
    void demodulatesKnownHardwareTonesAtTheEffectiveRate();
    void keepsWeakAmAndSidebandSignalsAudibleAndBounded();
    void keepsAmAndSidebandReceiverNoiseAudible();
    void rejectsTheOppositeSideband();
    void appliesListeningOffsetToControlledSignals();
    void switchesControlledModesWithoutStaleOrInvalidAudio();
    void requiresAnExplicitlySelectedHardwareDevice();
    void configuresAndStreamsExplicitlySelectedHardware();
    void continuesProducingHardwareSpectrumFrames();
    void continuesAfterTransientHardwareReadTimeout();
    void stopsWhileHardwareReadsTimeOut();
    void appliesV4HfPolicyThroughTheHardwarePath();
    void rejectsUnsupportedHardwareConfiguration();
    void reportsHardwareStreamStartAndDisconnectFailures();
    void synchronizesStoppedStateAfterHardwareCleanupFailure();
};

void GnuRadioReceiverBackendTest::constructsAndDestroysWhileStopped()
{
    const GnuRadioReceiverBackend receiver;
    QVERIFY(!receiver.state().running);
    QCOMPARE(receiver.frequencyTranslationOffsetHz(), 0.0);
}

void GnuRadioReceiverBackendTest::startsStopsAndWaitsRepeatedly()
{
    GnuRadioReceiverBackend receiver;

    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto startResult = receiver.startReception();
        QVERIFY(startResult.succeeded());
        QVERIFY(startResult.stateChanged);
        QVERIFY(receiver.state().running);

        const auto repeatedStart = receiver.startReception();
        QVERIFY(repeatedStart.succeeded());
        QVERIFY(!repeatedStart.stateChanged);

        const auto stopResult = receiver.stopReception();
        QVERIFY(stopResult.succeeded());
        QVERIFY(stopResult.stateChanged);
        QVERIFY(!receiver.state().running);

        const auto repeatedStop = receiver.stopReception();
        QVERIFY(repeatedStop.succeeded());
        QVERIFY(!repeatedStop.stateChanged);
    }
}

void GnuRadioReceiverBackendTest::stopsAndWaitsDuringDestruction()
{
    auto receiver = std::make_unique<GnuRadioReceiverBackend>();
    QVERIFY(receiver->startReception().succeeded());
    QVERIFY(receiver->state().running);
    receiver.reset();
}

void GnuRadioReceiverBackendTest::cleansUpAfterSimulatedSchedulerStartFailure()
{
    int schedulerStarts = 0;
    int schedulerStops = 0;
    int schedulerWaits = 0;
    int sourceStarts = 0;
    int sourceStops = 0;
    sdr::dsp::detail::FlowgraphLifecycle lifecycle({
        .startScheduler = [&] {
            ++schedulerStarts;
            throw std::runtime_error("Simulated scheduler start failure");
        },
        .stopScheduler = [&] { ++schedulerStops; },
        .waitScheduler = [&] { ++schedulerWaits; },
        .startSource = [&] { ++sourceStarts; },
        .stopSource = [&] { ++sourceStops; },
    });

    QVERIFY_EXCEPTION_THROWN(lifecycle.start(), std::runtime_error);
    QVERIFY(!lifecycle.running());
    QCOMPARE(schedulerStarts, 1);
    QCOMPARE(schedulerStops, 1);
    QCOMPARE(schedulerWaits, 1);
    QCOMPARE(sourceStarts, 1);
    QCOMPARE(sourceStops, 1);

    lifecycle.stopAndWait();
    QCOMPARE(schedulerStops, 1);
    QCOMPARE(schedulerWaits, 1);
    QCOMPARE(sourceStops, 1);
}

void GnuRadioReceiverBackendTest::destroysSafelyAfterPartialInitialization()
{
    int schedulerStops = 0;
    int schedulerWaits = 0;
    int sourceStops = 0;
    {
        sdr::dsp::detail::FlowgraphLifecycle lifecycle({
            .startScheduler = [] {},
            .stopScheduler = [&] { ++schedulerStops; },
            .waitScheduler = [&] { ++schedulerWaits; },
            .startSource = [] {},
            .stopSource = [&] { ++sourceStops; },
        });
        QVERIFY_EXCEPTION_THROWN(
            lifecycle.start([] {
                throw std::runtime_error("Simulated post-start failure");
            }),
            std::runtime_error);
        QVERIFY(!lifecycle.running());
    }

    QCOMPARE(schedulerStops, 1);
    QCOMPARE(schedulerWaits, 1);
    QCOMPARE(sourceStops, 1);
}

void GnuRadioReceiverBackendTest::exposesSourceCapabilitiesAndAdapters()
{
    const sdr::radio::WidebandIqCaptureMetadata metadata{
        .centerFrequency = 100'000'000,
        .effectiveSampleRate = 2'000'000,
    };
    sdr::dsp::SyntheticIqSource synthetic(metadata);
    QVERIFY(synthetic.capabilities().kind == sdr::radio::ReceiverSourceKind::Synthetic);
    QCOMPARE(synthetic.captureMetadata().centerFrequency, metadata.centerFrequency);
    std::array<std::complex<float>, 32> samples{};
    QVERIFY(synthetic.read(samples, std::chrono::milliseconds(1)).status ==
            sdr::radio::WidebandIqReadStatus::Stopped);
    QVERIFY(synthetic.start().succeeded);
    const auto syntheticRead = synthetic.read(samples, std::chrono::milliseconds(20));
    QVERIFY(syntheticRead.status == sdr::radio::WidebandIqReadStatus::Samples);
    QCOMPARE(syntheticRead.sampleCount, samples.size());
    QVERIFY(std::abs(samples.front()) > 0.1F);
    QVERIFY(synthetic.stop().succeeded);

    auto trace = std::make_shared<HardwareTrace>();
    auto controller = std::shared_ptr<DeviceController>(
        explicitlySelectedController(trace));
    QVERIFY(controller != nullptr);
    sdr::dsp::DeviceControllerIqSource hardware(controller, metadata);
    const auto hardwareCapabilities = hardware.capabilities();
    QVERIFY(hardwareCapabilities.kind == sdr::radio::ReceiverSourceKind::Hardware);
    QVERIFY(hardwareCapabilities.hardwareTuningSupported);
    QVERIFY(hardwareCapabilities.gainControlSupported);
    QVERIFY(hardwareCapabilities.ppmCorrectionSupported);
    QVERIFY(hardware.read(samples, std::chrono::milliseconds(1)).status ==
            sdr::radio::WidebandIqReadStatus::Stopped);
    QVERIFY(hardware.start().succeeded);
    const auto hardwareRead = hardware.read(samples, std::chrono::milliseconds(1));
    QVERIFY(hardwareRead.status == sdr::radio::WidebandIqReadStatus::Samples);
    QCOMPARE(hardwareRead.sampleCount, samples.size());
    QVERIFY(hardware.stop().succeeded);
    QCOMPARE(trace->streamStarts, 1);
    QCOMPARE(trace->streamStops, 1);

    GnuRadioReceiverBackend syntheticBackend;
    QVERIFY(syntheticBackend.sourceCapabilities().kind ==
            sdr::radio::ReceiverSourceKind::Synthetic);
    GnuRadioReceiverBackend hardwareBackend(explicitlySelectedController(trace));
    QVERIFY(hardwareBackend.sourceCapabilities().kind ==
            sdr::radio::ReceiverSourceKind::Hardware);
    QVERIFY(hardwareBackend.sourceCapabilities().hardwareTuningSupported);
}

void GnuRadioReceiverBackendTest::tracksCenterListeningOffset()
{
    GnuRadioReceiverBackend receiver;

    QVERIFY(receiver.setListeningFrequency(100'250'000).succeeded());
    QCOMPARE(receiver.frequencyTranslationOffsetHz(), 250'000.0);

    QVERIFY(receiver.shiftCenterFrequency(10'000).succeeded());
    QCOMPARE(receiver.state().centerFrequency, std::uint64_t{100'010'000});
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{100'010'000});
    QCOMPARE(receiver.frequencyTranslationOffsetHz(), 0.0);

    const auto directCenterChange = receiver.setCenterFrequency(200'000'000);
    QVERIFY(directCenterChange.succeeded());
    QVERIFY(!directCenterChange.adjusted);
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{200'000'000});
    QCOMPARE(receiver.frequencyTranslationOffsetHz(), 0.0);
}

void GnuRadioReceiverBackendTest::rebuildsRunningFlowgraphForSampleRateChange()
{
    GnuRadioReceiverBackend receiver;

    QVERIFY(receiver.startReception().succeeded());
    const auto result = receiver.setSampleRate(1'000'000);
    QVERIFY(result.succeeded());
    QCOMPARE(receiver.state().sampleRate, std::uint64_t{1'000'000});
    QVERIFY(receiver.state().running);
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::repeatsCaptureBandwidthChangesWithoutStaleFrames()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.startReception().succeeded());

    constexpr std::array<std::uint64_t, 5> rates{
        250'000,
        1'000'000,
        2'000'000,
        2'250'000,
        2'400'000,
    };
    for (const auto rate : rates) {
        QVERIFY2(receiver.setSampleRate(rate).succeeded(), "sample-rate change failed");
        QVERIFY(receiver.state().running);

        std::optional<sdr::radio::SpectrumFrame> frame;
        for (int attempt = 0; attempt < 30 && !frame.has_value(); ++attempt) {
            QTest::qWait(10);
            frame = receiver.takeLatestSpectrumFrame();
        }
        QVERIFY(frame.has_value());
        QCOMPARE(frame->sampleRate, rate);
    }
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::stopsSafelyWhenRunningSampleRateReconfigurationFails()
{
    auto trace = std::make_shared<HardwareTrace>();
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));

    QVERIFY(receiver.startReception().succeeded());
    QVERIFY(receiver.state().running);
    trace->failSampleRateChange = true;

    const auto result = receiver.setSampleRate(1'000'000);
    QVERIFY(!result.succeeded());
    QVERIFY(result.message.find("rejected sample-rate") != std::string::npos);
    QVERIFY(!receiver.state().running);
    QCOMPARE(receiver.state().sampleRate, std::uint64_t{2'000'000});
    QVERIFY(trace->streamStops >= 1);
    QVERIFY(!receiver.takeLatestSpectrumFrame().has_value());
}

void GnuRadioReceiverBackendTest::producesDisplayReadySpectrumFrames()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.startReception().succeeded());

    std::optional<sdr::radio::SpectrumFrame> frame;
    for (int attempt = 0; attempt < 20 && !frame.has_value(); ++attempt) {
        QTest::qWait(10);
        frame = receiver.takeLatestSpectrumFrame();
    }

    QVERIFY(frame.has_value());
    QCOMPARE(frame->normalizedMagnitudes.size(), std::size_t{4'096});
    QCOMPARE(frame->fftSize, std::size_t{4'096});
    QCOMPARE(frame->centerFrequency, receiver.state().centerFrequency);
    QCOMPARE(frame->sampleRate, receiver.state().sampleRate);
    QCOMPARE(frame->tuningGeneration, receiver.tuningGeneration());
    QVERIFY(std::ranges::all_of(
        frame->normalizedMagnitudes,
        [](float magnitude) { return magnitude >= 0.0F && magnitude <= 1.0F; }));
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::tagsFramesWithAcquisitionMetadataAcrossRetunes()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.startReception().succeeded());

    const auto waitForGeneration = [&receiver](
                                       std::uint64_t expectedCenter,
                                       std::uint64_t expectedGeneration) {
        std::optional<sdr::radio::SpectrumFrame> matching;
        for (int attempt = 0; attempt < 60 && !matching.has_value(); ++attempt) {
            QTest::qWait(10);
            auto frames = receiver.takePendingSpectrumFrames(64);
            for (const auto& frame : frames) {
                if (!sdr::radio::hasConsistentMetadata(frame)) {
                    QTest::qFail(
                        "FFT frame metadata was inconsistent", __FILE__, __LINE__);
                    return std::optional<sdr::radio::SpectrumFrame>{};
                }
                if (frame.tuningGeneration == expectedGeneration) {
                    if (frame.centerFrequency != expectedCenter) {
                        QTest::qFail(
                            "Tuning generation used the wrong center frequency",
                            __FILE__,
                            __LINE__);
                        return std::optional<sdr::radio::SpectrumFrame>{};
                    }
                    matching = frame;
                }
            }
        }
        return matching;
    };

    QVERIFY(waitForGeneration(100'000'000, 0).has_value());
    QVERIFY(receiver.shiftCenterFrequency(125'000).succeeded());
    QCOMPARE(receiver.tuningGeneration(), std::uint64_t{1});
    QVERIFY(waitForGeneration(100'125'000, 1).has_value());

    QVERIFY(receiver.shiftCenterFrequency(-200'000).succeeded());
    QCOMPARE(receiver.tuningGeneration(), std::uint64_t{2});
    const auto latest = waitForGeneration(99'925'000, 2);
    QVERIFY(latest.has_value());
    QVERIFY(sdr::radio::isCurrentTuningFrame(
        *latest,
        receiver.state().centerFrequency,
        receiver.effectiveSampleRate(),
        receiver.tuningGeneration()));
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::supportsConfiguredSpectrumFftSizes()
{
    std::vector<float> peakLevels;
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U,
             16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
        GnuRadioReceiverBackend receiver({
            .fftSize = fftSize,
            .targetFramesPerSecond = 25,
        });
        QVERIFY(receiver.startReception().succeeded());
        std::optional<sdr::radio::SpectrumFrame> frame;
        for (int attempt = 0; attempt < 30 && !frame.has_value(); ++attempt) {
            QTest::qWait(10);
            frame = receiver.takeLatestSpectrumFrame();
        }
        QVERIFY(frame.has_value());
        QCOMPARE(frame->normalizedMagnitudes.size(), fftSize);
        peakLevels.push_back(*std::max_element(
            frame->normalizedMagnitudes.begin(),
            frame->normalizedMagnitudes.end()));
        const auto metrics = receiver.spectrumProcessingMetrics();
        QCOMPARE(metrics.fftSize, fftSize);
        QVERIFY(std::abs(
                    metrics.hertzPerBin -
                    static_cast<double>(receiver.effectiveSampleRate()) /
                        static_cast<double>(fftSize)) < 0.01);
        QVERIFY(receiver.stopReception().succeeded());
    }
    const auto [minimumPeak, maximumPeak] = std::minmax_element(
        peakLevels.begin(), peakLevels.end());
    QVERIFY(*maximumPeak - *minimumPeak < 0.02F);

    QVERIFY_EXCEPTION_THROWN(
        GnuRadioReceiverBackend({.fftSize = 256}),
        std::invalid_argument);
}

void GnuRadioReceiverBackendTest::generatesWindowHopsBeforeExecutingSpectrumFfts()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.setSampleRate(1'000'000).succeeded());
    QVERIFY(receiver.startReception().succeeded());

    QVERIFY(QTest::qWaitFor([&receiver] {
        const auto metrics = receiver.spectrumProcessingMetrics();
        return metrics.fftsExecuted > 0 && metrics.framesPublished > 0;
    }));
    const auto metrics = receiver.spectrumProcessingMetrics();
    QVERIFY(metrics.fftsExecuted > 0);
    QVERIFY(metrics.framesPublished > 0);
    QVERIFY(metrics.vectorsReceived >= metrics.fftsExecuted);
    QVERIFY(metrics.inputSamples >= 4'096U);
    QCOMPARE(metrics.effectiveSampleRate, 1'000'000.0);
    QCOMPARE(metrics.targetFramesPerSecond, 60.0);
    QVERIFY(std::abs(metrics.hopSize - 1'000'000.0 / 60.0) < 0.001);
    QCOMPARE(metrics.overlapPercentage, 0.0);
    QVERIFY(std::abs(metrics.hertzPerBin -
                     1'000'000.0 / 4'096.0) < 0.001);
    QVERIFY(metrics.queueDepth <= 64U);
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::changesSpectrumFftSizeWhileAudioContinues()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.startReception().succeeded());
    QTest::qWait(100);
    const auto audioBefore = receiver.audioProducedSamples();

    for (const std::size_t fftSize : {
             8'192U, 16'384U, 32'768U, 65'536U,
             131'072U, 262'144U, 4'096U}) {
        const auto result = receiver.setSpectrumFftSize(fftSize);
        QVERIFY2(result.succeeded(), result.message.c_str());
        QVERIFY(result.stateChanged);
        QVERIFY(receiver.state().running);
        QCOMPARE(receiver.spectrumFftSize(), fftSize);

        std::optional<sdr::radio::SpectrumFrame> frame;
        for (int attempt = 0; attempt < 50 && !frame.has_value(); ++attempt) {
            QTest::qWait(10);
            auto candidate = receiver.takeLatestSpectrumFrame();
            if (candidate && candidate->fftSize == fftSize) {
                frame = std::move(candidate);
            }
        }
        QVERIFY(frame.has_value());
        QCOMPARE(frame->normalizedMagnitudes.size(), fftSize);
    }

    QVERIFY(receiver.audioProducedSamples() > audioBefore);
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::changesWaterfallRowRateWhileAudioContinues()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.setSampleRate(250'000).succeeded());
    QVERIFY(receiver.setSpectrumFftSize(65'536).succeeded());
    QVERIFY(receiver.startReception().succeeded());
    QTest::qWait(100);
    const auto audioBefore = receiver.audioProducedSamples();
    std::uint64_t previousTimestamp = 0;

    for (const std::uint32_t rowsPerSecond : {15U, 30U, 60U, 120U}) {
        const auto result = receiver.setSpectrumFramesPerSecond(rowsPerSecond);
        QVERIFY2(result.succeeded(), result.message.c_str());
        QVERIFY(receiver.state().running);
        QCOMPARE(receiver.spectrumFramesPerSecond(), rowsPerSecond);
        const auto metrics = receiver.spectrumProcessingMetrics();
        QCOMPARE(metrics.targetFramesPerSecond,
                 static_cast<double>(rowsPerSecond));
        const double expectedHop = std::max(
            static_cast<double>(std::size_t{65'536}),
            static_cast<double>(receiver.effectiveSampleRate()) /
                static_cast<double>(rowsPerSecond));
        QVERIFY(std::abs(
                    metrics.hopSize - expectedHop) < 0.001);
        QVERIFY(std::abs(
                    metrics.achievableFramesPerSecond -
                    std::min(
                        static_cast<double>(rowsPerSecond),
                        static_cast<double>(receiver.effectiveSampleRate()) /
                            65'536.0)) < 0.001);

        std::optional<sdr::radio::SpectrumFrame> frame;
        for (int attempt = 0; attempt < 80 && !frame.has_value(); ++attempt) {
            QTest::qWait(5);
            frame = receiver.takeLatestSpectrumFrame();
        }
        QVERIFY(frame.has_value());
        QVERIFY(frame->timestampNanoseconds > previousTimestamp);
        previousTimestamp = frame->timestampNanoseconds;
        QCOMPARE(frame->fftSize, std::size_t{65'536});
    }

    QVERIFY(receiver.audioProducedSamples() > audioBefore);
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::updatesModesFiltersAndSquelchWhileRunning()
{
    GnuRadioReceiverBackend receiver;
    QVERIFY(receiver.startReception().succeeded());

    const auto wfm = receiver.setDemodulationMode(
        sdr::radio::DemodulationMode::Wfm);
    QVERIFY(wfm.succeeded());
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{180'000});
    QVERIFY(receiver.setFilterWidth(200'000).succeeded());

    QVERIFY(receiver.setDemodulationMode(
        sdr::radio::DemodulationMode::Usb).succeeded());
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{2'400});
    QVERIFY(receiver.setDemodulationMode(
        sdr::radio::DemodulationMode::Lsb).succeeded());

    QVERIFY(receiver.setSquelchLevel(-70.0).succeeded());
    QVERIFY(QTest::qWaitFor([&receiver] {
        return receiver.squelchSignalStrengthDb().has_value();
    }));
    QVERIFY(receiver.disableSquelch().succeeded());
    QVERIFY(receiver.enableManualSquelch().succeeded());
    QCOMPARE(receiver.state().squelchLevelDb, -70.0);
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::
    producesUnsquelchedDigitalDiscriminatorWithoutStoppingSpectrum()
{
    auto trace = std::make_shared<HardwareTrace>();
    trace->modulatedSignal = HardwareTrace::ModulatedSignal::NfmTone;
    trace->signalAmplitude = 0.01F;
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
    QVERIFY(receiver.setDemodulationMode(
        sdr::radio::DemodulationMode::DigitalDecoderOutput).succeeded());
    QVERIFY(receiver.setFilterWidth(12'500).succeeded());
    QVERIFY(receiver.setSquelchLevel(-20.0).succeeded());
    QVERIFY(receiver.enableManualSquelch().succeeded());
    QVERIFY(receiver.startReception().succeeded());

    const auto decoder = waitForDecoderSamples(receiver);
    QVERIFY(!decoder.empty());
    QVERIFY(std::ranges::all_of(decoder, [](float sample) {
        return std::isfinite(sample) && std::abs(sample) <= 1.0F;
    }));
    QVERIFY(std::ranges::any_of(decoder, [](float sample) {
        return std::abs(sample) > 0.01F;
    }));
    QVERIFY(receiver.takeAudioSamples(4'096).empty());

    std::optional<sdr::radio::SpectrumFrame> spectrum;
    for (int attempt = 0; attempt < 40 && !spectrum; ++attempt) {
        QTest::qWait(10);
        spectrum = receiver.takeLatestSpectrumFrame();
    }
    QVERIFY(spectrum.has_value());
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::
    updatesFilterTapsWithoutRestartingRunningFlowgraph()
{
    struct SidebandCase {
        sdr::radio::DemodulationMode mode;
        HardwareTrace::ModulatedSignal signal;
    };
    constexpr SidebandCase cases[]{
        {sdr::radio::DemodulationMode::Usb,
         HardwareTrace::ModulatedSignal::UsbTone},
        {sdr::radio::DemodulationMode::Lsb,
         HardwareTrace::ModulatedSignal::LsbTone},
    };

    for (const auto& testCase : cases) {
        auto trace = std::make_shared<HardwareTrace>();
        trace->modulatedSignal = testCase.signal;
        GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
        QVERIFY(receiver.setDemodulationMode(testCase.mode).succeeded());
        QVERIFY(receiver.startReception().succeeded());
        QVERIFY(!waitForAudioSamples(receiver, true).empty());

        std::optional<sdr::radio::SpectrumFrame> initialFrame;
        for (int attempt = 0;
             attempt < 40 && !initialFrame.has_value();
             ++attempt) {
            QTest::qWait(10);
            initialFrame = receiver.takeLatestSpectrumFrame();
        }
        QVERIFY(initialFrame.has_value());
        const auto audioBefore = receiver.audioProducedSamples();
        const auto fftsBefore =
            receiver.spectrumProcessingMetrics().fftsExecuted;
        const auto generationBefore = receiver.tuningGeneration();
        QCOMPARE(trace->streamStarts, 1);
        QCOMPARE(trace->streamStops, 0);

        for (const std::uint64_t width :
             {2'500ULL, 2'700ULL, 3'000ULL, 3'500ULL}) {
            const auto result = receiver.setFilterWidth(width);
            QVERIFY2(result.succeeded(), result.message.c_str());
            QCOMPARE(receiver.state().filterWidth, width);
            QVERIFY(receiver.state().running);
            QCOMPARE(trace->streamStarts, 1);
            QCOMPARE(trace->streamStops, 0);
            QCOMPARE(receiver.tuningGeneration(), generationBefore);
        }

        for (int attempt = 0;
             attempt < 40 &&
             (receiver.audioProducedSamples() <= audioBefore ||
              receiver.spectrumProcessingMetrics().fftsExecuted <= fftsBefore);
             ++attempt) {
            QTest::qWait(10);
        }
        QVERIFY(receiver.audioProducedSamples() > audioBefore);
        QVERIFY(receiver.spectrumProcessingMetrics().fftsExecuted > fftsBefore);
        QVERIFY(!waitForAudioSamples(receiver, true).empty());
        QCOMPARE(trace->streamStarts, 1);
        QCOMPARE(trace->streamStops, 0);

        QVERIFY(receiver.stopReception().succeeded());
        QCOMPARE(trace->streamStarts, 1);
        QCOMPARE(trace->streamStops, 1);
    }
}

void GnuRadioReceiverBackendTest::reportsPpmUnsupportedWithoutHardware()
{
    GnuRadioReceiverBackend receiver;

    QVERIFY(!receiver.capabilities().ppmCorrectionSupported);
    const auto result = receiver.setPpmCorrection(5.0);
    QVERIFY(!result.succeeded());
    QVERIFY(result.error == sdr::radio::ReceiverError::PpmCorrectionUnsupported);
    QCOMPARE(receiver.state().ppmCorrection, 0.0);
}

void GnuRadioReceiverBackendTest::measuresPpmWithDriverCorrectionReset()
{
    auto trace = std::make_shared<HardwareTrace>();
    auto capabilities = streamingCapabilities();
    capabilities.rtlSdrTestModeSupported = true;
    GnuRadioReceiverBackend receiver(
        explicitlySelectedController(trace, capabilities));
    QVERIFY(receiver.setPpmCorrection(7.0).succeeded());

    QVERIFY(receiver.beginPpmCalibration().succeeded());
    QCOMPARE(receiver.state().ppmCorrection, 7.0);
    QCOMPARE(trace->ppmCorrections, std::vector<double>({7.0, 0.0}));
    QCOMPARE(trace->testStreamStarts, 1);

    QVERIFY(receiver.endPpmCalibration().succeeded());
    QCOMPARE(trace->testStreamStops, 1);
}

void GnuRadioReceiverBackendTest::demodulatesSyntheticSignals()
{
    struct ModeTuning {
        sdr::radio::DemodulationMode mode;
        std::uint64_t listeningFrequency;
    };
    constexpr ModeTuning cases[]{
        {sdr::radio::DemodulationMode::Am, 100'100'000},
        {sdr::radio::DemodulationMode::Nfm, 100'099'000},
        {sdr::radio::DemodulationMode::Wfm, 100'090'000},
        {sdr::radio::DemodulationMode::Usb, 100'099'000},
        {sdr::radio::DemodulationMode::Lsb, 100'101'000},
    };

    for (const auto& testCase : cases) {
        GnuRadioReceiverBackend receiver;
        const auto modeResult = receiver.setDemodulationMode(testCase.mode);
        QVERIFY2(modeResult.succeeded(), modeResult.message.c_str());
        QVERIFY(receiver.setListeningFrequency(
            testCase.listeningFrequency).succeeded());
        QVERIFY(receiver.startReception().succeeded());
        const bool unmodulatedAm =
            testCase.mode == sdr::radio::DemodulationMode::Am;
        const auto samples = waitForAudioSamples(receiver, !unmodulatedAm);
        QVERIFY(!samples.empty());
        QVERIFY(std::ranges::all_of(samples, [](float sample) {
            return std::isfinite(sample);
        }));
        // The synthetic AM input is an unmodulated carrier, so correct DC
        // removal may leave silence after its startup transient. The other
        // tuning cases deliberately place a tone in their audio passband.
        if (!unmodulatedAm) {
            QVERIFY(std::ranges::any_of(samples, [](float sample) {
                return std::abs(sample) > 0.01F;
            }));
        }
        QVERIFY(receiver.stopReception().succeeded());
    }
}

void GnuRadioReceiverBackendTest::demodulatesKnownHardwareTonesAtTheEffectiveRate()
{
    struct ModeCase {
        sdr::radio::DemodulationMode mode;
        HardwareTrace::ModulatedSignal signal;
        std::array<std::uint64_t, 3> filterWidths;
    };
    constexpr ModeCase cases[]{
        {sdr::radio::DemodulationMode::Am,
         HardwareTrace::ModulatedSignal::AmTone,
         {3'000, 12'500, 15'000}},
        {sdr::radio::DemodulationMode::Nfm,
         HardwareTrace::ModulatedSignal::NfmTone,
         {5'000, 12'500, 25'000}},
        {sdr::radio::DemodulationMode::Wfm,
         HardwareTrace::ModulatedSignal::WfmTone,
         {100'000, 180'000, 250'000}},
        {sdr::radio::DemodulationMode::Usb,
         HardwareTrace::ModulatedSignal::UsbTone,
         {1'800, 2'700, 4'000}},
        {sdr::radio::DemodulationMode::Lsb,
         HardwareTrace::ModulatedSignal::LsbTone,
         {1'800, 2'700, 4'000}},
    };
    constexpr std::array<std::uint64_t, 3> effectiveRates{
        1'000'000,
        2'000'000,
        2'400'000,
    };

    for (const auto& testCase : cases) {
        for (std::size_t rateIndex = 0; rateIndex < effectiveRates.size();
             ++rateIndex) {
            auto trace = std::make_shared<HardwareTrace>();
            trace->effectiveSampleRate = effectiveRates[rateIndex];
            trace->modulatedSignal = testCase.signal;
            GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));

            const auto modeResult = receiver.setDemodulationMode(testCase.mode);
            QVERIFY2(modeResult.succeeded(), modeResult.message.c_str());
            const auto filterResult = receiver.setFilterWidth(
                testCase.filterWidths[rateIndex]);
            QVERIFY2(filterResult.succeeded(), filterResult.message.c_str());
            QCOMPARE(
                receiver.state().filterWidth, testCase.filterWidths[rateIndex]);
            QVERIFY(receiver.startReception().succeeded());
            QCOMPARE(receiver.state().sampleRate, std::uint64_t{2'000'000});
            QCOMPARE(receiver.effectiveSampleRate(), effectiveRates[rateIndex]);

            std::optional<sdr::radio::SpectrumFrame> frame;
            for (int attempt = 0; attempt < 40 && !frame.has_value(); ++attempt) {
                QTest::qWait(10);
                frame = receiver.takeLatestSpectrumFrame();
            }
            QVERIFY(frame.has_value());
            QCOMPARE(frame->sampleRate, effectiveRates[rateIndex]);

            const auto samples = collectAudioSamples(receiver);
            const auto tone = measureAudioTone(samples);
            const auto settled = std::span<const float>(samples).subspan(
                std::min<std::size_t>(1'024, samples.size()));
            QVERIFY(std::ranges::all_of(settled, [](float sample) {
                return std::isfinite(sample) && std::abs(sample) < 0.98F;
            }));
            const std::string context =
                " at effective rate " +
                std::to_string(effectiveRates[rateIndex]) +
                " and filter width " +
                std::to_string(testCase.filterWidths[rateIndex]);
            const std::string frequencyMessage =
                "Expected a 1 kHz tone at 48 kHz; measured " +
                std::to_string(tone.frequencyHz) + " Hz" + context;
            QVERIFY2(tone.frequencyHz > 900.0 && tone.frequencyHz < 1'100.0,
                     frequencyMessage.c_str());
            const std::string levelMessage =
                "Expected a bounded audible tone; measured RMS " +
                std::to_string(tone.rms) + context;
            QVERIFY2(tone.rms > 0.04 && tone.rms < 0.85,
                     levelMessage.c_str());
            QVERIFY(receiver.stopReception().succeeded());
        }
    }
}

void GnuRadioReceiverBackendTest::keepsWeakAmAndSidebandSignalsAudibleAndBounded()
{
    struct ModeCase {
        sdr::radio::DemodulationMode mode;
        HardwareTrace::ModulatedSignal signal;
    };
    constexpr ModeCase cases[]{
        {sdr::radio::DemodulationMode::Am,
         HardwareTrace::ModulatedSignal::AmTone},
        {sdr::radio::DemodulationMode::Usb,
         HardwareTrace::ModulatedSignal::UsbTone},
        {sdr::radio::DemodulationMode::Lsb,
         HardwareTrace::ModulatedSignal::LsbTone},
    };

    for (const auto& testCase : cases) {
        auto trace = std::make_shared<HardwareTrace>();
        trace->effectiveSampleRate = 2'400'000;
        trace->modulatedSignal = testCase.signal;
        trace->signalAmplitude = 0.01F;
        GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
        QVERIFY(receiver.setDemodulationMode(testCase.mode).succeeded());
        QVERIFY(receiver.startReception().succeeded());

        const auto samples = collectAudioSamples(receiver);
        const auto tone = measureAudioTone(samples, 4'096);
        const auto settled = std::span<const float>(samples).subspan(
            std::min<std::size_t>(1'024, samples.size()));
        QVERIFY(std::ranges::all_of(settled, [](float sample) {
            return std::isfinite(sample) && std::abs(sample) < 0.98F;
        }));
        QVERIFY2(
            tone.frequencyHz > 900.0 && tone.frequencyHz < 1'100.0,
            "Weak controlled signal did not recover the expected 1 kHz tone");
        QVERIFY2(
            tone.rms > 0.04 && tone.rms < 0.5,
            "Weak controlled signal remained effectively inaudible or clipped");
        QVERIFY(receiver.stopReception().succeeded());
    }
}

void GnuRadioReceiverBackendTest::keepsAmAndSidebandReceiverNoiseAudible()
{
    constexpr std::array modes{
        sdr::radio::DemodulationMode::Am,
        sdr::radio::DemodulationMode::Usb,
        sdr::radio::DemodulationMode::Lsb,
    };

    for (const auto mode : modes) {
        auto trace = std::make_shared<HardwareTrace>();
        trace->modulatedSignal = HardwareTrace::ModulatedSignal::Noise;
        trace->signalAmplitude = 0.01F;
        GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
        QVERIFY(receiver.setDemodulationMode(mode).succeeded());
        QVERIFY(receiver.startReception().succeeded());

        const auto samples = collectAudioSamples(receiver);
        const auto noise = measureAudioTone(samples, 4'096);
        const std::string message =
            "Controlled receiver noise remained silent; RMS " +
            std::to_string(noise.rms);
        QVERIFY2(noise.rms > 0.002 && noise.rms < 0.5, message.c_str());
        QVERIFY(receiver.stopReception().succeeded());
    }
}

void GnuRadioReceiverBackendTest::rejectsTheOppositeSideband()
{
    struct ModeCase {
        sdr::radio::DemodulationMode mode;
        HardwareTrace::ModulatedSignal rejectedSignal;
    };
    constexpr ModeCase cases[]{
        {sdr::radio::DemodulationMode::Usb,
         HardwareTrace::ModulatedSignal::LsbTone},
        {sdr::radio::DemodulationMode::Lsb,
         HardwareTrace::ModulatedSignal::UsbTone},
    };

    for (const auto& testCase : cases) {
        auto trace = std::make_shared<HardwareTrace>();
        trace->effectiveSampleRate = 2'400'000;
        trace->modulatedSignal = testCase.rejectedSignal;
        GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
        QVERIFY(receiver.setDemodulationMode(testCase.mode).succeeded());
        QVERIFY(receiver.startReception().succeeded());

        const auto samples = collectAudioSamples(receiver);
        const auto tone = measureAudioTone(samples, 4'096);
        const std::string message =
            "The unselected sideband leaked into the recovered audio; RMS " +
            std::to_string(tone.rms);
        QVERIFY2(tone.rms < 0.02, message.c_str());
        QVERIFY(receiver.stopReception().succeeded());
    }
}

void GnuRadioReceiverBackendTest::appliesListeningOffsetToControlledSignals()
{
    struct ModeCase {
        sdr::radio::DemodulationMode mode;
        HardwareTrace::ModulatedSignal signal;
    };
    constexpr ModeCase cases[]{
        {sdr::radio::DemodulationMode::Am,
         HardwareTrace::ModulatedSignal::AmTone},
        {sdr::radio::DemodulationMode::Nfm,
         HardwareTrace::ModulatedSignal::NfmTone},
        {sdr::radio::DemodulationMode::Wfm,
         HardwareTrace::ModulatedSignal::WfmTone},
        {sdr::radio::DemodulationMode::Usb,
         HardwareTrace::ModulatedSignal::UsbTone},
        {sdr::radio::DemodulationMode::Lsb,
         HardwareTrace::ModulatedSignal::LsbTone},
    };

    for (const auto& testCase : cases) {
        auto trace = std::make_shared<HardwareTrace>();
        trace->signalOffsetHz = 100'000.0;
        trace->modulatedSignal = testCase.signal;
        GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
        QVERIFY(receiver.setDemodulationMode(testCase.mode).succeeded());
        QVERIFY(receiver.setListeningFrequency(100'100'000).succeeded());
        QCOMPARE(receiver.frequencyTranslationOffsetHz(), 100'000.0);
        QVERIFY(receiver.startReception().succeeded());

        const auto samples = collectAudioSamples(receiver);
        const auto tone = measureAudioTone(samples, 4'096);
        QVERIFY2(
            tone.frequencyHz > 900.0 && tone.frequencyHz < 1'100.0,
            "Listening-frequency translation did not recover the 1 kHz tone");
        QVERIFY2(
            tone.rms > 0.04 && tone.rms < 0.85,
            "Listening-frequency translation produced silent or clipped audio");
        QVERIFY(receiver.stopReception().succeeded());
    }
}

void GnuRadioReceiverBackendTest::switchesControlledModesWithoutStaleOrInvalidAudio()
{
    struct ModeCase {
        sdr::radio::DemodulationMode mode;
        HardwareTrace::ModulatedSignal signal;
        std::uint64_t defaultFilterWidth;
    };
    constexpr ModeCase cases[]{
        {sdr::radio::DemodulationMode::Am,
         HardwareTrace::ModulatedSignal::AmTone,
         10'000},
        {sdr::radio::DemodulationMode::Nfm,
         HardwareTrace::ModulatedSignal::NfmTone,
         12'500},
        {sdr::radio::DemodulationMode::Wfm,
         HardwareTrace::ModulatedSignal::WfmTone,
         180'000},
        {sdr::radio::DemodulationMode::Usb,
         HardwareTrace::ModulatedSignal::UsbTone,
         2'400},
        {sdr::radio::DemodulationMode::Lsb,
         HardwareTrace::ModulatedSignal::LsbTone,
         2'400},
    };

    auto trace = std::make_shared<HardwareTrace>();
    trace->modulatedSignal = cases[0].signal;
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));
    QVERIFY(receiver.startReception().succeeded());

    for (const auto& testCase : cases) {
        trace->modulatedSignal.store(
            testCase.signal, std::memory_order_relaxed);
        const auto modeResult = receiver.setDemodulationMode(testCase.mode);
        QVERIFY2(modeResult.succeeded(), modeResult.message.c_str());
        const auto filterResult = receiver.setFilterWidth(
            testCase.defaultFilterWidth);
        QVERIFY2(filterResult.succeeded(), filterResult.message.c_str());
        QVERIFY(receiver.state().running);
        QCOMPARE(receiver.state().demodulationMode, testCase.mode);
        QCOMPARE(receiver.state().filterWidth, testCase.defaultFilterWidth);

        const auto samples = collectAudioSamples(receiver);
        const auto tone = measureAudioTone(samples, 4'096);
        const auto settled = std::span<const float>(samples).subspan(
            std::min<std::size_t>(4'096, samples.size()));
        QVERIFY(std::ranges::all_of(settled, [](float sample) {
            return std::isfinite(sample) && std::abs(sample) < 0.98F;
        }));
        QVERIFY2(
            tone.frequencyHz > 900.0 && tone.frequencyHz < 1'100.0,
            "Live mode switch retained stale audio or lost the 1 kHz tone");
        QVERIFY2(
            tone.rms > 0.04 && tone.rms < 0.85,
            "Live mode switch produced silent or clipped audio");
    }

    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::requiresAnExplicitlySelectedHardwareDevice()
{
    auto trace = std::make_shared<HardwareTrace>();
    DeviceDescriptor descriptor{
        "mock:serial=selected",
        true,
        "Explicit mock SDR",
        "mock",
        "mock-hardware",
        "selected",
        streamingCapabilities(),
    };
    auto controller = std::make_unique<DeviceController>(
        std::make_unique<StreamingMockProvider>(descriptor, trace));
    QVERIFY(controller->discover().succeeded());
    QVERIFY(trace->openedIdentifiers.empty());
    QVERIFY_EXCEPTION_THROWN(
        GnuRadioReceiverBackend(std::move(controller)), std::invalid_argument);
    QVERIFY(trace->openedIdentifiers.empty());
}

void GnuRadioReceiverBackendTest::configuresAndStreamsExplicitlySelectedHardware()
{
    auto trace = std::make_shared<HardwareTrace>();
    auto controller = explicitlySelectedController(trace);
    QVERIFY(controller != nullptr);
    QCOMPARE(trace->openedIdentifiers.size(), std::size_t{1});

    GnuRadioReceiverBackend receiver(std::move(controller));
    QVERIFY(receiver.usesHardwareSource());
    QVERIFY(receiver.capabilities().ppmCorrectionSupported);
    QVERIFY(receiver.setGain(12.0).succeeded());
    QVERIFY(receiver.setPpmCorrection(-8.0).succeeded());
    QVERIFY(receiver.setListeningFrequency(100'099'000).succeeded());
    QCOMPARE(receiver.frequencyTranslationOffsetHz(), 99'000.0);
    QVERIFY(receiver.startReception().succeeded());
    QVERIFY(!waitForAudioSamples(receiver).empty());
    QVERIFY(receiver.stopReception().succeeded());

    QVERIFY(std::ranges::find(trace->sampleRates, std::uint64_t{2'000'000}) !=
            trace->sampleRates.end());
    QVERIFY(std::ranges::find(trace->gains, 12.0) != trace->gains.end());
    QVERIFY(std::ranges::find(trace->ppmCorrections, -8.0) !=
            trace->ppmCorrections.end());
    QVERIFY(std::ranges::find(
                trace->tuningRequests,
                std::pair<std::uint64_t, HfTuningMode>{
                    100'000'000, HfTuningMode::Normal}) !=
            trace->tuningRequests.end());
    QCOMPARE(trace->streamStarts, 1);
    QVERIFY(trace->streamStops >= 1);
}

void GnuRadioReceiverBackendTest::continuesProducingHardwareSpectrumFrames()
{
    auto trace = std::make_shared<HardwareTrace>();
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));

    QVERIFY(receiver.startReception().succeeded());
    std::optional<sdr::radio::SpectrumFrame> firstFrame;
    std::optional<sdr::radio::SpectrumFrame> laterFrame;
    for (int attempt = 0; attempt < 100 && !laterFrame.has_value(); ++attempt) {
        QTest::qWait(10);
        if (auto frame = receiver.takeLatestSpectrumFrame()) {
            if (!firstFrame.has_value()) {
                firstFrame = std::move(frame);
            } else if (frame->sequence > firstFrame->sequence) {
                laterFrame = std::move(frame);
            }
        }
    }

    QVERIFY(firstFrame.has_value());
    QVERIFY(laterFrame.has_value());
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::continuesAfterTransientHardwareReadTimeout()
{
    auto trace = std::make_shared<HardwareTrace>();
    trace->transientReadTimeouts = 1;
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));

    QVERIFY(receiver.startReception().succeeded());
    std::optional<sdr::radio::SpectrumFrame> firstFrame;
    std::optional<sdr::radio::SpectrumFrame> laterFrame;
    for (int attempt = 0; attempt < 100 && !laterFrame.has_value(); ++attempt) {
        QTest::qWait(10);
        if (auto frame = receiver.takeLatestSpectrumFrame()) {
            if (!firstFrame.has_value()) {
                firstFrame = std::move(frame);
            } else if (frame->sequence > firstFrame->sequence) {
                laterFrame = std::move(frame);
            }
        }
    }

    QVERIFY(firstFrame.has_value());
    QVERIFY(laterFrame.has_value());
    QVERIFY(trace->reads > 1);
    QVERIFY(receiver.stopReception().succeeded());
}

void GnuRadioReceiverBackendTest::stopsWhileHardwareReadsTimeOut()
{
    auto trace = std::make_shared<HardwareTrace>();
    trace->timeoutEveryRead = true;
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));

    QVERIFY(receiver.startReception().succeeded());
    QTest::qWait(20);
    QElapsedTimer stopTimer;
    stopTimer.start();
    QVERIFY(receiver.stopReception().succeeded());
    QVERIFY2(
        stopTimer.elapsed() < 500,
        "Stopping must interrupt a hardware source that is retrying timeouts");
}

void GnuRadioReceiverBackendTest::appliesV4HfPolicyThroughTheHardwarePath()
{
    auto trace = std::make_shared<HardwareTrace>();
    GnuRadioReceiverBackend receiver(explicitlySelectedController(
        trace, streamingCapabilities(true)));

    QVERIFY(receiver.setCenterFrequency(500'000).succeeded());
    QCOMPARE(trace->tuningRequests.back().first, std::uint64_t{500'000});
    QVERIFY(!receiver.setCenterFrequency(499'999).succeeded());
    QVERIFY(receiver.setCenterFrequency(7'100'000).succeeded());
    QCOMPARE(trace->tuningRequests.back().first, std::uint64_t{7'100'000});
    QVERIFY(receiver.setCenterFrequency(26'000'000).succeeded());
    QCOMPARE(
        trace->tuningRequests.back().second,
        HfTuningMode::DriverManagedRtlSdrBlogV4);
    QVERIFY(receiver.setCenterFrequency(30'000'000).succeeded());
    QCOMPARE(trace->tuningRequests.back().second, HfTuningMode::Normal);
}

void GnuRadioReceiverBackendTest::rejectsUnsupportedHardwareConfiguration()
{
    auto trace = std::make_shared<HardwareTrace>();
    auto capabilities = streamingCapabilities();
    capabilities.receiveFrequencyRanges = {{50'000'000, 500'000'000}};
    capabilities.receiveSampleRateRanges = {{1'000'000, 2'000'000}};
    capabilities.ppmCorrectionSupported = false;
    GnuRadioReceiverBackend receiver(
        explicitlySelectedController(trace, capabilities));

    const auto frequency = receiver.setCenterFrequency(900'000'000);
    QVERIFY(!frequency.succeeded());
    QVERIFY(frequency.error == sdr::radio::ReceiverError::CenterFrequencyOutOfRange);
    QCOMPARE(receiver.state().centerFrequency, std::uint64_t{100'000'000});

    const auto sampleRate = receiver.setSampleRate(3'000'000);
    QVERIFY(!sampleRate.succeeded());
    QVERIFY(sampleRate.error == sdr::radio::ReceiverError::SampleRateOutOfRange);
    QCOMPARE(receiver.state().sampleRate, std::uint64_t{2'000'000});

    const auto ppm = receiver.setPpmCorrection(5.0);
    QVERIFY(!ppm.succeeded());
    QVERIFY(ppm.error == sdr::radio::ReceiverError::PpmCorrectionUnsupported);

    auto v4Trace = std::make_shared<HardwareTrace>();
    auto unsupportedV4 = streamingCapabilities(true);
    unsupportedV4.driverManagedHfBelow27Mhz = false;
    unsupportedV4.hfLimitation = "Mock V4 driver exposes no HF control";
    GnuRadioReceiverBackend v4Receiver(
        explicitlySelectedController(v4Trace, unsupportedV4));
    const auto hf = v4Receiver.setCenterFrequency(20'000'000);
    QVERIFY(!hf.succeeded());
    QVERIFY(hf.message.find("no HF control") != std::string::npos);
    QCOMPARE(v4Receiver.state().centerFrequency, std::uint64_t{100'000'000});
}

void GnuRadioReceiverBackendTest::reportsHardwareStreamStartAndDisconnectFailures()
{
    auto startTrace = std::make_shared<HardwareTrace>();
    startTrace->failStreamStart = true;
    GnuRadioReceiverBackend startFailure(
        explicitlySelectedController(startTrace));
    const auto start = startFailure.startReception();
    QVERIFY(!start.succeeded());
    QVERIFY(!startFailure.state().running);

    auto disconnectTrace = std::make_shared<HardwareTrace>();
    disconnectTrace->disconnectOnRead = true;
    GnuRadioReceiverBackend disconnected(
        explicitlySelectedController(disconnectTrace));
    QVERIFY(disconnected.startReception().succeeded());

    std::optional<sdr::radio::OperationResult> runtimeError;
    for (int attempt = 0; attempt < 30 && !runtimeError.has_value(); ++attempt) {
        QTest::qWait(10);
        runtimeError = disconnected.takeRuntimeError();
    }
    QVERIFY(runtimeError.has_value());
    QVERIFY(!runtimeError->succeeded());
    QVERIFY(runtimeError->message.find("disconnected") != std::string::npos);
    QVERIFY(!disconnected.state().running);
}

void GnuRadioReceiverBackendTest::synchronizesStoppedStateAfterHardwareCleanupFailure()
{
    auto trace = std::make_shared<HardwareTrace>();
    trace->failStreamStop = true;
    GnuRadioReceiverBackend receiver(explicitlySelectedController(trace));

    QVERIFY(receiver.startReception().succeeded());
    const auto stopResult = receiver.stopReception();
    QVERIFY(!stopResult.succeeded());
    QVERIFY(stopResult.stateChanged);
    QVERIFY(!receiver.state().running);
    QCOMPARE(trace->streamStops, 1);
}

QTEST_GUILESS_MAIN(GnuRadioReceiverBackendTest)

#include "GnuRadioReceiverBackendTest.moc"
