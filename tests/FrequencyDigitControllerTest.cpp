// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FrequencyDigitController.hpp"

#include <QtTest>

#include <array>
#include <cstdint>
#include <vector>

using sdr::app::FrequencyDigitController;
using sdr::radio::FrequencyRange;

class FrequencyDigitControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void representsEveryDecimalPlace();
    void carriesAcrossDigits();
    void borrowsAcrossDigits();
    void zerosSelectedAndLessSignificantDigits();
    void enforcesMinimumFrequency();
    void enforcesMaximumFrequency();
    void enforcesDeviceSpecificRanges();
    void rejectsZeroingOutsideLimits();
};

void FrequencyDigitControllerTest::representsEveryDecimalPlace()
{
    constexpr std::array<std::uint64_t, 10> expectedPlaces{
        1'000'000'000,
        100'000'000,
        10'000'000,
        1'000'000,
        100'000,
        10'000,
        1'000,
        100,
        10,
        1,
    };
    const std::vector<FrequencyRange> allowed{{0, 9'999'999'999}};

    for (int index = 0; index < FrequencyDigitController::digitCount; ++index) {
        const auto arrayIndex = static_cast<std::size_t>(index);
        const auto place = FrequencyDigitController::placeValue(index);
        QVERIFY(place.has_value());
        QCOMPARE(*place, expectedPlaces[arrayIndex]);
        const auto result = FrequencyDigitController::adjustDigit(
            0, index, 1, allowed);
        QVERIFY(result.succeeded());
        QCOMPARE(result.frequency, expectedPlaces[arrayIndex]);

        const auto decremented = FrequencyDigitController::adjustDigit(
            expectedPlaces[arrayIndex], index, -1, allowed);
        QVERIFY(decremented.succeeded());
        QCOMPARE(decremented.frequency, std::uint64_t{0});
    }
}

void FrequencyDigitControllerTest::carriesAcrossDigits()
{
    const std::vector<FrequencyRange> allowed{{0, 9'999'999'999}};
    const auto result = FrequencyDigitController::adjustDigit(
        199'999'999, 9, 1, allowed);

    QVERIFY(result.succeeded());
    QVERIFY(!result.adjustedToLimit);
    QCOMPARE(result.frequency, std::uint64_t{200'000'000});
}

void FrequencyDigitControllerTest::borrowsAcrossDigits()
{
    const std::vector<FrequencyRange> allowed{{0, 9'999'999'999}};
    const auto result = FrequencyDigitController::adjustDigit(
        200'000'000, 9, -1, allowed);

    QVERIFY(result.succeeded());
    QVERIFY(!result.adjustedToLimit);
    QCOMPARE(result.frequency, std::uint64_t{199'999'999});
}

void FrequencyDigitControllerTest::zerosSelectedAndLessSignificantDigits()
{
    const std::vector<FrequencyRange> allowed{{0, 9'999'999'999}};
    constexpr std::uint64_t frequency = 1'234'567'899;
    for (int index = 0; index < FrequencyDigitController::digitCount; ++index) {
        const auto place = FrequencyDigitController::placeValue(index);
        QVERIFY(place.has_value());
        const std::uint64_t zeroingRange = *place * 10;
        const auto result = FrequencyDigitController::zeroFromDigit(
            frequency, index, allowed);

        QVERIFY(result.succeeded());
        QCOMPARE(
            result.frequency,
            (frequency / zeroingRange) * zeroingRange);
    }
}

void FrequencyDigitControllerTest::enforcesMinimumFrequency()
{
    const std::vector<FrequencyRange> allowed{{1'000'000, 9'998'999'999}};
    const auto result = FrequencyDigitController::adjustDigit(
        1'000'000, 0, -1, allowed);

    QVERIFY(result.succeeded());
    QVERIFY(result.adjustedToLimit);
    QCOMPARE(result.frequency, std::uint64_t{1'000'000});
}

void FrequencyDigitControllerTest::enforcesMaximumFrequency()
{
    const std::vector<FrequencyRange> allowed{{1'000'000, 9'998'999'999}};
    const auto result = FrequencyDigitController::adjustDigit(
        9'998'999'999, 9, 1, allowed);

    QVERIFY(result.succeeded());
    QVERIFY(result.adjustedToLimit);
    QCOMPARE(result.frequency, std::uint64_t{9'998'999'999});
}

void FrequencyDigitControllerTest::enforcesDeviceSpecificRanges()
{
    const std::vector<FrequencyRange> allowed{
        {1'800'000, 2'000'000},
        {88'000'000, 108'000'000},
    };

    const auto upper = FrequencyDigitController::adjustDigit(
        108'000'000, 9, 1, allowed);
    QVERIFY(upper.succeeded());
    QVERIFY(upper.adjustedToLimit);
    QCOMPARE(upper.frequency, std::uint64_t{108'000'000});

    const auto gap = FrequencyDigitController::constrain(50'000'000, allowed);
    QVERIFY(gap.succeeded());
    QVERIFY(gap.adjustedToLimit);
    QCOMPARE(gap.frequency, std::uint64_t{88'000'000});
}

void FrequencyDigitControllerTest::rejectsZeroingOutsideLimits()
{
    const std::vector<FrequencyRange> allowed{{88'000'000, 108'000'000}};
    const auto result = FrequencyDigitController::zeroFromDigit(
        100'123'456, 1, allowed);

    QVERIFY(!result.succeeded());
    QCOMPARE(result.frequency, std::uint64_t{0});
}

QTEST_GUILESS_MAIN(FrequencyDigitControllerTest)

#include "FrequencyDigitControllerTest.moc"
