// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WideRangeScanPlanner.hpp"

#include <QtTest>

#include <limits>

class WideRangeScanPlannerTest final : public QObject
{
    Q_OBJECT

private slots:
    void plansTheMinimumSafeCaptureBlocks();
    void accountsForCompleteFilterAndEdgeGuard();
    void preservesAsymmetricSidebandOffsets();
    void keepsCenterWhenDynamicFilterStillFits();
    void rejectsDeviceGapsAndOversizedFilters();
    void mapsFrequenciesToStableBlockProgress();
};

namespace {

sdr::app::WideRangeCaptureGeometry geometry(
    std::uint64_t bandwidth = 100,
    std::uint64_t guard = 10,
    std::vector<sdr::radio::FrequencyRange> tuning = {{0, 1'000}})
{
    return {bandwidth, guard, tuning, tuning};
}

sdr::app::CurrentPassbandScanSettings settings(
    std::uint64_t lower,
    std::uint64_t upper,
    std::uint64_t step)
{
    return {lower, upper, step, 50, 75};
}

}  // namespace

void WideRangeScanPlannerTest::plansTheMinimumSafeCaptureBlocks()
{
    const auto planned = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 200, 20), {5, 5}, geometry());
    QVERIFY2(planned.succeeded(), planned.error.c_str());
    QCOMPARE(planned.plan->frequencies,
             (std::vector<std::uint64_t>{100, 120, 140, 160, 180, 200}));
    QCOMPARE(planned.plan->blocks.size(), std::size_t{2});
    QCOMPARE(planned.plan->blocks[0].firstFrequencyIndex, std::size_t{0});
    QCOMPARE(planned.plan->blocks[0].lastFrequencyIndex, std::size_t{3});
    QCOMPARE(planned.plan->blocks[0].centerFrequency, std::uint64_t{130});
    QCOMPARE(planned.plan->blocks[1].firstFrequencyIndex, std::size_t{4});
    QCOMPARE(planned.plan->blocks[1].lastFrequencyIndex, std::size_t{5});
    QCOMPARE(planned.plan->blocks[1].centerFrequency, std::uint64_t{190});
}

void WideRangeScanPlannerTest::accountsForCompleteFilterAndEdgeGuard()
{
    const auto capture = geometry();
    QVERIFY(sdr::app::WideRangeScanPlanner::frequencyFits(
        100, 135, {5, 5}, capture));
    QVERIFY(!sdr::app::WideRangeScanPlanner::frequencyFits(
        100, 136, {5, 5}, capture));
    QVERIFY(!sdr::app::WideRangeScanPlanner::frequencyFits(
        100, 60, {5, 5}, capture));

    const auto tooWide = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 100, 1), {41, 40}, capture);
    QVERIFY(!tooWide.succeeded());
    QVERIFY(QString::fromStdString(tooWide.error).contains(
        QStringLiteral("active receive filter")));

    const auto overflowingCount = sdr::app::WideRangeScanPlanner::plan(
        settings(0, std::numeric_limits<std::uint64_t>::max(), 1),
        {0, 0},
        geometry(100, 10, {{0, std::numeric_limits<std::uint64_t>::max()}}));
    QVERIFY(!overflowingCount.succeeded());
    QVERIFY(QString::fromStdString(overflowingCount.error).contains(
        QStringLiteral("too many frequencies")));
}

void WideRangeScanPlannerTest::preservesAsymmetricSidebandOffsets()
{
    const auto usb = sdr::app::WideRangeScanPlanner::filterOffsets(
        sdr::radio::DemodulationMode::Usb, 20);
    const auto lsb = sdr::app::WideRangeScanPlanner::filterOffsets(
        sdr::radio::DemodulationMode::Lsb, 20);
    QCOMPARE(usb.lower, std::uint64_t{0});
    QCOMPARE(usb.upper, std::uint64_t{20});
    QCOMPARE(lsb.lower, std::uint64_t{20});
    QCOMPARE(lsb.upper, std::uint64_t{0});

    const auto usbPlan = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 100, 1), usb, geometry());
    const auto lsbPlan = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 100, 1), lsb, geometry());
    QVERIFY(usbPlan.succeeded());
    QVERIFY(lsbPlan.succeeded());
    QCOMPARE(usbPlan.plan->blocks.front().centerFrequency, std::uint64_t{110});
    QCOMPARE(lsbPlan.plan->blocks.front().centerFrequency, std::uint64_t{90});
}

void WideRangeScanPlannerTest::keepsCenterWhenDynamicFilterStillFits()
{
    const auto capture = geometry(200, 10);
    QVERIFY(sdr::app::WideRangeScanPlanner::frequencyFits(
        200, 250, {20, 20}, capture));
    QVERIFY(sdr::app::WideRangeScanPlanner::frequencyFits(
        200, 250, {5, 5}, capture));
    QVERIFY(!sdr::app::WideRangeScanPlanner::frequencyFits(
        200, 250, {45, 45}, capture));
    QVERIFY(sdr::app::WideRangeScanPlanner::frequencyFits(
        250, 250, {45, 45}, capture));
}

void WideRangeScanPlannerTest::rejectsDeviceGapsAndOversizedFilters()
{
    const auto splitRanges = geometry(
        100,
        10,
        {{0, 150}, {200, 300}});
    const auto gap = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 200, 25), {0, 0}, splitRanges);
    QVERIFY(!gap.succeeded());
    QVERIFY(QString::fromStdString(gap.error).contains(
        QStringLiteral("tuning ranges")));

    const auto supported = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 200, 50), {0, 0}, splitRanges);
    QVERIFY(supported.succeeded());
    QCOMPARE(supported.plan->blocks.size(), std::size_t{2});
}

void WideRangeScanPlannerTest::mapsFrequenciesToStableBlockProgress()
{
    const auto planned = sdr::app::WideRangeScanPlanner::plan(
        settings(100, 200, 20), {5, 5}, geometry());
    QVERIFY(planned.succeeded());
    const auto frequencyIndex = sdr::app::WideRangeScanPlanner::frequencyIndex(
        *planned.plan, 180);
    QVERIFY(frequencyIndex.has_value());
    QCOMPARE(*frequencyIndex, std::size_t{4});
    QCOMPARE(
        sdr::app::WideRangeScanPlanner::blockIndex(
            *planned.plan, *frequencyIndex),
        std::optional<std::size_t>{1});
    QVERIFY(!sdr::app::WideRangeScanPlanner::frequencyIndex(
        *planned.plan, 181).has_value());
}

QTEST_GUILESS_MAIN(WideRangeScanPlannerTest)

#include "WideRangeScanPlannerTest.moc"
