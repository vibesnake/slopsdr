// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioSampleBuffer.hpp"
#include "FrequencyMapping.hpp"
#include "MockReceiverBackend.hpp"
#include "ReceiverStateModel.hpp"

#include <QtTest>

#include <array>
#include <cmath>
#include <complex>

using sdr::radio::DemodulationMode;
using sdr::radio::MockReceiverBackend;
using sdr::radio::ReceiverError;
using sdr::radio::SquelchMode;

class ReceiverDomainTest final : public QObject
{
    Q_OBJECT

private slots:
    void startsAndStops();
    void repeatedStartAndStopAreIdempotent();
    void changesCenterFrequencyAndRecentersListening();
    void changesListeningFrequencyWithinPassband();
    void validatesPassbandAndSpectrumPosition();
    void mapsPixelsAndFrequenciesAtPassbandEdges();
    void shiftsCenterAndRecentersListening();
    void appliesSampleRateRules();
    void validatesPpmCorrection();
    void rejectsUnsupportedPpmCorrection();
    void changesEverySupportedMode();
    void appliesModeSpecificFilterRules();
    void setsManualSquelch();
    void estimatesOneShotSquelchWithMarginAndSpikeRejection();
    void clampsOneShotSquelchAndRejectsUnavailableSamples();
    void returnsToSavedManualSquelch();
    void disablesSquelch();
    void validatesFilterWidth();
    void validatesGain();
    void capsMockWaterfallCadenceForLongFftWindows();
    void keepsLifecycleStateOnBackendFailure();
    void preservesAcceptedIqSamplesWhenCaptureStops();
};

void ReceiverDomainTest::startsAndStops()
{
    MockReceiverBackend receiver;

    const auto startResult = receiver.startReception();
    QVERIFY(startResult.succeeded());
    QVERIFY(startResult.stateChanged);
    QVERIFY(receiver.state().running);

    const auto stopResult = receiver.stopReception();
    QVERIFY(stopResult.succeeded());
    QVERIFY(stopResult.stateChanged);
    QVERIFY(!receiver.state().running);
}

void ReceiverDomainTest::repeatedStartAndStopAreIdempotent()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.startReception().succeeded());
    const auto repeatedStart = receiver.startReception();
    QVERIFY(repeatedStart.succeeded());
    QVERIFY(!repeatedStart.stateChanged);
    QVERIFY(receiver.state().running);

    QVERIFY(receiver.stopReception().succeeded());
    const auto repeatedStop = receiver.stopReception();
    QVERIFY(repeatedStop.succeeded());
    QVERIFY(!repeatedStop.stateChanged);
    QVERIFY(!receiver.state().running);
}

void ReceiverDomainTest::preservesAcceptedIqSamplesWhenCaptureStops()
{
    sdr::radio::ComplexSampleBuffer buffer(2);
    buffer.setEnabled(true);
    buffer.push(std::array<std::complex<float>, 1>{std::complex<float>{0.5F, -0.25F}});

    buffer.setEnabled(false);
    buffer.push(std::array<std::complex<float>, 1>{std::complex<float>{1.0F, 1.0F}});

    const auto samples = buffer.take(2);
    QCOMPARE(samples.size(), std::size_t{1});
    QVERIFY((samples.front() == std::complex<float>{0.5F, -0.25F}));
}

void ReceiverDomainTest::changesCenterFrequencyAndRecentersListening()
{
    MockReceiverBackend receiver;

    const auto result = receiver.setCenterFrequency(200'000'000);
    QVERIFY(result.succeeded());
    QVERIFY(!result.adjusted);
    QCOMPARE(receiver.state().centerFrequency, std::uint64_t{200'000'000});
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{200'000'000});

    const auto previousState = receiver.state();
    const auto invalidResult = receiver.setCenterFrequency(0);
    QVERIFY(!invalidResult.succeeded());
    QVERIFY(invalidResult.error == ReceiverError::CenterFrequencyOutOfRange);
    QCOMPARE(receiver.state().centerFrequency, previousState.centerFrequency);
    QCOMPARE(receiver.state().listeningFrequency, previousState.listeningFrequency);
}

void ReceiverDomainTest::changesListeningFrequencyWithinPassband()
{
    MockReceiverBackend receiver;

    const auto result = receiver.setListeningFrequency(100'750'000);
    QVERIFY(result.succeeded());
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{100'750'000});

    const auto positionResult = receiver.tuneListeningFrequency(0.25);
    QVERIFY(positionResult.succeeded());
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{99'500'000});
}

void ReceiverDomainTest::validatesPassbandAndSpectrumPosition()
{
    MockReceiverBackend receiver;

    const auto outsidePassband = receiver.setListeningFrequency(101'000'001);
    QVERIFY(!outsidePassband.succeeded());
    QVERIFY(
        outsidePassband.error == ReceiverError::ListeningFrequencyOutsidePassband);
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{100'000'000});

    const auto invalidPosition = receiver.tuneListeningFrequency(1.1);
    QVERIFY(!invalidPosition.succeeded());
    QVERIFY(invalidPosition.error == ReceiverError::SpectrumPositionOutOfRange);
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{100'000'000});
}

void ReceiverDomainTest::mapsPixelsAndFrequenciesAtPassbandEdges()
{
    const sdr::radio::FrequencyRange range{99'000'000, 101'000'000};

    QCOMPARE(
        sdr::radio::frequencyForPixel(0.0, 800.0, range),
        std::optional<std::uint64_t>{99'000'000});
    QCOMPARE(
        sdr::radio::frequencyForPixel(400.0, 800.0, range),
        std::optional<std::uint64_t>{100'000'000});
    QCOMPARE(
        sdr::radio::frequencyForPixel(800.0, 800.0, range),
        std::optional<std::uint64_t>{101'000'000});
    QVERIFY(!sdr::radio::frequencyForPixel(-1.0, 800.0, range).has_value());
    QVERIFY(!sdr::radio::frequencyForPixel(0.0, 0.0, range).has_value());

    QCOMPARE(
        sdr::radio::pixelForFrequency(99'000'000, 800.0, range),
        std::optional<double>{0.0});
    QCOMPARE(
        sdr::radio::pixelForFrequency(100'000'000, 800.0, range),
        std::optional<double>{400.0});
    QCOMPARE(
        sdr::radio::pixelForFrequency(101'000'000, 800.0, range),
        std::optional<double>{800.0});
    QVERIFY(!sdr::radio::pixelForFrequency(101'000'001, 800.0, range).has_value());
}

void ReceiverDomainTest::shiftsCenterAndRecentersListening()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.tuneListeningFrequency(0.75).succeeded());
    const auto result = receiver.shiftCenterFrequency(10'000);
    QVERIFY(result.succeeded());
    QVERIFY(!result.adjusted);
    QCOMPARE(receiver.state().centerFrequency, std::uint64_t{100'010'000});
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{100'010'000});

    QVERIFY(receiver.setCenterFrequency(1'000'000).succeeded());
    const auto limitedResult = receiver.shiftCenterFrequency(-10'000);
    QVERIFY(limitedResult.succeeded());
    QVERIFY(limitedResult.adjusted);
    QVERIFY(!limitedResult.stateChanged);
    QCOMPARE(receiver.state().centerFrequency, std::uint64_t{1'000'000});
}

void ReceiverDomainTest::appliesSampleRateRules()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.setListeningFrequency(100'900'000).succeeded());
    const auto narrowerRate = receiver.setSampleRate(1'000'000);
    QVERIFY(narrowerRate.succeeded());
    QVERIFY(narrowerRate.adjusted);
    QCOMPARE(receiver.state().sampleRate, std::uint64_t{1'000'000});
    QCOMPARE(receiver.state().listeningFrequency, std::uint64_t{100'500'000});

    QVERIFY(receiver.setDemodulationMode(DemodulationMode::Wfm).succeeded());
    QVERIFY(receiver.setFilterWidth(250'000).succeeded());
    const auto rejectedRate = receiver.setSampleRate(200'000);
    QVERIFY(!rejectedRate.succeeded());
    QVERIFY(rejectedRate.error == ReceiverError::FilterWidthOutOfRange);
    QCOMPARE(receiver.state().sampleRate, std::uint64_t{1'000'000});
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{250'000});
}

void ReceiverDomainTest::rejectsUnsupportedPpmCorrection()
{
    MockReceiverBackend receiver({.ppmCorrectionSupported = false});

    const auto result = receiver.setPpmCorrection(12.0);
    QVERIFY(!result.succeeded());
    QVERIFY(result.error == ReceiverError::PpmCorrectionUnsupported);
    QCOMPARE(receiver.state().ppmCorrection, 0.0);
}

void ReceiverDomainTest::validatesPpmCorrection()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.setPpmCorrection(-25.5).succeeded());
    QCOMPARE(receiver.state().ppmCorrection, -25.5);

    const auto invalidResult = receiver.setPpmCorrection(200.1);
    QVERIFY(!invalidResult.succeeded());
    QVERIFY(invalidResult.error == ReceiverError::PpmCorrectionOutOfRange);
    QCOMPARE(receiver.state().ppmCorrection, -25.5);
}

void ReceiverDomainTest::changesEverySupportedMode()
{
    MockReceiverBackend receiver;
    constexpr std::array modes{
        DemodulationMode::Am,
        DemodulationMode::Nfm,
        DemodulationMode::Wfm,
        DemodulationMode::Usb,
        DemodulationMode::Lsb,
        DemodulationMode::DigitalDecoderOutput,
    };

    for (const auto mode : modes) {
        const auto result = receiver.setDemodulationMode(mode);
        QVERIFY(result.succeeded());
        QVERIFY(receiver.state().demodulationMode == mode);
    }

    const auto invalidResult =
        receiver.setDemodulationMode(static_cast<DemodulationMode>(999));
    QVERIFY(!invalidResult.succeeded());
    QVERIFY(invalidResult.error == ReceiverError::UnsupportedMode);
    QVERIFY(
        receiver.state().demodulationMode == DemodulationMode::DigitalDecoderOutput);
}

void ReceiverDomainTest::appliesModeSpecificFilterRules()
{
    MockReceiverBackend receiver;

    const auto wfm = receiver.setDemodulationMode(DemodulationMode::Wfm);
    QVERIFY(wfm.succeeded());
    QVERIFY(wfm.adjusted);
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{180'000});
    QVERIFY(receiver.setFilterWidth(250'000).succeeded());
    QVERIFY(!receiver.setFilterWidth(99'999).succeeded());

    const auto usb = receiver.setDemodulationMode(DemodulationMode::Usb);
    QVERIFY(usb.succeeded());
    QVERIFY(usb.adjusted);
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{2'400});
    QVERIFY(receiver.setFilterWidth(1'800).succeeded());
    QVERIFY(!receiver.setFilterWidth(4'001).succeeded());

    const auto lsb = receiver.setDemodulationMode(DemodulationMode::Lsb);
    QVERIFY(lsb.succeeded());
    QVERIFY(receiver.state().demodulationMode == DemodulationMode::Lsb);

    const auto digital = receiver.setDemodulationMode(
        DemodulationMode::DigitalDecoderOutput);
    QVERIFY(digital.succeeded());
    QVERIFY(digital.adjusted);
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{12'500});
}

void ReceiverDomainTest::setsManualSquelch()
{
    MockReceiverBackend receiver;

    const auto result = receiver.setSquelchLevel(-65.0);
    QVERIFY(result.succeeded());
    QCOMPARE(receiver.state().squelchLevelDb, -65.0);
    QVERIFY(receiver.state().squelchMode == SquelchMode::Manual);

    const auto invalidResult = receiver.setSquelchLevel(-161.0);
    QVERIFY(!invalidResult.succeeded());
    QVERIFY(invalidResult.error == ReceiverError::SquelchLevelOutOfRange);
    QCOMPARE(receiver.state().squelchLevelDb, -65.0);
    QVERIFY(receiver.state().squelchMode == SquelchMode::Manual);
}

void ReceiverDomainTest::estimatesOneShotSquelchWithMarginAndSpikeRejection()
{
    constexpr double samples[]{-101.0, -100.0, -99.0, -98.0, -20.0};
    const auto threshold = sdr::radio::estimateOneShotSquelchThreshold(
        samples, sdr::radio::ReceiverLimits{});
    QVERIFY(threshold.has_value());
    QCOMPARE(*threshold, -97.0);

    MockReceiverBackend receiver;
    QVERIFY(receiver.setSquelchLevel(*threshold).succeeded());
    constexpr double laterSignal[]{-40.0, -39.0, -38.0};
    const auto laterThreshold = sdr::radio::estimateOneShotSquelchThreshold(
        laterSignal, receiver.limits());
    QVERIFY(laterThreshold.has_value());
    QCOMPARE(receiver.state().squelchLevelDb, -97.0);
}

void ReceiverDomainTest::clampsOneShotSquelchAndRejectsUnavailableSamples()
{
    sdr::radio::ReceiverLimits limits;
    limits.minimumSquelchDb = -100.0;
    limits.maximumSquelchDb = -10.0;
    constexpr double high[]{-2.0, -1.0, 0.0};
    constexpr double low[]{-150.0, -140.0, -130.0};
    constexpr double invalid[]{NAN, INFINITY, -INFINITY};
    QCOMPARE(
        *sdr::radio::estimateOneShotSquelchThreshold(high, limits), -10.0);
    QCOMPARE(
        *sdr::radio::estimateOneShotSquelchThreshold(low, limits), -100.0);
    QVERIFY(!sdr::radio::estimateOneShotSquelchThreshold(invalid, limits));
}

void ReceiverDomainTest::returnsToSavedManualSquelch()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.setSquelchLevel(-67.0).succeeded());

    QVERIFY(receiver.disableSquelch().succeeded());
    QVERIFY(receiver.enableManualSquelch().succeeded());
    QCOMPARE(receiver.state().squelchLevelDb, -67.0);
}

void ReceiverDomainTest::disablesSquelch()
{
    MockReceiverBackend receiver;

    const auto result = receiver.disableSquelch();
    QVERIFY(result.succeeded());
    QVERIFY(result.stateChanged);
    QVERIFY(receiver.state().squelchMode == SquelchMode::Disabled);

    const auto repeatedResult = receiver.disableSquelch();
    QVERIFY(repeatedResult.succeeded());
    QVERIFY(!repeatedResult.stateChanged);
}

void ReceiverDomainTest::validatesFilterWidth()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.setFilterWidth(10'000).succeeded());
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{10'000});

    const auto tooWide = receiver.setFilterWidth(15'001);
    QVERIFY(!tooWide.succeeded());
    QVERIFY(tooWide.error == ReceiverError::FilterWidthOutOfRange);
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{10'000});

    const auto zeroWidth = receiver.setFilterWidth(0);
    QVERIFY(!zeroWidth.succeeded());
    QVERIFY(zeroWidth.error == ReceiverError::FilterWidthOutOfRange);
    QCOMPARE(receiver.state().filterWidth, std::uint64_t{10'000});
}

void ReceiverDomainTest::keepsLifecycleStateOnBackendFailure()
{
    MockReceiverBackend startFailure({.startSucceeds = false});
    const auto start = startFailure.startReception();
    QVERIFY(!start.succeeded());
    QVERIFY(!startFailure.state().running);

    MockReceiverBackend stopFailure({.stopSucceeds = false});
    QVERIFY(stopFailure.startReception().succeeded());
    const auto stop = stopFailure.stopReception();
    QVERIFY(!stop.succeeded());
    QVERIFY(stopFailure.state().running);
}

void ReceiverDomainTest::validatesGain()
{
    MockReceiverBackend receiver;

    QVERIFY(receiver.setGain(35.0).succeeded());
    QCOMPARE(receiver.state().gainDb, 35.0);

    const auto invalidResult = receiver.setGain(100.1);
    QVERIFY(!invalidResult.succeeded());
    QVERIFY(invalidResult.error == ReceiverError::GainOutOfRange);
    QCOMPARE(receiver.state().gainDb, 35.0);
}

void ReceiverDomainTest::capsMockWaterfallCadenceForLongFftWindows()
{
    MockReceiverBackend receiver;
    QVERIFY(receiver.setSampleRate(250'000).succeeded());
    QVERIFY(receiver.setSpectrumFftSize(262'144).succeeded());
    QVERIFY(receiver.setSpectrumFramesPerSecond(120).succeeded());

    const auto metrics = receiver.spectrumProcessingMetrics();
    QCOMPARE(metrics.fftSize, std::size_t{262'144});
    QCOMPARE(metrics.hopSize, 262'144.0);
    QCOMPARE(metrics.overlapPercentage, 0.0);
    QVERIFY(std::abs(
                metrics.achievableFramesPerSecond -
                250'000.0 / 262'144.0) < 0.001);
}

QTEST_GUILESS_MAIN(ReceiverDomainTest)

#include "ReceiverDomainTest.moc"
