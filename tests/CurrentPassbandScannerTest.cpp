// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "CurrentPassbandScanner.hpp"

#include <QtTest>

class CurrentPassbandScannerTest final : public QObject
{
    Q_OBJECT

private slots:
    void validatesSettingsAgainstUsablePassband();
    void wrapsAndPausesWithoutChangingTheCurrentFrequency();
    void holdsForSquelchAndResumesAfterTheDelay();
    void skipsOnceWhilePausedAndContinuesFromHold();
};

namespace {

constexpr sdr::radio::FrequencyRange passband{100, 200};
constexpr sdr::app::CurrentPassbandScanSettings settings{
    100,
    200,
    40,
    50,
    75,
};

}  // namespace

void CurrentPassbandScannerTest::validatesSettingsAgainstUsablePassband()
{
    auto invalid = settings;
    invalid.lowerFrequency = 99;
    QVERIFY(sdr::app::CurrentPassbandScanner::validate(invalid, passband));
    invalid = settings;
    invalid.lowerFrequency = 201;
    QVERIFY(sdr::app::CurrentPassbandScanner::validate(invalid, passband));
    invalid = settings;
    invalid.lowerFrequency = 180;
    invalid.upperFrequency = 120;
    QVERIFY(sdr::app::CurrentPassbandScanner::validate(invalid, passband));
    invalid = settings;
    invalid.stepSize = 0;
    QVERIFY(sdr::app::CurrentPassbandScanner::validate(invalid, passband));
    invalid = settings;
    invalid.dwellMilliseconds = 0;
    QVERIFY(sdr::app::CurrentPassbandScanner::validate(invalid, passband));
    invalid = settings;
    invalid.resumeDelayMilliseconds = -1;
    QVERIFY(sdr::app::CurrentPassbandScanner::validate(invalid, passband));
    QVERIFY(!sdr::app::CurrentPassbandScanner::validate(settings, passband));
}

void CurrentPassbandScannerTest::wrapsAndPausesWithoutChangingTheCurrentFrequency()
{
    sdr::app::CurrentPassbandScanner scanner;
    QVERIFY(scanner.start(settings, passband, 180, false));
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Running);
    QCOMPARE(scanner.currentFrequency(), 180ULL);
    QCOMPARE(scanner.advanceAfterDwell(), std::optional<std::uint64_t>{100});
    QCOMPARE(scanner.advanceAfterDwell(), std::optional<std::uint64_t>{140});
    scanner.pause();
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Paused);
    QCOMPARE(scanner.advanceAfterDwell(), std::nullopt);
    QCOMPARE(scanner.currentFrequency(), 140ULL);
    scanner.resume(false);
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Running);
}

void CurrentPassbandScannerTest::holdsForSquelchAndResumesAfterTheDelay()
{
    sdr::app::CurrentPassbandScanner scanner;
    QVERIFY(scanner.start(settings, passband, 100, false));
    scanner.setSquelchOpen(true);
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Holding);
    QCOMPARE(scanner.advanceAfterDwell(), std::nullopt);
    QCOMPARE(scanner.advanceAfterResumeDelay(true), std::nullopt);
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Holding);
    QCOMPARE(scanner.advanceAfterResumeDelay(false), std::optional<std::uint64_t>{140});
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Running);
}

void CurrentPassbandScannerTest::skipsOnceWhilePausedAndContinuesFromHold()
{
    sdr::app::CurrentPassbandScanner scanner;
    QVERIFY(scanner.start(settings, passband, 100, false));
    scanner.pause();
    QCOMPARE(scanner.skip(), std::optional<std::uint64_t>{140});
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Paused);
    scanner.resume(true);
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Holding);
    QCOMPARE(scanner.skip(), std::optional<std::uint64_t>{180});
    QCOMPARE(scanner.state(), sdr::app::CurrentPassbandScanState::Running);
}

QTEST_GUILESS_MAIN(CurrentPassbandScannerTest)

#include "CurrentPassbandScannerTest.moc"
