// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FftFrameProcessor.hpp"
#include "FrequencyMapping.hpp"
#include "SpectrumFramePacing.hpp"
#include "SpectrumWindow.hpp"
#include "SpectrumFrame.hpp"

#include <QtTest>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

using sdr::dsp::FftFrameProcessor;
using sdr::radio::SpectrumFrame;
using sdr::radio::SpectrumFrameQueue;

class SpectrumDataPathTest final : public QObject
{
    Q_OBJECT

private slots:
    void schedulesRowsAtConfiguredRateAcrossCaptureBandwidths();
    void preservesFractionalTimingAcrossFftSizes();
    void publishesEveryVectorWhenTheInputCannotMeetTargetRate();
    void resetsTimingWithoutBuildingBacklog();
    void schedulerBatchSizeDoesNotChangeAverageRate();
    void supportsConfiguredFftSizes();
    void adaptsInternalCadenceToVisibleDuration();
    void calculatesWindowHopOverlapSkippingAndFractionalRemainder();
    void keepsWindowHopRowRateStableAcrossCaptureBandwidths();
    void calculatesHertzPerBinAcrossConfiguredSizes();
    void mapsTheCompleteEffectiveSampleRateSpan();
    void mapsPlotOffsetsAndFractionalPositionsWithSharedAxis();
    void distinguishesCompatibilityFromExactTuningState();
    void countsPublishedFftFrames();
    void normalizesFftMagnitudeAndWindowGain();
    void boundsQueueAndDropsStaleFrames();
};

void SpectrumDataPathTest::schedulesRowsAtConfiguredRateAcrossCaptureBandwidths()
{
    constexpr std::uint32_t targetRowsPerSecond = 25;
    constexpr std::size_t fftSize = 2'048;
    constexpr double testSeconds = 30.0;
    for (const std::uint64_t sampleRate : {
             250'000ULL, 1'000'000ULL, 2'000'000ULL, 2'250'000ULL, 2'400'000ULL}) {
        sdr::dsp::SpectrumFrameScheduler scheduler(
            sampleRate, fftSize, targetRowsPerSecond);
        const auto vectors = static_cast<std::uint64_t>(
            std::floor(testSeconds * static_cast<double>(sampleRate) /
                       static_cast<double>(fftSize)));
        std::uint64_t published = 0;
        for (std::uint64_t vector = 0; vector < vectors; ++vector) {
            published += scheduler.acceptsNextVector() ? 1U : 0U;
        }

        const double elapsedSeconds = static_cast<double>(vectors * fftSize) /
                                      static_cast<double>(sampleRate);
        const double actualRowsPerSecond =
            static_cast<double>(published) / elapsedSeconds;
        QVERIFY2(
            std::abs(actualRowsPerSecond - targetRowsPerSecond) < 0.10,
            qPrintable(QStringLiteral("sample rate %1 produced %2 rows/s")
                           .arg(sampleRate)
                           .arg(actualRowsPerSecond, 0, 'f', 3)));
    }
}

void SpectrumDataPathTest::preservesFractionalTimingAcrossFftSizes()
{
    constexpr std::uint64_t fractionalEffectiveRate = 2'250'123;
    constexpr std::uint32_t targetRowsPerSecond = 25;
    constexpr double testSeconds = 30.0;
    for (const std::size_t fftSize : {1'024U, 2'048U, 4'096U}) {
        sdr::dsp::SpectrumFrameScheduler scheduler(
            fractionalEffectiveRate, fftSize, targetRowsPerSecond);
        const auto vectors = static_cast<std::uint64_t>(
            std::floor(testSeconds * static_cast<double>(fractionalEffectiveRate) /
                       static_cast<double>(fftSize)));
        std::uint64_t published = 0;
        for (std::uint64_t vector = 0; vector < vectors; ++vector) {
            published += scheduler.acceptsNextVector() ? 1U : 0U;
        }
        const double elapsedSeconds = static_cast<double>(vectors * fftSize) /
                                      static_cast<double>(fractionalEffectiveRate);
        QVERIFY(std::abs(static_cast<double>(published) / elapsedSeconds -
                         targetRowsPerSecond) < 0.10);
    }
}

void SpectrumDataPathTest::publishesEveryVectorWhenTheInputCannotMeetTargetRate()
{
    sdr::dsp::SpectrumFrameScheduler scheduler(20'000, 2'048, 25);
    QVERIFY(scheduler.publishesEveryVector());
    QCOMPARE(scheduler.availableVectorsPerSecond(), 20'000.0 / 2'048.0);

    for (int vector = 0; vector < 100; ++vector) {
        QVERIFY(scheduler.acceptsNextVector());
    }
}

void SpectrumDataPathTest::resetsTimingWithoutBuildingBacklog()
{
    sdr::dsp::SpectrumFrameScheduler scheduler(2'000'000, 2'048, 25);
    for (int vector = 0; vector < 39; ++vector) {
        QVERIFY(!scheduler.acceptsNextVector());
    }
    QVERIFY(scheduler.acceptsNextVector());

    scheduler.reset();
    for (int vector = 0; vector < 39; ++vector) {
        QVERIFY(!scheduler.acceptsNextVector());
    }
    QVERIFY(scheduler.acceptsNextVector());

    // A pause supplies no vectors, so it cannot create a burst of stale rows.
    for (int vector = 0; vector < 38; ++vector) {
        QVERIFY(!scheduler.acceptsNextVector());
    }
    QVERIFY(scheduler.acceptsNextVector());

    // A flowgraph rebuild for a new FFT size starts its own fresh phase.
    sdr::dsp::SpectrumFrameScheduler afterFftSizeChange(2'000'000, 4'096, 25);
    for (int vector = 0; vector < 19; ++vector) {
        QVERIFY(!afterFftSizeChange.acceptsNextVector());
    }
    QVERIFY(afterFftSizeChange.acceptsNextVector());
}

void SpectrumDataPathTest::schedulerBatchSizeDoesNotChangeAverageRate()
{
    constexpr std::uint64_t vectorCount = 20'000;
    sdr::dsp::SpectrumFrameScheduler singleItem(2'250'123, 2'048, 25);
    sdr::dsp::SpectrumFrameScheduler burstItems(2'250'123, 2'048, 25);
    std::uint64_t singlePublished = 0;
    std::uint64_t burstPublished = 0;
    for (std::uint64_t vector = 0; vector < vectorCount; ++vector) {
        singlePublished += singleItem.acceptsNextVector() ? 1U : 0U;
    }
    for (std::uint64_t first = 0; first < vectorCount; first += 64) {
        const auto count = std::min<std::uint64_t>(64, vectorCount - first);
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            burstPublished += burstItems.acceptsNextVector() ? 1U : 0U;
        }
    }
    QCOMPARE(burstPublished, singlePublished);
}

void SpectrumDataPathTest::supportsConfiguredFftSizes()
{
    QCOMPARE(sdr::dsp::defaultSpectrumFftSize, std::size_t{4'096});
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U,
             16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
        QVERIFY(sdr::dsp::isSupportedSpectrumFftSize(fftSize));
    }
    QCOMPARE(sdr::dsp::defaultSpectrumDisplayFramesPerSecond, 60U);
    for (const std::uint32_t rowsPerSecond : {15U, 30U, 60U, 120U, 240U}) {
        QVERIFY(sdr::dsp::isSupportedSpectrumFrameRate(rowsPerSecond));
    }
    QVERIFY(!sdr::dsp::isSupportedSpectrumFrameRate(0));
    QVERIFY(!sdr::dsp::isSupportedSpectrumFrameRate(241));
    QVERIFY(!sdr::dsp::isSupportedSpectrumFftSize(256));
}

void SpectrumDataPathTest::adaptsInternalCadenceToVisibleDuration()
{
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(5.0), 120U);
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(10.0), 60U);
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(15.0), 40U);
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(30.0), 20U);
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(60.0), 10U);
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(300.0), 2U);
    QCOMPARE(sdr::dsp::adaptiveSpectrumFrameRate(0.0), 60U);
}

void SpectrumDataPathTest::calculatesWindowHopOverlapSkippingAndFractionalRemainder()
{
    sdr::dsp::SpectrumWindowHopScheduler longWindow(250'000, 65'536, 60);
    QCOMPARE(longWindow.nominalHopSize(), 65'536.0);
    QCOMPARE(longWindow.nextHopSize(), std::uint64_t{65'536});
    QCOMPARE(longWindow.overlapPercentage(), 0.0);
    QVERIFY(std::abs(
                longWindow.achievableFramesPerSecond() -
                250'000.0 / 65'536.0) < 0.001);

    sdr::dsp::SpectrumWindowHopScheduler skipping(2'400'000, 1'024, 120);
    QCOMPARE(skipping.nextHopSize(), std::uint64_t{20'000});
    QCOMPARE(skipping.overlapPercentage(), 0.0);

    sdr::dsp::SpectrumWindowHopScheduler fractional(2'250'123, 4'096, 60);
    std::uint64_t accumulated = 0;
    bool sawFloor = false;
    bool sawCeiling = false;
    for (int row = 0; row < 60; ++row) {
        const auto hop = fractional.nextHopSize();
        accumulated += hop;
        sawFloor = sawFloor || hop == 37'502;
        sawCeiling = sawCeiling || hop == 37'503;
    }
    QCOMPARE(accumulated, std::uint64_t{2'250'123});
    QVERIFY(sawFloor);
    QVERIFY(sawCeiling);
    fractional.reset();
    QCOMPARE(fractional.nextHopSize(), std::uint64_t{37'502});
}

void SpectrumDataPathTest::keepsWindowHopRowRateStableAcrossCaptureBandwidths()
{
    constexpr std::uint64_t seconds = 120;
    for (const std::uint32_t targetRowsPerSecond : {15U, 30U, 60U, 120U}) {
        for (const auto sampleRate : {
                 250'000ULL, 1'000'000ULL, 1'200'000ULL,
                 2'000'000ULL, 2'250'000ULL, 2'400'000ULL}) {
            for (const std::size_t fftSize : {
                     1'024U, 2'048U, 4'096U, 8'192U,
                     16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
                sdr::dsp::SpectrumWindowHopScheduler scheduler(
                    sampleRate, fftSize, targetRowsPerSecond);
                const std::uint64_t rows =
                    static_cast<std::uint64_t>(targetRowsPerSecond) * seconds;
                std::uint64_t consumedSamples = 0;
                for (std::uint64_t row = 0; row < rows; ++row) {
                    consumedSamples += scheduler.nextHopSize();
                }
                const double actual = static_cast<double>(rows) /
                                      (static_cast<double>(consumedSamples) /
                                       static_cast<double>(sampleRate));
                const double expected = std::min(
                    static_cast<double>(targetRowsPerSecond),
                    static_cast<double>(sampleRate) /
                        static_cast<double>(fftSize));
                QVERIFY2(
                    std::abs(actual - expected) < 0.1,
                    qPrintable(QStringLiteral("%1 rows/s at %2 sps and FFT %3 achieved %4")
                                   .arg(targetRowsPerSecond)
                                   .arg(sampleRate)
                                   .arg(fftSize)
                                   .arg(actual, 0, 'f', 3)));
            }
        }
    }
}

void SpectrumDataPathTest::calculatesHertzPerBinAcrossConfiguredSizes()
{
    constexpr double effectiveSampleRate = 2'400'000.0;
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U,
             16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
        const double hertzPerBin = effectiveSampleRate /
                                   static_cast<double>(fftSize);
        QVERIFY(std::abs(hertzPerBin * static_cast<double>(fftSize) -
                         effectiveSampleRate) < 0.001);
    }
}

void SpectrumDataPathTest::mapsTheCompleteEffectiveSampleRateSpan()
{
    constexpr std::uint64_t centerFrequency = 100'000'000;
    constexpr std::uint64_t effectiveSampleRate = 2'400'000;
    constexpr double displayWidth = 1'200.0;
    const sdr::radio::FrequencyRange visibleRange{
        centerFrequency - effectiveSampleRate / 2,
        centerFrequency + effectiveSampleRate / 2,
    };

    QCOMPARE(
        sdr::radio::frequencyForPixel(0.0, displayWidth, visibleRange),
        std::optional<std::uint64_t>{98'800'000});
    QCOMPARE(
        sdr::radio::frequencyForPixel(displayWidth / 2, displayWidth, visibleRange),
        std::optional<std::uint64_t>{centerFrequency});
    QCOMPARE(
        sdr::radio::frequencyForPixel(displayWidth, displayWidth, visibleRange),
        std::optional<std::uint64_t>{101'200'000});
    QCOMPARE(
        sdr::radio::pixelForFrequency(visibleRange.maximum, displayWidth, visibleRange),
        std::optional<double>{displayWidth});
}

void SpectrumDataPathTest::mapsPlotOffsetsAndFractionalPositionsWithSharedAxis()
{
    const sdr::radio::FrequencyAxisMapper mapper(
        100'000'000, 2'000'000, {37.5, 800.0});
    QVERIFY(mapper.valid());
    QCOMPARE(
        mapper.positionForFrequency(99'000'000.0),
        std::optional<double>{37.5});
    QCOMPARE(
        mapper.positionForFrequency(100'000'000.0),
        std::optional<double>{437.5});
    QCOMPARE(
        mapper.positionForFrequency(100'250'000.0),
        std::optional<double>{537.5});
    QCOMPARE(
        mapper.frequencyForPosition(537.5),
        std::optional<double>{100'250'000.0});
}

void SpectrumDataPathTest::distinguishesCompatibilityFromExactTuningState()
{
    const SpectrumFrame oldFrame{
        .sequence = 9,
        .centerFrequency = 100'000'000,
        .sampleRate = 2'000'000,
        .fftSize = 2,
        .tuningGeneration = 4,
        .normalizedMagnitudes = {0.2F, 0.8F},
    };
    QVERIFY(sdr::radio::isCurrentTuningFrame(
        oldFrame, 100'000'000, 2'000'000, 4));
    QVERIFY(sdr::radio::isCompatibleSpectrumFrame(
        oldFrame, 2'000'000, 2));
    QVERIFY(!sdr::radio::isCurrentTuningFrame(
        oldFrame, 100'010'000, 2'000'000, 5));

    auto relabelledOldFrame = oldFrame;
    relabelledOldFrame.centerFrequency = 100'010'000;
    QVERIFY(!sdr::radio::isCurrentTuningFrame(
        relabelledOldFrame, 100'010'000, 2'000'000, 5));
    QVERIFY(sdr::radio::isCompatibleSpectrumFrame(
        relabelledOldFrame, 2'000'000, 2));
    QVERIFY(!sdr::radio::isCompatibleSpectrumFrame(
        relabelledOldFrame, 1'000'000, 2));
    QVERIFY(!sdr::radio::isCompatibleSpectrumFrame(
        relabelledOldFrame, 2'000'000, 4));
}

void SpectrumDataPathTest::countsPublishedFftFrames()
{
    auto queue = std::make_shared<SpectrumFrameQueue>(3);
    auto counters = std::make_shared<sdr::dsp::SpectrumProcessingCounters>();
    FftFrameProcessor processor(queue, counters, 4, 1.0F, -100.0F, 0.0F);
    const std::vector<float> magnitudes{1.0F, 0.1F, 0.01F, 0.0F};

    QVERIFY(processor.submitMagnitudeFrame(
        magnitudes, 100'000'000, 2'000'000, 1));
    QVERIFY(processor.submitMagnitudeFrame(
        magnitudes, 100'000'000, 2'000'000, 2));
    QCOMPARE(counters->fftsExecuted.load(), std::uint64_t{2});
    QCOMPARE(counters->framesPublished.load(), std::uint64_t{2});

    const auto latest = queue->takeLatest();
    QVERIFY(latest.has_value());
    QCOMPARE(latest->sequence, std::uint64_t{2});
    QCOMPARE(latest->normalizedMagnitudes.size(), std::size_t{4});
    QVERIFY(std::abs(latest->normalizedMagnitudes[0] - 0.879588F) < 0.0001F);
    QVERIFY(std::abs(latest->normalizedMagnitudes[1] - 0.679588F) < 0.0001F);
    QVERIFY(std::abs(latest->normalizedMagnitudes[2] - 0.479588F) < 0.0001F);
    QCOMPARE(latest->normalizedMagnitudes[3], 0.0F);
}

void SpectrumDataPathTest::normalizesFftMagnitudeAndWindowGain()
{
    constexpr float toneAmplitude = 0.5F;
    std::vector<float> displayedLevels;
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U,
             16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
        const auto window = sdr::dsp::makeHannWindow(fftSize);
        const float gain = sdr::dsp::coherentGain(window);
        QVERIFY(std::abs(gain - 0.5F) < 0.00001F);

        auto queue = std::make_shared<SpectrumFrameQueue>(1);
        auto counters = std::make_shared<sdr::dsp::SpectrumProcessingCounters>();
        FftFrameProcessor processor(
            queue, counters, fftSize, gain, -120.0F, 0.0F);
        std::vector<float> magnitude(fftSize, 0.0F);
        magnitude.front() = toneAmplitude * static_cast<float>(fftSize) * gain;
        QVERIFY(processor.submitMagnitudeFrame(
            magnitude, 100'000'000, 2'000'000, 1));
        const auto frame = queue->takeLatest();
        QVERIFY(frame.has_value());
        displayedLevels.push_back(frame->normalizedMagnitudes.front());
    }

    const float expected = (120.0F + 20.0F * std::log10(toneAmplitude)) / 120.0F;
    for (const float level : displayedLevels) {
        QVERIFY(std::abs(level - expected) < 0.00001F);
        QVERIFY(std::abs(level - displayedLevels.front()) < 0.00001F);
    }
}

void SpectrumDataPathTest::boundsQueueAndDropsStaleFrames()
{
    SpectrumFrameQueue queue(2);
    queue.push(SpectrumFrame{
        .sequence = 1,
        .centerFrequency = 100'000'000,
        .sampleRate = 2'000'000,
        .fftSize = 2,
        .tuningGeneration = 0,
        .normalizedMagnitudes = {0.1F, 0.1F},
    });
    queue.push(SpectrumFrame{
        .sequence = 2,
        .centerFrequency = 100'000'000,
        .sampleRate = 2'000'000,
        .fftSize = 2,
        .tuningGeneration = 0,
        .normalizedMagnitudes = {0.2F, 0.2F},
    });
    queue.push(SpectrumFrame{
        .sequence = 3,
        .centerFrequency = 100'000'000,
        .sampleRate = 2'000'000,
        .fftSize = 2,
        .tuningGeneration = 0,
        .normalizedMagnitudes = {0.3F, 0.3F},
    });

    QCOMPARE(queue.capacity(), std::size_t{2});
    QCOMPARE(queue.size(), std::size_t{2});
    QCOMPARE(queue.droppedFrameCount(), std::uint64_t{1});

    const auto latest = queue.takeLatest();
    QVERIFY(latest.has_value());
    QCOMPARE(latest->sequence, std::uint64_t{3});
    QCOMPARE(queue.size(), std::size_t{0});
    QCOMPARE(queue.droppedFrameCount(), std::uint64_t{2});
}

QTEST_GUILESS_MAIN(SpectrumDataPathTest)

#include "SpectrumDataPathTest.moc"
