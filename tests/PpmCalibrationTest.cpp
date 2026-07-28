// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "PpmCalibration.hpp"

#include <QTest>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> counterBytes(
    std::uint8_t& nextByte, std::uint64_t complexSamples)
{
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(complexSamples * 2));
    for (auto& byte : bytes) {
        byte = nextByte++;
    }
    return bytes;
}

std::uint64_t elapsedForPpm(
    std::uint64_t complexSamples,
    std::uint64_t requestedRate,
    double ppm)
{
    return static_cast<std::uint64_t>(std::llround(
        static_cast<long double>(complexSamples) * 1'000'000'000.0L /
        (static_cast<long double>(requestedRate) *
         (1.0L + static_cast<long double>(ppm) / 1'000'000.0L))));
}

class PpmCalibrationTest final : public QObject
{
    Q_OBJECT

private slots:
    void calculatesPositiveNegativeAndZeroPpm();
    void usesDriverCorrectionSignConvention();
    void ignoresCounterResetDuringSettling();
    void rejectsCounterDiscontinuity();
    void rejectsDriverReportedDroppedData();
    void suppressesMonotonicReadJitter();
    void finishesAfterTwoStableWindows();
    void rejectsUnstableWindows();
    void usesMedianOfThreeWindows();
};

void PpmCalibrationTest::calculatesPositiveNegativeAndZeroPpm()
{
    using sdr::devices::PpmCalibrationEstimator;
    const auto positive = PpmCalibrationEstimator::calculateMeasuredPpm(
        10'000'100, 10'000'000'000ULL, 1'000'000);
    const auto negative = PpmCalibrationEstimator::calculateMeasuredPpm(
        9'999'900, 10'000'000'000ULL, 1'000'000);
    const auto zero = PpmCalibrationEstimator::calculateMeasuredPpm(
        10'000'000, 10'000'000'000ULL, 1'000'000);
    QVERIFY(positive.has_value());
    QVERIFY(negative.has_value());
    QVERIFY(zero.has_value());
    QCOMPARE(*positive, 10.0);
    QCOMPARE(*negative, -10.0);
    QCOMPARE(*zero, 0.0);
}

void PpmCalibrationTest::usesDriverCorrectionSignConvention()
{
    using sdr::devices::PpmCalibrationEstimator;
    QCOMPARE(
        PpmCalibrationEstimator::driverCorrectionForMeasuredPpm(3.6), 4);
    QCOMPARE(
        PpmCalibrationEstimator::driverCorrectionForMeasuredPpm(-3.6), -4);
    QCOMPARE(
        PpmCalibrationEstimator::driverCorrectionForMeasuredPpm(0.2), 0);
}

void PpmCalibrationTest::ignoresCounterResetDuringSettling()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    estimator.start(1);
    const std::vector<std::uint8_t> startupReset{
        0, 1, 2, 3, 90, 91, 92, 93};
    estimator.ingest(startupReset, 2);
    QCOMPARE(
        estimator.phase(), sdr::devices::PpmCalibrationPhase::Settling);

    const std::vector<std::uint8_t> endOfSettling{94, 95, 96, 97};
    estimator.ingest(
        endOfSettling,
        1 + sdr::devices::ppmCalibrationSettlingNanoseconds);
    QCOMPARE(
        estimator.phase(),
        sdr::devices::PpmCalibrationPhase::MeasuringFirst);

    const std::vector<std::uint8_t> measurement{98, 99, 100, 101};
    estimator.ingest(
        measurement,
        2 + sdr::devices::ppmCalibrationSettlingNanoseconds);
    QCOMPARE(
        estimator.phase(),
        sdr::devices::PpmCalibrationPhase::MeasuringFirst);
}

void PpmCalibrationTest::rejectsCounterDiscontinuity()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    estimator.start(1);
    std::uint8_t next = 0;
    auto settling = counterBytes(next, 1);
    estimator.ingest(
        settling,
        1 + sdr::devices::ppmCalibrationSettlingNanoseconds);
    auto measurement = counterBytes(next, 10'000);
    measurement[100] = static_cast<std::uint8_t>(measurement[100] + 2);
    estimator.ingest(
        measurement,
        1 + sdr::devices::ppmCalibrationSettlingNanoseconds +
            10'000'000'000ULL);
    QCOMPARE(
        estimator.phase(), sdr::devices::PpmCalibrationPhase::Failed);
    QVERIFY(QString::fromStdString(estimator.failureMessage())
                .contains(QStringLiteral("discontinuity")));
}

void PpmCalibrationTest::rejectsDriverReportedDroppedData()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    estimator.start(1);
    std::uint8_t next = 0;
    const auto bytes = counterBytes(next, 1);
    estimator.ingest(bytes, 2, true);
    QCOMPARE(
        estimator.phase(), sdr::devices::PpmCalibrationPhase::Failed);
    QVERIFY(QString::fromStdString(estimator.failureMessage())
                .contains(QStringLiteral("dropped")));
}

void PpmCalibrationTest::suppressesMonotonicReadJitter()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    std::uint64_t now = 1;
    estimator.start(now);
    std::uint8_t next = 0;
    auto settling = counterBytes(next, 1);
    now += sdr::devices::ppmCalibrationSettlingNanoseconds;
    estimator.ingest(settling, now);

    for (int window = 0; window < 2; ++window) {
        const auto phase = estimator.phase();
        const std::uint64_t windowStart = now;
        for (std::uint64_t chunk = 1; estimator.phase() == phase; ++chunk) {
            auto bytes = counterBytes(next, 100);
            const std::int64_t jitterNanoseconds =
                chunk % 2 == 0 ? -500'000 : 500'000;
            now = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(windowStart) +
                static_cast<std::int64_t>(chunk * 100'000'000ULL) +
                jitterNanoseconds);
            estimator.ingest(bytes, now);
        }
    }

    QCOMPARE(
        estimator.phase(),
        sdr::devices::PpmCalibrationPhase::ReadyToApply);
    QCOMPARE(estimator.windows().size(), std::size_t{2});
    QCOMPARE(estimator.correctionPpm(), std::optional<int>{0});
}

void PpmCalibrationTest::finishesAfterTwoStableWindows()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    std::uint64_t now = 1;
    estimator.start(now);
    std::uint8_t next = 0;
    auto settling = counterBytes(next, 1);
    now += sdr::devices::ppmCalibrationSettlingNanoseconds;
    estimator.ingest(settling, now);
    for (const double ppm : {2.1, 2.2}) {
        auto bytes = counterBytes(next, 10'100);
        now += elapsedForPpm(10'100, 1'000, ppm);
        estimator.ingest(bytes, now);
    }
    QCOMPARE(
        estimator.phase(),
        sdr::devices::PpmCalibrationPhase::ReadyToApply);
    QCOMPARE(estimator.windows().size(), std::size_t{2});
    QCOMPARE(estimator.correctionPpm(), std::optional<int>{2});
}

void PpmCalibrationTest::rejectsUnstableWindows()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    std::uint64_t now = 1;
    estimator.start(now);
    std::uint8_t next = 0;
    auto settling = counterBytes(next, 1);
    now += sdr::devices::ppmCalibrationSettlingNanoseconds;
    estimator.ingest(settling, now);
    for (const double ppm : {0.0, 30.0, 1.0}) {
        auto bytes = counterBytes(next, 10'100);
        now += elapsedForPpm(10'100, 1'000, ppm);
        estimator.ingest(bytes, now);
    }
    QCOMPARE(
        estimator.phase(), sdr::devices::PpmCalibrationPhase::Failed);
    QVERIFY(QString::fromStdString(estimator.failureMessage())
                .contains(QStringLiteral("stable PPM spread")));
}

void PpmCalibrationTest::usesMedianOfThreeWindows()
{
    sdr::devices::PpmCalibrationEstimator estimator(1'000);
    std::uint64_t now = 1;
    estimator.start(now);
    std::uint8_t next = 0;
    auto settling = counterBytes(next, 1);
    now += sdr::devices::ppmCalibrationSettlingNanoseconds;
    estimator.ingest(settling, now);
    for (const double ppm : {1.1, 1.8, 1.2}) {
        auto bytes = counterBytes(next, 10'100);
        now += elapsedForPpm(10'100, 1'000, ppm);
        estimator.ingest(bytes, now);
    }
    QCOMPARE(
        estimator.phase(),
        sdr::devices::PpmCalibrationPhase::ReadyToApply);
    QCOMPARE(estimator.windows().size(), std::size_t{3});
    QCOMPARE(estimator.correctionPpm(), std::optional<int>{1});
}

}  // namespace

QTEST_GUILESS_MAIN(PpmCalibrationTest)

#include "PpmCalibrationTest.moc"
