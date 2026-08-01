// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumFramePacing.hpp"
#include "WaterfallFrameDelivery.hpp"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

struct SimulationResult {
    double measuredRowsPerSecond = 0.0;
    std::size_t maximumDepth = 0;
    std::size_t finalDepth = 0;
    sdr::app::WaterfallDeliveryMetrics metrics;
};

SimulationResult simulateBurstyHardware(
    std::uint64_t sampleRate,
    std::size_t fftSize = 2'048,
    double jitterSeconds = 0.0,
    std::uint32_t rowsPerSecond = 60)
{
    constexpr std::uint64_t hardwareBufferSamples = 131'072;
    constexpr double duration = 30.0;
    constexpr double measurementStart = 2.0;
    constexpr std::uint64_t centerFrequency = 100'000'000;

    sdr::dsp::SpectrumWindowHopScheduler producer(
        sampleRate, fftSize, rowsPerSecond);
    sdr::app::WaterfallFrameDelivery delivery(
        std::max<std::size_t>(64, rowsPerSecond));
    delivery.reset(sampleRate);

    const double presentationRate = std::min(
        static_cast<double>(rowsPerSecond),
        static_cast<double>(sampleRate) /
            static_cast<double>(fftSize));
    const double displayPeriod = 1.0 / presentationRate;
    double nextBurst = static_cast<double>(hardwareBufferSamples) /
                       static_cast<double>(sampleRate);
    double nextDisplay = displayPeriod;
    std::uint64_t availableSamples = 0;
    std::uint64_t nextWindowStart = 0;
    std::uint64_t sequence = 0;
    std::uint64_t measuredRows = 0;
    std::size_t maximumDepth = 0;
    bool jitterApplied = false;

    while (std::min(nextBurst, nextDisplay) <= duration) {
        if (nextBurst <= nextDisplay) {
            availableSamples += hardwareBufferSamples;
            while (nextWindowStart + fftSize <= availableSamples) {
                if (!delivery.enqueue({
                    .sequence = ++sequence,
                    .timestampNanoseconds =
                        1 + static_cast<std::uint64_t>(std::llround(
                                static_cast<long double>(
                                    nextWindowStart + fftSize / 2) *
                                1'000'000'000.0L /
                                static_cast<long double>(sampleRate))),
                    .centerFrequency = centerFrequency,
                    .sampleRate = sampleRate,
                    .fftSize = 2,
                    .tuningGeneration = 0,
                    .normalizedMagnitudes = {0.25F, 0.5F},
                })) {
                    throw std::runtime_error("simulated waterfall frame was rejected");
                }
                nextWindowStart += producer.nextHopSize();
            }
            maximumDepth = std::max(maximumDepth, delivery.size());
            nextBurst += static_cast<double>(hardwareBufferSamples) /
                         static_cast<double>(sampleRate);
            if (!jitterApplied && jitterSeconds > 0.0 && nextBurst >= 10.0) {
                nextBurst += jitterSeconds;
                jitterApplied = true;
            }
        } else {
            if (delivery.takeNextRow() && nextDisplay >= measurementStart) {
                ++measuredRows;
            }
            nextDisplay += displayPeriod;
        }
    }

    return {
        .measuredRowsPerSecond =
            static_cast<double>(measuredRows) / (duration - measurementStart),
        .maximumDepth = maximumDepth,
        .finalDepth = delivery.size(),
        .metrics = delivery.metrics(),
    };
}

sdr::radio::SpectrumFrame frame(
    std::uint64_t sequence, std::uint64_t sampleRate = 2'000'000)
{
    return {
        .sequence = sequence,
        .timestampNanoseconds = sequence * 10'000'000ULL,
        .centerFrequency = 100'000'000,
        .sampleRate = sampleRate,
        .fftSize = 2,
        .tuningGeneration = 0,
        .normalizedMagnitudes = {0.25F, 0.5F},
    };
}

}  // namespace

class WaterfallFrameDeliveryTest final : public QObject
{
    Q_OBJECT

private slots:
    void retainsNewestRowsFromBurstyHardwareAtEveryCaptureRate();
    void supportsFractionalEffectiveRatesAndFftSizes();
    void schedulesPresentationAtRequestedAndEffectiveCadences();
    void startsWithoutWaitingForAPrefill();
    void clearsOldRateFramesDuringRuntimeChanges();
    void clearsPendingFramesAcrossCenterRetunes();
    void recoversFromOverflowByDroppingOldest();
    void rejectsDuplicateAndRegressingRowsAndReportsSequenceGaps();
    void preservesSequentialRowsAtLiveCadenceAndCollapsesStalls();
    void catchesUpSmallSteadyBacklogsWithoutDroppingRows();
    void toleratesTemporaryProducerJitter();
    void remainsBoundedAndLeavesSpectrumCurrent();
};

void WaterfallFrameDeliveryTest::
    retainsNewestRowsFromBurstyHardwareAtEveryCaptureRate()
{
    for (const std::uint32_t rowsPerSecond : {15U, 30U, 60U, 120U}) {
        for (const std::uint64_t sampleRate : {
                 250'000ULL, 1'000'000ULL, 1'200'000ULL,
                 2'000'000ULL, 2'250'000ULL, 2'400'000ULL}) {
            const auto result = simulateBurstyHardware(
                sampleRate, 2'048, 0.0, rowsPerSecond);
            QVERIFY(result.measuredRowsPerSecond > 0.0);
            QVERIFY(result.measuredRowsPerSecond <=
                    static_cast<double>(rowsPerSecond) + 0.25);
            QVERIFY(result.maximumDepth <=
                    std::max<std::size_t>(64, rowsPerSecond));
            QCOMPARE(result.metrics.overflowDrops, std::uint64_t{0});
        }
    }
}

void WaterfallFrameDeliveryTest::supportsFractionalEffectiveRatesAndFftSizes()
{
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U,
             16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
        const auto result = simulateBurstyHardware(2'250'123, fftSize);
        const double expected = std::min(
            60.0,
            2'250'123.0 / static_cast<double>(fftSize));
        QVERIFY(std::abs(result.measuredRowsPerSecond - expected) < 0.25);
        QCOMPARE(result.metrics.overflowDrops, std::uint64_t{0});
    }
}

void WaterfallFrameDeliveryTest::schedulesPresentationAtRequestedAndEffectiveCadences()
{
    for (const double rate : {
             10.0, 12.5, 15.0,
             250'000.0 / 262'144.0,
             2'000'000.0 / 262'144.0}) {
        double fractionalMilliseconds = 0.0;
        std::uint64_t totalMilliseconds = 0;
        constexpr std::size_t ticks = 10'000;
        for (std::size_t tick = 0; tick < ticks; ++tick) {
            const auto interval =
                sdr::app::nextWaterfallPresentationInterval(
                    60.0, rate, fractionalMilliseconds);
            QVERIFY(interval.milliseconds > 0);
            QVERIFY(interval.fractionalMilliseconds >= 0.0);
            QVERIFY(interval.fractionalMilliseconds < 1.0);
            totalMilliseconds +=
                static_cast<std::uint64_t>(interval.milliseconds);
            fractionalMilliseconds = interval.fractionalMilliseconds;
        }
        const double measuredRate =
            static_cast<double>(ticks) * 1'000.0 /
            static_cast<double>(totalMilliseconds);
        QVERIFY(std::abs(measuredRate - std::min(60.0, rate)) < 0.001);
    }
}

void WaterfallFrameDeliveryTest::startsWithoutWaitingForAPrefill()
{
    sdr::app::WaterfallFrameDelivery delivery(8);
    delivery.reset(2'000'000, 2);
    QVERIFY(delivery.enqueue(frame(1)));
    const auto first = delivery.takeNextRow();
    QVERIFY(first.has_value());
    QCOMPARE(first->sequence, std::uint64_t{1});
}

void WaterfallFrameDeliveryTest::clearsOldRateFramesDuringRuntimeChanges()
{
    sdr::app::WaterfallFrameDelivery delivery(25);
    delivery.reset(1'000'000, 2);
    QVERIFY(delivery.enqueue(frame(1, 1'000'000)));

    delivery.reset(2'000'000, 2);
    QCOMPARE(delivery.size(), std::size_t{0});
    QVERIFY(!delivery.enqueue(frame(2, 1'000'000)));
    QVERIFY(delivery.enqueue(frame(3, 2'000'000)));
    const auto row = delivery.takeNextRow();
    QVERIFY(row.has_value());
    QCOMPARE(row->sequence, std::uint64_t{3});

    delivery.stop();
    QVERIFY(!delivery.enqueue(frame(4, 2'000'000)));
    QVERIFY(!delivery.takeNextRow().has_value());
}

void WaterfallFrameDeliveryTest::clearsPendingFramesAcrossCenterRetunes()
{
    sdr::app::WaterfallFrameDelivery delivery(8);
    delivery.reset(2'000'000, 2);

    auto first = frame(1);
    first.centerFrequency = 100'000'000;
    first.tuningGeneration = 0;
    auto second = frame(2);
    second.centerFrequency = 100'250'000;
    second.tuningGeneration = 1;
    auto third = frame(3);
    third.centerFrequency = 99'875'000;
    third.tuningGeneration = 2;
    QVERIFY(delivery.enqueue(std::move(first)));
    QVERIFY(delivery.enqueue(std::move(second)));
    QVERIFY(delivery.enqueue(std::move(third)));

    const auto row = delivery.takeNextRow();
    QVERIFY(row.has_value());
    QCOMPARE(row->sequence, std::uint64_t{3});
    QCOMPARE(delivery.metrics().rowsConsumed, std::uint64_t{1});
    QCOMPARE(delivery.metrics().staleGenerationDrops, std::uint64_t{2});
    QCOMPARE(delivery.metrics().displayUnderruns, std::uint64_t{0});
}

void WaterfallFrameDeliveryTest::recoversFromOverflowByDroppingOldest()
{
    sdr::app::WaterfallFrameDelivery delivery(4);
    delivery.reset(2'000'000, 2);
    for (std::uint64_t sequence = 1; sequence <= 7; ++sequence) {
        QVERIFY(delivery.enqueue(frame(sequence)));
    }
    QCOMPARE(delivery.size(), std::size_t{4});
    QCOMPARE(delivery.metrics().overflowDrops, std::uint64_t{3});
    const auto first = delivery.takeNextRow();
    QVERIFY(first.has_value());
    QCOMPARE(first->sequence, std::uint64_t{4});
    QCOMPARE(delivery.metrics().coalescedRows, std::uint64_t{0});
}

void WaterfallFrameDeliveryTest::rejectsDuplicateAndRegressingRowsAndReportsSequenceGaps()
{
    sdr::app::WaterfallFrameDelivery delivery(8);
    delivery.reset(2'000'000, 2);

    QVERIFY(delivery.enqueue(frame(1)));
    QVERIFY(!delivery.enqueue(frame(1)));
    QVERIFY(delivery.enqueue(frame(3)));
    auto regressingTimestamp = frame(4);
    regressingTimestamp.timestampNanoseconds = 20'000'000ULL;
    QVERIFY(!delivery.enqueue(std::move(regressingTimestamp)));
    QVERIFY(delivery.enqueue(frame(5)));

    const auto& metrics = delivery.metrics();
    QCOMPARE(metrics.sequenceGaps, std::uint64_t{2});
    QCOMPARE(metrics.duplicateRows, std::uint64_t{1});
    QCOMPARE(metrics.nonMonotonicTimestamps, std::uint64_t{1});
    QCOMPARE(metrics.lastProducedIntervalNanoseconds, 20'000'000ULL);
    QCOMPARE(delivery.size(), std::size_t{3});
}

void WaterfallFrameDeliveryTest::preservesSequentialRowsAtLiveCadenceAndCollapsesStalls()
{
    sdr::app::WaterfallFrameDelivery delivery(32);
    delivery.reset(2'000'000, 2);
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        QVERIFY(delivery.enqueue(frame(sequence)));
    }
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        const auto row = delivery.takeNextRow();
        QVERIFY(row.has_value());
        QCOMPARE(row->sequence, sequence);
        QCOMPARE(row->timestampNanoseconds, sequence * 10'000'000ULL);
    }
    for (std::uint64_t sequence = 5; sequence <= 20; ++sequence) {
        QVERIFY(delivery.enqueue(frame(sequence)));
    }
    const auto newest = delivery.takeNextRow();
    QVERIFY(newest.has_value());
    QCOMPARE(newest->sequence, std::uint64_t{20});
    QCOMPARE(delivery.size(), std::size_t{0});
    QCOMPARE(delivery.metrics().coalescedRows, std::uint64_t{15});

    // After the one bounded recovery, live rows resume in acquisition order.
    for (std::uint64_t sequence = 21; sequence <= 24; ++sequence) {
        QVERIFY(delivery.enqueue(frame(sequence)));
        const auto recovered = delivery.takeNextRow();
        QVERIFY(recovered.has_value());
        QCOMPARE(recovered->sequence, sequence);
        QCOMPARE(
            recovered->timestampNanoseconds,
            sequence * 10'000'000ULL);
    }
    QCOMPARE(delivery.metrics().coalescedRows, std::uint64_t{15});
}

void WaterfallFrameDeliveryTest::catchesUpSmallSteadyBacklogsWithoutDroppingRows()
{
    sdr::app::WaterfallFrameDelivery delivery(64);
    delivery.reset(2'000'000, 2);
    constexpr std::uint64_t sourceRows = 600;
    constexpr std::uint64_t deliveryTicks = 550;
    std::uint64_t produced = 0;
    std::uint64_t expectedSequence = 1;

    for (std::uint64_t tick = 1; tick <= deliveryTicks; ++tick) {
        const std::uint64_t requiredProduced = tick * sourceRows / deliveryTicks;
        while (produced < requiredProduced) {
            QVERIFY(delivery.enqueue(frame(++produced)));
        }
        const std::size_t budget =
            sdr::app::waterfallPresentationRowBudget(delivery.size());
        for (std::size_t row = 0; row < budget; ++row) {
            const auto delivered = delivery.takeNextRow();
            QVERIFY(delivered.has_value());
            QCOMPARE(delivered->sequence, expectedSequence++);
            QCOMPARE(
                delivered->timestampNanoseconds,
                (expectedSequence - 1) * 10'000'000ULL);
        }
    }
    while (delivery.size() > 0) {
        const auto delivered = delivery.takeNextRow();
        QVERIFY(delivered.has_value());
        QCOMPARE(delivered->sequence, expectedSequence++);
    }
    QCOMPARE(produced, sourceRows);
    QCOMPARE(expectedSequence, sourceRows + 1);
    QCOMPARE(delivery.metrics().coalescedRows, std::uint64_t{0});
    QCOMPARE(delivery.metrics().overflowDrops, std::uint64_t{0});
}

void WaterfallFrameDeliveryTest::toleratesTemporaryProducerJitter()
{
    const auto result = simulateBurstyHardware(2'400'000, 2'048, 0.12, 60);
    QVERIFY(std::abs(result.measuredRowsPerSecond - 60.0) < 0.5);
    QCOMPARE(result.metrics.overflowDrops, std::uint64_t{0});
    QVERIFY(result.metrics.displayUnderruns <= 50);
}

void WaterfallFrameDeliveryTest::remainsBoundedAndLeavesSpectrumCurrent()
{
    sdr::app::WaterfallFrameDelivery delivery(8);
    delivery.reset(2'000'000, 2);
    std::vector<sdr::radio::SpectrumFrame> burst;
    for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
        burst.push_back(frame(sequence));
        QVERIFY(delivery.enqueue(frame(sequence)));
    }
    QCOMPARE(delivery.size(), delivery.capacity());
    delivery.setCapacity(4);
    QCOMPARE(delivery.size(), std::size_t{4});
    QCOMPARE(delivery.capacity(), std::size_t{4});
    QCOMPARE(burst.back().sequence, std::uint64_t{20});
    const auto newestRetained = delivery.takeNextRow();
    QVERIFY(newestRetained.has_value());
    QCOMPARE(newestRetained->sequence, std::uint64_t{17});
    QCOMPARE(delivery.size(), std::size_t{3});
}

QTEST_GUILESS_MAIN(WaterfallFrameDeliveryTest)

#include "WaterfallFrameDeliveryTest.moc"
