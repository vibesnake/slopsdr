// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationModel.hpp"
#include "SpectrumAmplitudeScale.hpp"
#include "FrequencyAlignedDisplay.hpp"
#include "FilterIndicator.hpp"
#include "SpectrumWaterfallItem.hpp"
#include "SpectrumDisplayReduction.hpp"
#include "SpectrumFramePacing.hpp"
#include "WaterfallPalette.hpp"

#include <QMetaObject>
#include <QSettings>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGTextureMaterial>
#include <QtTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

sdr::radio::SpectrumFrame carrierFrame(
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    std::size_t bins,
    std::uint64_t carrierFrequency,
    std::uint64_t sequence,
    std::uint64_t tuningGeneration,
    std::uint64_t timestampNanoseconds = 0)
{
    sdr::radio::SpectrumFrame frame{
        .sequence = sequence,
        .timestampNanoseconds = timestampNanoseconds,
        .centerFrequency = centerFrequency,
        .sampleRate = sampleRate,
        .fftSize = bins,
        .tuningGeneration = tuningGeneration,
        .normalizedMagnitudes = std::vector<float>(bins, 0.0F),
    };
    const sdr::radio::FftBinFrequencyMapper sourceAxis(
        centerFrequency,
        sampleRate,
        {0.0, static_cast<double>(bins - 1)});
    const auto position = sourceAxis.positionForFrequency(
        static_cast<double>(carrierFrequency));
    if (position.has_value()) {
        frame.normalizedMagnitudes[static_cast<std::size_t>(
            std::llround(*position))] = 1.0F;
    }
    return frame;
}

std::size_t peakIndex(const std::vector<float>& values)
{
    return static_cast<std::size_t>(
        std::max_element(values.begin(), values.end()) - values.begin());
}

bool deliverSpectrumFrame(
    SpectrumWaterfallItem& item,
    const QVector<float>& magnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 sequence,
    quint64 timestampNanoseconds)
{
    return QMetaObject::invokeMethod(
        &item,
        "receiveSpectrumFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, magnitudes),
        Q_ARG(quint64, centerFrequency),
        Q_ARG(quint64, sampleRate),
        Q_ARG(quint64, static_cast<quint64>(magnitudes.size())),
        Q_ARG(quint64, sequence),
        Q_ARG(quint64, timestampNanoseconds),
        Q_ARG(quint64, 0));
}

bool deliverWaterfallFrame(
    SpectrumWaterfallItem& item,
    const QVector<float>& magnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 sequence,
    quint64 timestampNanoseconds)
{
    return QMetaObject::invokeMethod(
        &item,
        "receiveWaterfallFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, magnitudes),
        Q_ARG(quint64, centerFrequency),
        Q_ARG(quint64, sampleRate),
        Q_ARG(quint64, static_cast<quint64>(magnitudes.size())),
        Q_ARG(quint64, sequence),
        Q_ARG(quint64, timestampNanoseconds),
        Q_ARG(quint64, 0));
}

sdr::gui::WaterfallViewportDescriptor viewportDescriptor(
    const sdr::radio::SpectrumFrame& frame,
    sdr::radio::FrequencyRange visibleRange,
    std::size_t physicalWidth,
    double devicePixelRatio,
    std::uint64_t generation = 1)
{
    return {
        .generation = generation,
        .visibleRange = visibleRange,
        .physicalWidth = physicalWidth,
        .devicePixelRatio = devicePixelRatio,
        .captureCenterFrequency = frame.centerFrequency,
        .captureSampleRate = frame.sampleRate,
        .captureSpan = sdr::radio::captureSpan(frame),
        .captureFftSize = frame.fftSize,
        .tuningGeneration = frame.tuningGeneration,
    };
}

}  // namespace

class SpectrumWaterfallItemTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void preservesPeaksWhenReducingBins();
    void interpolatesWhenExpandingBins();
    void samplesWaterfallRowsWithoutHorizontalSmoothing();
    void preservesWaterfallPeaksWhenZoomedOut();
    void rendersPartialCapturePassbandsAtRfLimits();
    void retainsPeakAndLinearPowerStatisticsAtEveryFftSize();
    void averagesBinsAndFramesInLinearPower();
    void mapsDbfsToSpectrumCoordinatesAndClips();
    void fillsSpectrumWithIndependentPaletteGradient();
    void updatesIndependentSpectrumHoldEnvelopes();
    void persistsHoldVisibilityWithoutPersistingEnvelopeData();
    void resetsSpectrumHoldsForCaptureAndGainChanges();
    void preservesSpectrumHoldsAcrossDisplayOnlyChanges();
    void keepsSpectrumAndHoldsUpdatingAcrossVisibleHistoryChanges();
    void rendersSpectrumHoldsAbovePaletteAndLiveTrace();
    void retainsSpectrumFrameAcrossCenterRetunes();
    void pausesSpectrumAndWaterfallIndependently();
    void blanksPausedWaterfallForEveryScannerModeUntilResume();
    void keepsPausedDisplaysPausedWhileViewportPans();
    void exposesMajorTicksAndCustomRange();
    void estimatesAndSmoothsNoiseFloor();
    void calculatesHighDpiScaleMargin();
    void generatesSlopSpectrumPalette();
    void mapsWaterfallDbfsToSlopSpectrumAndClamps();
    void clampsWaterfallDbRangeWithoutSwappingHandles();
    void persistsAndValidatesWaterfallDbRange();
    void recolorsExistingWaterfallHistoryWithoutChangingSpectrumScale();
    void persistsAggregationAndRerendersRetainedHistory();
    void usesTheSamePaletteForLiveAndHistoricalRows();
    void leavesPersistedNonDefaultPalettePreferenceUntouched();
    void shiftsRowsForPositiveNegativeAndFractionalRetunes();
    void mapsSeveralCaptureCentersAndNewRowsToOneCarrier();
    void fillsUncoveredPixelsWithTheMinimumLevel();
    void repeatedlyProjectsOriginalRowsWithoutCumulativeBlur();
    void boundsCompactMixedResolutionHistoryByMemoryBudget();
    void selectsAdaptiveHistoryBinsAcrossSupportedSettings();
    void preservesNarrowPeaksInReducedHistory();
    void projectsSharpViewportRowsFromFullFftForBothAggregations();
    void strictlyRejectsMismatchedViewportRows();
    void boundsViewportHistoryByItsSeparateBudget();
    void fallsBackAcrossViewportChangesWithoutBlackSidesAndSwapsAtomically();
    void retainsRequestedDurationAfterWarmup();
    void boundsHistoryByTimestampDuration();
    void mapsTimestampedRowsToVerticalPixels();
    void showsNewStepsWithoutVisibleHistoryStartupDelay();
    void weightsAverageSmoothingByElapsedTime();
    void doesNotBlendDifferentTuningGenerations();
    void scrollsByStableFractionalPixels();
    void buildsPixelNativeRasterGeometryAcrossDurationsAndDpr();
    void preservesMonotonicRasterPhaseAcrossResize();
    void mapsEveryVisiblePixelRowAcrossDurations();
    void rendersEveryPhysicalWaterfallRowAcrossHistoryChanges();
    void recalculatesForResizeAndHighDpiWithoutResamplingRows();
    void reprojectsReducedHistoryAcrossRetunes();
    void sharesTheFullWidthPlotAcrossSpectrumWaterfallAndOverlay();
    void sharesZoomedFrequencyAxisBetweenSpectrumAndWaterfall();
    void coalescesRapidAxisChangesIntoOneRenderedProjection();
    void replacesOwnedTexturesSafely();
    void drawsExactSlimFilterGateAtMultipleDprValues();
    void adaptsFilterGateLinesToPhysicalPassbandWidth();
    void revealsFullLinesDuringActiveWidthAdjustment();
    void expiresFilterWidthLabelLifetimeWithControllableClock();
    void rendersFilterGateWithoutInputItems();

private:
    QTemporaryDir m_settingsDirectory;
};

class TestableSpectrumWaterfallItem final : public SpectrumWaterfallItem
{
public:
    using SpectrumWaterfallItem::updatePaintNode;
};

void SpectrumWaterfallItemTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("slopSDR Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("SpectrumWaterfallItemTest"));
    qputenv("XDG_CONFIG_HOME", m_settingsDirectory.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        m_settingsDirectory.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::SystemScope,
        m_settingsDirectory.path());
}

void SpectrumWaterfallItemTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
}

void SpectrumWaterfallItemTest::preservesPeaksWhenReducingBins()
{
    std::vector<float> magnitudes(4'096, 0.1F);
    magnitudes[2'345] = 1.0F;
    const auto reduced = sdr::gui::reduceSpectrumForDisplay(magnitudes, 512);

    QCOMPARE(reduced.size(), std::size_t{512});
    QCOMPARE(*std::max_element(reduced.begin(), reduced.end()), 1.0F);
    QCOMPARE(reduced.front(), 0.1F);
    QCOMPARE(reduced.back(), 0.1F);
}

void SpectrumWaterfallItemTest::interpolatesWhenExpandingBins()
{
    const std::vector<float> magnitudes{0.0F, 1.0F};
    const auto expanded = sdr::gui::reduceSpectrumForDisplay(magnitudes, 5);

    QCOMPARE(expanded.size(), std::size_t{5});
    QCOMPARE(expanded[0], 0.0F);
    QCOMPARE(expanded[1], 0.25F);
    QCOMPARE(expanded[2], 0.5F);
    QCOMPARE(expanded[3], 0.75F);
    QCOMPARE(expanded[4], 1.0F);
}

void SpectrumWaterfallItemTest::samplesWaterfallRowsWithoutHorizontalSmoothing()
{
    const sdr::gui::WaterfallHistoryRow row{
        .sequence = 1,
        .centerFrequency = 100'000,
        .sampleRate = 4,
        .captureSpan = 4,
        .fftSize = 4,
        .normalizedMagnitudes = {0, 10'000, 30'000, 65'535},
        .linearPowerSums = {},
    };
    const sdr::radio::FrequencyAxisMapper nativeAxis(100'000, 4, {0.0, 3.0});
    const auto native = sdr::gui::projectWaterfallRowToFrequencyAxis(
        row, nativeAxis, 4);
    QCOMPARE(native.size(), std::size_t{4});
    for (std::size_t column = 0; column < native.size(); ++column) {
        const float expected = static_cast<float>(row.normalizedMagnitudes[column]) /
                               65'535.0F;
        QVERIFY(std::abs(native[column] - expected) <= 1.0F / 65'535.0F);
    }

    const sdr::radio::FrequencyAxisMapper axis(100'000, 4, {0.0, 12.0});
    const auto projected = sdr::gui::projectWaterfallRowToFrequencyAxis(
        row, axis, 13);

    QCOMPARE(projected.size(), std::size_t{13});
    const std::array<std::size_t, 13> expectedBins{
        0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3,
    };
    for (std::size_t column = 0; column < projected.size(); ++column) {
        const float expected = static_cast<float>(
            row.normalizedMagnitudes[expectedBins[column]]) / 65'535.0F;
        QVERIFY(std::abs(projected[column] - expected) <= 1.0F / 65'535.0F);
    }
}

void SpectrumWaterfallItemTest::preservesWaterfallPeaksWhenZoomedOut()
{
    constexpr std::uint64_t center = 100'000;
    sdr::gui::WaterfallHistoryBuffer history(2);
    QVERIFY(history.append(carrierFrame(center, 4'096, 4'096, center + 731, 1, 0)));
    const auto projected = sdr::gui::projectWaterfallRowToFrequencyAxis(
        history.rows().front(),
        sdr::radio::FrequencyAxisMapper(center, 4'096, {0.0, 255.0}),
        256);

    QCOMPARE(projected.size(), std::size_t{256});
    QCOMPARE(*std::max_element(projected.begin(), projected.end()), 1.0F);
}

void SpectrumWaterfallItemTest::rendersPartialCapturePassbandsAtRfLimits()
{
    constexpr std::uint64_t sampleRate = 2'400'000;
    constexpr std::size_t bins = 25;
    constexpr sdr::radio::FrequencyRange deviceRange{
        500'000,
        1'766'000'000,
    };

    const sdr::radio::FftBinFrequencyMapper lowSource(
        500'000, sampleRate, {0.0, static_cast<double>(bins - 1)});
    QVERIFY(lowSource.valid());
    QCOMPARE(lowSource.nominalLowerFrequency(), -700'000.0);
    QCOMPARE(lowSource.nominalUpperFrequency(), 1'700'000.0);
    QCOMPARE(
        lowSource.positionForFrequency(500'000.0),
        std::optional<double>{12.0});

    for (const std::uint64_t centerFrequency : {
             500'000ULL,
             800'000ULL,
             1'000'000ULL,
             1'200'000ULL}) {
        const auto visibleRange = sdr::radio::visibleCaptureRange(
            centerFrequency, sampleRate, deviceRange);
        QVERIFY(visibleRange.has_value());
        QCOMPARE(visibleRange->minimum, std::uint64_t{500'000});
        const std::uint64_t expectedUpper =
            centerFrequency + (sampleRate - sampleRate / 2);
        QCOMPARE(visibleRange->maximum, expectedUpper);

        const std::size_t columns = static_cast<std::size_t>(
            (visibleRange->maximum - visibleRange->minimum) / 100'000 + 1);
        const sdr::radio::FrequencyAxisMapper targetAxis(
            *visibleRange, {0.0, static_cast<double>(columns - 1)});
        QVERIFY(targetAxis.valid());

        const std::uint64_t peakFrequency = centerFrequency + 600'000;
        const std::size_t expectedPeak = static_cast<std::size_t>(
            (peakFrequency - visibleRange->minimum) / 100'000);
        const auto liveFrame = carrierFrame(
            centerFrequency,
            sampleRate,
            bins,
            peakFrequency,
            1,
            0,
            1);
        const auto live = sdr::gui::projectFrameToFrequencyAxis(
            liveFrame, targetAxis, columns);

        sdr::gui::WaterfallHistoryBuffer history(2);
        QVERIFY(history.append(liveFrame));
        const auto& retained = history.rows().front();
        const auto original =
            sdr::gui::projectWaterfallRowToFrequencyAxis(
                retained, targetAxis, columns);
        const auto average =
            sdr::gui::projectAverageWaterfallRowToFrequencyAxis(
                retained, targetAxis, columns);

        std::vector<float> holds(bins, -120.0F);
        const auto holdPeakPosition =
            sdr::radio::FftBinFrequencyMapper(
                centerFrequency,
                sampleRate,
                {0.0, static_cast<double>(bins - 1)})
                .positionForFrequency(static_cast<double>(peakFrequency));
        QVERIFY(holdPeakPosition.has_value());
        holds[static_cast<std::size_t>(std::llround(*holdPeakPosition))] =
            -20.0F;
        std::vector<float> maximumHold(columns);
        sdr::gui::projectMaximumHoldToFrequencyAxis(
            holds,
            centerFrequency,
            sampleRate,
            targetAxis,
            -120.0F,
            maximumHold);

        QCOMPARE(live.size(), columns);
        QCOMPARE(*std::max_element(live.begin(), live.end()), 1.0F);
        QCOMPARE(peakIndex(live), expectedPeak);
        QCOMPARE(peakIndex(original), expectedPeak);
        QCOMPARE(peakIndex(average), expectedPeak);
        QCOMPARE(peakIndex(maximumHold), expectedPeak);
    }

    constexpr std::uint64_t vhfCenter = 100'000'000;
    const sdr::radio::FrequencyAxisMapper oldVhfSource(
        vhfCenter, sampleRate, {0.0, static_cast<double>(bins - 1)});
    const sdr::radio::FftBinFrequencyMapper newVhfSource(
        vhfCenter, sampleRate, {0.0, static_cast<double>(bins - 1)});
    QVERIFY(oldVhfSource.valid());
    QVERIFY(newVhfSource.valid());
    for (const double frequency : {
             98'800'000.0,
             99'400'000.0,
             100'000'000.0,
             100'600'000.0,
             101'200'000.0}) {
        QCOMPARE(
            newVhfSource.positionForFrequency(frequency),
            oldVhfSource.positionForFrequency(frequency));
    }
}

void SpectrumWaterfallItemTest::retainsPeakAndLinearPowerStatisticsAtEveryFftSize()
{
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U, 16'384U,
             32'768U, 65'536U, 131'072U, 262'144U}) {
        sdr::gui::WaterfallHistoryBuffer history(2);
        history.setStoredBinCount(2'048);
        sdr::radio::SpectrumFrame frame{
            .sequence = 1,
            .timestampNanoseconds = 1,
            .centerFrequency = 100'000'000,
            .sampleRate = 2'000'000,
            .captureSpan = 2'000'000,
            .fftSize = fftSize,
            .tuningGeneration = 0,
            .normalizedMagnitudes = std::vector<float>(fftSize, 0.0F),
        };
        for (std::size_t bin = 1; bin < fftSize; bin += 2) {
            frame.normalizedMagnitudes[bin] = 1.0F;
        }

        QVERIFY(history.append(std::move(frame)));
        const auto& retained = history.rows().front();
        QCOMPARE(
            retained.normalizedMagnitudes.size(),
            std::min<std::size_t>(fftSize, 2'048));
        QVERIFY(retained.normalizedMagnitudes.size() <= 2'048);
        if (fftSize > 2'048) {
            QCOMPARE(retained.linearPowerSums.size(), std::size_t{2'048});
            QVERIFY(std::all_of(
                retained.linearPowerSums.begin(),
                retained.linearPowerSums.end(),
                [](float sum) { return sum > 0.0F; }));
        } else {
            QVERIFY(retained.linearPowerSums.empty());
        }
        QCOMPARE(
            *std::max_element(
                retained.normalizedMagnitudes.begin(),
                retained.normalizedMagnitudes.end()),
            std::uint16_t{65'535});
        QVERIFY(history.memoryUsageBytes() <= history.memoryBudgetBytes());
    }
}

void SpectrumWaterfallItemTest::averagesBinsAndFramesInLinearPower()
{
    sdr::gui::WaterfallHistoryBuffer history(2);
    history.setStoredBinCount(2);
    sdr::radio::SpectrumFrame frame{
        .sequence = 1,
        .timestampNanoseconds = 1,
        .centerFrequency = 100'000,
        .sampleRate = 4,
        .captureSpan = 4,
        .fftSize = 4,
        .tuningGeneration = 0,
        .normalizedMagnitudes = {0.0F, 1.0F, 0.0F, 0.0F},
    };
    QVERIFY(history.append(std::move(frame)));
    const auto& retained = history.rows().front();
    QCOMPARE(retained.normalizedMagnitudes,
             std::vector<std::uint16_t>({65'535, 0}));
    QCOMPARE(retained.linearPowerSums.size(), std::size_t{2});

    const sdr::radio::FrequencyAxisMapper axis(100'000, 4, {0.0, 1.0});
    const auto original = sdr::gui::projectWaterfallRowToFrequencyAxis(
        retained, axis, 2);
    const auto average = sdr::gui::projectAverageWaterfallRowToFrequencyAxis(
        retained, axis, 2);
    QCOMPARE(original, std::vector<float>({1.0F, 0.0F}));
    QVERIFY(std::abs(average[0] - 0.5F) < 0.0001F);
    QVERIFY(std::abs(average[1] - 1.0e-12F) < 1.0e-15F);
    const float normalized =
        sdr::gui::normalizedMagnitudeForLinearPower(average[0]);
    QVERIFY(std::abs(normalized - 0.974914F) < 0.0001F);
    QVERIFY(std::abs(normalized - 0.5F) > 0.4F);

    const sdr::radio::FrequencyAxisMapper zoomAxis(100'000, 4, {0.0, 6.0});
    const auto zoomedAverage =
        sdr::gui::projectAverageWaterfallRowToFrequencyAxis(
            retained, zoomAxis, 7);
    for (const float power : zoomedAverage) {
        QVERIFY(std::abs(power - average[0]) < 0.0001F ||
                std::abs(power - average[1]) < 1.0e-15F);
    }

    std::vector<float> temporal{
        sdr::gui::linearPowerForNormalizedMagnitude(0.0F)};
    const std::vector<float> second{
        sdr::gui::linearPowerForNormalizedMagnitude(1.0F)};
    sdr::gui::combineWaterfallFrames(
        temporal, second, sdr::gui::WaterfallAggregation::Average);
    sdr::gui::finishWaterfallFrameAggregation(
        temporal, 2, sdr::gui::WaterfallAggregation::Average);
    QVERIFY(std::abs(temporal.front() - 0.5F) < 0.0001F);

    std::vector<float> peaks{0.2F};
    const std::vector<float> briefSignal{0.9F};
    sdr::gui::combineWaterfallFrames(
        peaks, briefSignal, sdr::gui::WaterfallAggregation::Original);
    sdr::gui::finishWaterfallFrameAggregation(
        peaks, 2, sdr::gui::WaterfallAggregation::Original);
    QCOMPARE(peaks.front(), 0.9F);
}

void SpectrumWaterfallItemTest::mapsDbfsToSpectrumCoordinatesAndClips()
{
    QCOMPARE(sdr::gui::spectrumYForDbfs(-20.0F, 200.0F, -120.0F, -20.0F), 0.0F);
    QCOMPARE(sdr::gui::spectrumYForDbfs(-120.0F, 200.0F, -120.0F, -20.0F), 200.0F);
    QCOMPARE(sdr::gui::spectrumYForDbfs(-70.0F, 200.0F, -120.0F, -20.0F), 100.0F);
    QCOMPARE(sdr::gui::spectrumYForDbfs(3.0F, 200.0F, -120.0F, -20.0F), 0.0F);
    QCOMPARE(sdr::gui::spectrumYForDbfs(-140.0F, 200.0F, -120.0F, -20.0F), 200.0F);
}

void SpectrumWaterfallItemTest::fillsSpectrumWithIndependentPaletteGradient()
{
    TestableSpectrumWaterfallItem item;
    item.setSize(QSizeF(100.0, 100.0));
    item.setSpectrumMinimumDbfs(-100.0F);
    item.setSpectrumMaximumDbfs(-40.0F);
    item.setWaterfallMinimumDbfs(-120.0F);
    item.setWaterfallMaximumDbfs(-20.0F);
    const QRgb spectrumColorBeforeWaterfallRangeChange =
        item.spectrumColorForDbfs(-60.0F);
    item.setWaterfallMinimumDbfs(-90.0F);
    item.setWaterfallMaximumDbfs(-10.0F);
    QCOMPARE(
        item.spectrumColorForDbfs(-60.0F),
        spectrumColorBeforeWaterfallRangeChange);

    const QVector<float> magnitudes{
        0.5F, 0.75F, 0.5F, 0.75F, 0.5F, 0.75F, 0.5F, 0.75F};
    QVERIFY(QMetaObject::invokeMethod(
        &item,
        "receiveFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, magnitudes)));

    QSGNode* root = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(root);
    QSGNode* content = root->firstChild();
    QVERIFY(content);
    QCOMPARE(content->childCount(), 5);

    auto* background = static_cast<QSGGeometryNode*>(content->childAtIndex(0));
    auto* fill = static_cast<QSGGeometryNode*>(content->childAtIndex(1));
    auto* trace = static_cast<QSGGeometryNode*>(content->childAtIndex(2));
    QCOMPARE(
        static_cast<QSGFlatColorMaterial*>(background->material())->color(),
        QColor(Qt::black));
    QCOMPARE(fill->geometry()->drawingMode(), QSGGeometry::DrawTriangleStrip);
    QCOMPARE(trace->geometry()->drawingMode(), QSGGeometry::DrawLineStrip);
    QCOMPARE(trace->geometry()->vertexCount(), 100);
    QCOMPARE(fill->geometry()->vertexCount(), 200);
    QVERIFY(static_cast<QSGTextureMaterial*>(fill->material()));

    const auto paletteCoordinate = [](int index) {
        return (static_cast<float>(index) + 0.5F) / 256.0F;
    };
    const auto* vertices = fill->geometry()->vertexDataAsTexturedPoint2D();
    QCOMPARE(vertices[0].y, item.yForDbfs(-60.0F, 100.0F));
    QCOMPARE(vertices[1].y, 100.0F);
    QCOMPARE(vertices[198].y, item.yForDbfs(-30.0F, 100.0F));
    QCOMPARE(vertices[1].tx, paletteCoordinate(0));
    QCOMPARE(
        vertices[0].tx,
        paletteCoordinate(sdr::gui::waterfallPaletteIndex(-60.0F, -100.0F, -40.0F)));
    QCOMPARE(
        vertices[198].tx,
        paletteCoordinate(sdr::gui::waterfallPaletteIndex(-30.0F, -100.0F, -40.0F)));
    QVERIFY(vertices[0].tx != vertices[198].tx);

    delete root;
}

void SpectrumWaterfallItemTest::updatesIndependentSpectrumHoldEnvelopes()
{
    SpectrumWaterfallItem item;
    const QVector<float> first{0.2F, 0.8F, 0.5F, 1.0F};
    QVERIFY(deliverSpectrumFrame(
        item, first, 100'000'000, 2'000'000, 1, 100));
    QVERIFY(item.spectrumHoldsAvailable());

    const QVector<float> second{0.1F, 0.9F, 0.7F, 0.4F};
    QVERIFY(deliverSpectrumFrame(
        item, second, 100'000'000, 2'000'000, 2, 200));
    const QVector<float> expectedMaximum{
        sdr::gui::dbfsForNormalizedSpectrum(0.2F),
        sdr::gui::dbfsForNormalizedSpectrum(0.9F),
        sdr::gui::dbfsForNormalizedSpectrum(0.7F),
        sdr::gui::dbfsForNormalizedSpectrum(1.0F),
    };
    QCOMPARE(item.maximumHoldDbfs(), expectedMaximum);

    const QVector<float> duplicate{0.0F, 1.0F, 0.0F, 1.0F};
    QVERIFY(deliverSpectrumFrame(
        item, duplicate, 100'000'000, 2'000'000, 2, 200));
    QCOMPARE(item.maximumHoldDbfs(), expectedMaximum);
}

void SpectrumWaterfallItemTest::persistsHoldVisibilityWithoutPersistingEnvelopeData()
{
    SpectrumWaterfallItem item;
    const QVector<float> frame{0.2F, 0.4F, 0.6F, 0.8F};
    QVERIFY(deliverSpectrumFrame(
        item, frame, 100'000'000, 2'000'000, 1, 100));
    const QVector<float> maximum = item.maximumHoldDbfs();

    item.setMaximumHoldEnabled(true);
    QCOMPARE(item.maximumHoldDbfs(), maximum);
    item.setMaximumHoldEnabled(false);
    QCOMPARE(item.maximumHoldDbfs(), maximum);

    SpectrumWaterfallItem restored;
    QVERIFY(!restored.maximumHoldEnabled());
    QVERIFY(!restored.spectrumHoldsAvailable());
    QVERIFY(restored.maximumHoldDbfs().isEmpty());
}

void SpectrumWaterfallItemTest::resetsSpectrumHoldsForCaptureAndGainChanges()
{
    ApplicationModel model;
    SpectrumWaterfallItem item;
    item.setApplicationModel(&model);
    QVector<float> frame(
        static_cast<qsizetype>(model.effectiveSpectrumFftSize()), 0.5F);
    quint64 sequence = 1;
    quint64 timestamp = 100;
    const auto initialize = [&] {
        QVERIFY(deliverSpectrumFrame(
            item,
            frame,
            model.centerFrequency(),
            model.effectiveSampleRate(),
            sequence++,
            timestamp += 100));
        QVERIFY(item.spectrumHoldsAvailable());
    };

    initialize();
    model.setCenterFrequencyText(QStringLiteral("100500000"));
    QVERIFY(!item.spectrumHoldsAvailable());

    initialize();
    model.previewGain(model.requestedGain() + model.gainStep());
    QVERIFY(!item.spectrumHoldsAvailable());

    initialize();
    model.setGain(model.requestedGain() + model.gainStep());
    QVERIFY(!item.spectrumHoldsAvailable());

    initialize();
    model.setSpectrumFftSize(2'048);
    QVERIFY(!item.spectrumHoldsAvailable());
    frame.fill(0.5F, 2'048);

    initialize();
    model.setSampleRate(1'000'000);
    QVERIFY(!item.spectrumHoldsAvailable());
}

void SpectrumWaterfallItemTest::preservesSpectrumHoldsAcrossDisplayOnlyChanges()
{
    ApplicationModel model;
    SpectrumWaterfallItem item;
    item.setApplicationModel(&model);
    const QVector<float> frame(
        static_cast<qsizetype>(model.effectiveSpectrumFftSize()), 0.5F);
    QVERIFY(deliverSpectrumFrame(
        item,
        frame,
        model.centerFrequency(),
        model.effectiveSampleRate(),
        1,
        100));
    const QVector<float> maximum = item.maximumHoldDbfs();

    item.setMaximumHoldEnabled(true);
    item.setWaterfallMinimumDbfs(-110.0F);
    item.setWaterfallMaximumDbfs(-10.0F);
    item.setSpectrumMinimumDbfs(-110.0F);
    item.setSpectrumMaximumDbfs(-10.0F);
    model.setListeningFrequency(model.listeningFrequency() + 10'000);
    model.setFilterWidth(model.filterWidth() + 500);
    QVERIFY(QMetaObject::invokeMethod(
        &item, "frequencyAxisChanged", Qt::DirectConnection));

    QCOMPARE(item.maximumHoldDbfs(), maximum);
}

void SpectrumWaterfallItemTest::
    keepsSpectrumAndHoldsUpdatingAcrossVisibleHistoryChanges()
{
    TestableSpectrumWaterfallItem spectrum;
    spectrum.setSize(QSizeF(100.0, 100.0));
    spectrum.setMaximumHoldEnabled(true);

    SpectrumWaterfallItem waterfall;
    waterfall.setSize(QSizeF(100.0, 100.0));
    waterfall.setWaterfall(true);

    const std::array<float, 5> liveLevels{0.2F, 0.8F, 0.1F, 0.9F, 0.4F};
    const std::array<quint32, 5> historyDurations{10, 5, 30, 15, 60};
    QVector<float> expectedMaximum;
    QSGNode* root = nullptr;
    QSGNode* retainedContent = nullptr;
    quint64 previousWaterfallMemory = 0;

    for (qsizetype frameIndex = 0;
         frameIndex < static_cast<qsizetype>(liveLevels.size());
         ++frameIndex) {
        spectrum.setVisibleHistorySeconds(
            historyDurations[static_cast<std::size_t>(frameIndex)]);
        waterfall.setVisibleHistorySeconds(
            historyDurations[static_cast<std::size_t>(frameIndex)]);

        QVector<float> frame(100, 0.5F);
        frame[0] = liveLevels[static_cast<std::size_t>(frameIndex)];
        frame[50] = 1.0F - frame[0] * 0.5F;
        const quint64 sequence = static_cast<quint64>(frameIndex + 1);
        const quint64 timestamp = sequence * 1'000'000'000ULL;
        QVERIFY(deliverSpectrumFrame(
            spectrum,
            frame,
            100'000'000,
            2'000'000,
            sequence,
            timestamp));
        QVERIFY(deliverWaterfallFrame(
            waterfall,
            frame,
            100'000'000,
            2'000'000,
            sequence,
            timestamp));

        if (expectedMaximum.isEmpty()) {
            expectedMaximum.resize(frame.size());
            for (qsizetype index = 0; index < frame.size(); ++index) {
                const float dbfs =
                    sdr::gui::dbfsForNormalizedSpectrum(frame[index]);
                expectedMaximum[index] = dbfs;
            }
        } else {
            for (qsizetype index = 0; index < frame.size(); ++index) {
                const float dbfs =
                    sdr::gui::dbfsForNormalizedSpectrum(frame[index]);
                expectedMaximum[index] =
                    std::max(expectedMaximum[index], dbfs);
            }
        }

        root = spectrum.updatePaintNode(root, nullptr);
        QVERIFY(root);
        if (!retainedContent) {
            retainedContent = root->firstChild();
        }
        QCOMPARE(root->firstChild(), retainedContent);
        QVERIFY(retainedContent);
        auto* liveTrace = static_cast<QSGGeometryNode*>(
            retainedContent->childAtIndex(2));
        QCOMPARE(liveTrace->geometry()->vertexCount(), 100);
        QCOMPARE(
            liveTrace->geometry()->vertexDataAsPoint2D()[0].y,
            spectrum.yForDbfs(
                sdr::gui::dbfsForNormalizedSpectrum(frame[0]),
                static_cast<float>(spectrum.height())));
        QCOMPARE(spectrum.maximumHoldDbfs(), expectedMaximum);
        QVERIFY(waterfall.historyMemoryUsageBytes() >
                previousWaterfallMemory);
        previousWaterfallMemory = waterfall.historyMemoryUsageBytes();
    }

    delete root;
}

void SpectrumWaterfallItemTest::rendersSpectrumHoldsAbovePaletteAndLiveTrace()
{
    TestableSpectrumWaterfallItem item;
    item.setSize(QSizeF(100.0, 100.0));
    item.setMaximumHoldEnabled(true);
    const QVector<float> first{
        0.2F, 0.4F, 0.6F, 0.8F, 0.8F, 0.6F, 0.4F, 0.2F};
    const QVector<float> second{
        0.8F, 0.6F, 0.4F, 0.2F, 0.2F, 0.4F, 0.6F, 0.8F};
    QVERIFY(deliverSpectrumFrame(
        item, first, 100'000'000, 2'000'000, 1, 100));
    QVERIFY(deliverSpectrumFrame(
        item, second, 100'000'000, 2'000'000, 2, 200));

    QSGNode* root = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(root);
    QSGNode* content = root->firstChild();
    QVERIFY(content);
    QCOMPARE(content->childCount(), 5);
    auto* liveTrace =
        static_cast<QSGGeometryNode*>(content->childAtIndex(2));
    auto* maximumUnder =
        static_cast<QSGGeometryNode*>(content->childAtIndex(3));
    auto* maximumWhite =
        static_cast<QSGGeometryNode*>(content->childAtIndex(4));
    QCOMPARE(liveTrace->geometry()->drawingMode(), QSGGeometry::DrawLineStrip);
    for (auto* hold : {maximumUnder, maximumWhite}) {
        QCOMPARE(hold->geometry()->drawingMode(), QSGGeometry::DrawLineStrip);
        QCOMPARE(hold->geometry()->vertexCount(), 100);
    }
    QCOMPARE(maximumUnder->geometry()->lineWidth(), 3.0F);
    QCOMPARE(maximumWhite->geometry()->lineWidth(), 1.0F);
    QCOMPARE(
        static_cast<QSGFlatColorMaterial*>(maximumWhite->material())->color(),
        QColor(Qt::white));
    QVERIFY(
        static_cast<QSGFlatColorMaterial*>(maximumUnder->material())
            ->color()
            .alpha() > 0);

    delete root;
}

void SpectrumWaterfallItemTest::retainsSpectrumFrameAcrossCenterRetunes()
{
    ApplicationModel model;
    TestableSpectrumWaterfallItem item;
    item.setSize(QSizeF(100.0, 100.0));
    item.setApplicationModel(&model);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);

    QVERIFY(QTest::qWaitFor(
        [&spectrumFrames] { return spectrumFrames.count() >= 1; }, 1'000));
    QVERIFY(item.spectrumHoldsAvailable());
    QSGNode* root = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(root);
    QSGNode* retainedContent = root->firstChild();
    QVERIFY(retainedContent);
    auto* trace = static_cast<QSGGeometryNode*>(
        retainedContent->childAtIndex(2));
    QCOMPARE(trace->geometry()->vertexCount(), 100);
    QVERIFY(
        trace->geometry()->vertexDataAsPoint2D()[99].y < item.height());

    spectrumFrames.clear();
    model.setCenterFrequencyText(QStringLiteral("100500000"));
    QCOMPARE(spectrumResets.count(), 0);
    QCOMPARE(spectrumFrames.count(), 0);
    QVERIFY(!item.spectrumHoldsAvailable());

    root = item.updatePaintNode(root, nullptr);
    QCOMPARE(root->firstChild(), retainedContent);
    trace = static_cast<QSGGeometryNode*>(
        retainedContent->childAtIndex(2));
    const auto* retainedVertices =
        trace->geometry()->vertexDataAsPoint2D();
    QVERIFY(retainedVertices[0].y < item.height());
    QCOMPARE(retainedVertices[99].y, static_cast<float>(item.height()));

    const auto intermediate = carrierFrame(
        100'250'000,
        2'000'000,
        100,
        100'500'000,
        2,
        1,
        2);
    const QVector<float> intermediateMagnitudes(
        intermediate.normalizedMagnitudes.begin(),
        intermediate.normalizedMagnitudes.end());
    model.spectrumFrameReady(
        intermediateMagnitudes,
        intermediate.centerFrequency,
        intermediate.sampleRate,
        static_cast<quint64>(intermediate.fftSize),
        intermediate.sequence,
        intermediate.timestampNanoseconds,
        intermediate.tuningGeneration);
    QCOMPARE(
        spectrumFrames.last().at(1).toULongLong(),
        qulonglong{100'250'000});
    QVERIFY(!item.spectrumHoldsAvailable());
    root = item.updatePaintNode(root, nullptr);
    trace = static_cast<QSGGeometryNode*>(
        retainedContent->childAtIndex(2));
    const auto* intermediateVertices =
        trace->geometry()->vertexDataAsPoint2D();
    int carrierColumn = 0;
    for (int column = 1; column < trace->geometry()->vertexCount(); ++column) {
        if (intermediateVertices[column].y <
            intermediateVertices[carrierColumn].y) {
            carrierColumn = column;
        }
    }
    QVERIFY(std::abs(
        intermediateVertices[carrierColumn].x -
        item.xForFrequency(100'500'000)) <= 2.0F);

    spectrumFrames.clear();
    QVERIFY(QTest::qWaitFor(
        [&spectrumFrames] { return spectrumFrames.count() >= 1; }, 1'000));
    for (const auto& arguments : spectrumFrames) {
        QVERIFY(arguments.front().value<QVector<float>>().size() >= 2);
        QCOMPARE(arguments.at(1).toULongLong(), qulonglong{100'500'000});
    }
    QVERIFY(item.spectrumHoldsAvailable());
    root = item.updatePaintNode(root, nullptr);
    QCOMPARE(root->firstChild(), retainedContent);
    trace = static_cast<QSGGeometryNode*>(
        retainedContent->childAtIndex(2));
    QVERIFY(
        trace->geometry()->vertexDataAsPoint2D()[99].y < item.height());

    delete root;
}

void SpectrumWaterfallItemTest::pausesSpectrumAndWaterfallIndependently()
{
    SpectrumWaterfallItem spectrum;
    SpectrumWaterfallItem waterfall;
    waterfall.setWaterfall(true);

    const QVector<float> first{0.1F, 0.8F, 0.2F, 0.4F};
    QVERIFY(deliverSpectrumFrame(
        spectrum, first, 100'000'000, 2'000'000, 1, 100));
    QVERIFY(deliverWaterfallFrame(
        waterfall, first, 100'000'000, 2'000'000, 1, 100));
    QCOMPARE(spectrum.m_latestFrame.sequence, std::uint64_t{1});
    QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{1});
    const auto frozenSpectrum = spectrum.m_latestFrame.normalizedMagnitudes;

    spectrum.setPaused(true);
    QVERIFY(spectrum.paused());
    QVERIFY(!waterfall.paused());
    const QVector<float> pausedSpectrumFrame{0.9F, 0.2F, 0.7F, 0.3F};
    QVERIFY(deliverSpectrumFrame(
        spectrum, pausedSpectrumFrame, 100'000'000, 2'000'000, 2, 200));
    QVERIFY(deliverWaterfallFrame(
        waterfall, pausedSpectrumFrame, 100'000'000, 2'000'000, 2, 200));
    QCOMPARE(spectrum.m_latestFrame.sequence, std::uint64_t{1});
    QCOMPARE(spectrum.m_latestFrame.normalizedMagnitudes, frozenSpectrum);
    QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{2});

    spectrum.setPaused(false);
    QVERIFY(!spectrum.paused());
    const QVector<float> resumedSpectrumFrame{0.6F, 0.3F, 0.9F, 0.1F};
    QVERIFY(deliverSpectrumFrame(
        spectrum, resumedSpectrumFrame, 100'000'000, 2'000'000, 3, 300));
    QCOMPARE(spectrum.m_latestFrame.sequence, std::uint64_t{3});
    QCOMPARE(
        spectrum.m_latestFrame.normalizedMagnitudes,
        std::vector<float>(
            resumedSpectrumFrame.begin(), resumedSpectrumFrame.end()));

    waterfall.setPaused(true);
    QVERIFY(waterfall.paused());
    QVERIFY(!spectrum.paused());
    QVERIFY(deliverWaterfallFrame(
        waterfall, resumedSpectrumFrame, 100'000'000, 2'000'000, 3, 300));
    QCOMPARE(waterfall.m_latestFrame.sequence, std::uint64_t{2});
    QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{2});
    QVERIFY(deliverSpectrumFrame(
        spectrum, first, 100'000'000, 2'000'000, 4, 400));
    QCOMPARE(spectrum.m_latestFrame.sequence, std::uint64_t{4});

    waterfall.setPaused(false);
    QVERIFY(!waterfall.paused());
    QVERIFY(deliverWaterfallFrame(
        waterfall, first, 100'000'000, 2'000'000, 4, 400));
    QCOMPARE(waterfall.m_latestFrame.sequence, std::uint64_t{4});
    QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{3});
}

void SpectrumWaterfallItemTest::blanksPausedWaterfallForEveryScannerModeUntilResume()
{
    enum class ScannerMode { CurrentPassband, WideRange, Bookmarks };
    for (const auto scannerMode : {ScannerMode::CurrentPassband,
             ScannerMode::WideRange, ScannerMode::Bookmarks}) {
        ApplicationModel model;
        SpectrumWaterfallItem waterfall;
        waterfall.setWaterfall(true);
        waterfall.setApplicationModel(&model);
        const QVector<float> stale{0.1F, 0.8F, 0.2F, 0.4F};
        QVERIFY(deliverWaterfallFrame(
            waterfall, stale, 100'000'000, 2'000'000, 1, 100));
        QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{1});
        waterfall.setPaused(true);

        model.startReception();
        if (scannerMode == ScannerMode::WideRange) {
            model.setScanTypeIndex(1);
            model.setScanLowerFrequency(99'000'000);
            model.setScanUpperFrequency(101'000'000);
            model.setScanStepSize(1'000'000);
            model.startScan();
        } else if (scannerMode == ScannerMode::Bookmarks) {
            auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
                model.bookmarkModel());
            QVERIFY(bookmarks);
            sdr::app::BookmarkData bookmark;
            bookmark.name = QStringLiteral("Paused waterfall");
            bookmark.listeningFrequency = 100'000'000;
            bookmark.demodulatorId = QStringLiteral("am");
            bookmark.filterLowHz = -6'250;
            bookmark.filterHighHz = 6'250;
            bookmark.scannerIncluded = true;
            QVERIFY(!bookmarks->addBookmark(-1, bookmark).isEmpty());
            model.startBookmarkScan();
        } else {
            model.startScan();
        }

        QVERIFY(model.scannerOwnsTuning());
        QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{0});
        QVERIFY(waterfall.m_waterfallImage.isNull());
        QVERIFY(deliverWaterfallFrame(
            waterfall, stale, 100'000'000, 2'000'000, 2, 200));
        QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{0});

        if (scannerMode == ScannerMode::Bookmarks) {
            model.stopBookmarkScan();
        } else {
            model.stopScan();
        }
        QVERIFY(!model.scannerOwnsTuning());
        QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{0});
        waterfall.setPaused(false);
        const QVector<float> current{0.9F, 0.3F, 0.6F, 0.2F};
        QVERIFY(deliverWaterfallFrame(
            waterfall, current, model.centerFrequency(),
            model.effectiveSampleRate(), 3, 300));
        QCOMPARE(waterfall.m_waterfallHistory.size(), std::size_t{1});
        QCOMPARE(waterfall.m_latestFrame.sequence, std::uint64_t{3});
    }
}

void SpectrumWaterfallItemTest::keepsPausedDisplaysPausedWhileViewportPans()
{
    ApplicationModel model;
    SpectrumWaterfallItem spectrum;
    SpectrumWaterfallItem waterfall;
    waterfall.setWaterfall(true);
    spectrum.setWidth(800.0);
    waterfall.setWidth(800.0);
    spectrum.setApplicationModel(&model);
    waterfall.setApplicationModel(&model);

    const QVector<float> frame{0.1F, 0.8F, 0.2F, 0.4F};
    QVERIFY(deliverSpectrumFrame(
        spectrum, frame, 100'000'000, 2'000'000, 1, 100));
    QVERIFY(deliverWaterfallFrame(
        waterfall, frame, 100'000'000, 2'000'000, 1, 100));
    spectrum.setPaused(true);
    waterfall.setPaused(true);
    const float beforePan = spectrum.xForFrequency(100'000'000);

    model.requestWaterfallZoom(240);
    QVERIFY(QTest::qWaitFor(
        [&model] { return model.displayPanEnabled(); }, 500));
    model.setDisplayPanPosition(0.0);
    QCoreApplication::processEvents();

    QVERIFY(spectrum.paused());
    QVERIFY(waterfall.paused());
    QCOMPARE(spectrum.m_latestFrame.sequence, std::uint64_t{1});
    QCOMPARE(waterfall.m_latestFrame.sequence, std::uint64_t{1});
    QVERIFY(spectrum.xForFrequency(100'000'000) != beforePan);
    QCOMPARE(
        spectrum.xForFrequency(100'000'000),
        waterfall.xForFrequency(100'000'000));
}

void SpectrumWaterfallItemTest::exposesMajorTicksAndCustomRange()
{
    const auto defaultTicks = sdr::gui::majorDbfsTicks(-120.0F, -20.0F);
    QCOMPARE(defaultTicks, std::vector<float>({-120.0F, -100.0F, -80.0F,
                                                -60.0F, -40.0F, -20.0F}));

    SpectrumWaterfallItem item;
    item.setSpectrumMinimumDbfs(-100.0F);
    item.setSpectrumMaximumDbfs(-40.0F);
    QCOMPARE(item.spectrumMinimumDbfs(), -100.0F);
    QCOMPARE(item.spectrumMaximumDbfs(), -40.0F);
    QCOMPARE(item.yForDbfs(-70.0F, 0.0F), 0.0F);
    QCOMPARE(item.yForDbfs(-70.0F, 200.0F), 100.0F);
    QCOMPARE(item.majorDbfsTicks(), QVariantList({-100.0F, -80.0F, -60.0F, -40.0F}));
}

void SpectrumWaterfallItemTest::estimatesAndSmoothsNoiseFloor()
{
    std::vector<float> magnitudes(256, 0.20F);
    magnitudes[11] = 1.0F;
    magnitudes[92] = 0.95F;
    magnitudes[175] = 0.90F;

    const auto estimate = sdr::gui::estimateNoiseFloorDbfs(magnitudes);
    QVERIFY(estimate.has_value());
    QVERIFY(std::abs(*estimate - -96.0F) < 0.001F);
    QCOMPARE(sdr::gui::smoothNoiseFloorDbfs(-100.0F, -80.0F, 0.18F), -96.4F);
}

void SpectrumWaterfallItemTest::calculatesHighDpiScaleMargin()
{
    QCOMPARE(sdr::gui::amplitudeScaleMarginForPanel(320.0F, 1.0F), 48.0F);
    QCOMPARE(sdr::gui::amplitudeScaleMarginForPanel(1'280.0F, 1.0F), 60.0F);

    const float highDpiMargin = sdr::gui::amplitudeScaleMarginForPanel(780.0F, 2.0F);
    QVERIFY(std::abs(highDpiMargin - 58.5F) < 0.001F);
    QVERIFY(std::abs(std::round(highDpiMargin * 2.0F) - highDpiMargin * 2.0F) <
            0.001F);
}

void SpectrumWaterfallItemTest::generatesSlopSpectrumPalette()
{
    const auto& palette = sdr::gui::slopSpectrumPalette();
    const auto& repeatedPalette = sdr::gui::slopSpectrumPalette();
    QCOMPARE(palette.size(), std::size_t{256});
    QCOMPARE(&palette, &repeatedPalette);
    QVERIFY(std::equal(palette.begin(), palette.end(), repeatedPalette.begin()));
    for (const QRgb color : palette) {
        QCOMPARE(qAlpha(color), 255);
    }
    QCOMPARE(palette.front(), qRgba(0, 0, 0, 255));
    QCOMPARE(palette.back(), qRgba(255, 245, 220, 255));
    for (const std::size_t index : {16U, 32U, 48U, 64U}) {
        QVERIFY(qBlue(palette[index]) > qRed(palette[index]));
        QVERIFY(qBlue(palette[index]) > qGreen(palette[index]));
    }
    for (const std::size_t index : {16U, 32U, 48U, 64U}) {
        QVERIFY(qGreen(palette[index]) < qBlue(palette[index]));
    }
    QVERIFY(qGreen(palette[108]) >= 180);
    QVERIFY(qBlue(palette[108]) >= 200);
    QVERIFY(qGreen(palette[141]) > qRed(palette[141]));
    QVERIFY(qGreen(palette[141]) > qBlue(palette[141]));
    QVERIFY(qRed(palette[176]) > qGreen(palette[176]));
    QVERIFY(qGreen(palette[176]) > qBlue(palette[176]));
    QVERIFY(qRed(palette[205]) > qGreen(palette[205]));
    QVERIFY(qGreen(palette[205]) > qBlue(palette[205]));
    QVERIFY(qRed(palette[233]) > qGreen(palette[233]));
    QVERIFY(qRed(palette[233]) > qBlue(palette[233]));
    QVERIFY(qGreen(palette[248]) > qBlue(palette[248]));
    QVERIFY(qBlue(palette[248]) > 150);

    for (const QRgb color : palette) {
        QVERIFY(!(qRed(color) > qGreen(color) &&
                  qBlue(color) > qGreen(color)));
    }

    for (std::size_t index = 1; index < palette.size(); ++index) {
        const QRgb previous = palette[index - 1];
        const QRgb current = palette[index];
        const int difference = std::abs(qRed(current) - qRed(previous)) +
                               std::abs(qGreen(current) - qGreen(previous)) +
                               std::abs(qBlue(current) - qBlue(previous));
        QVERIFY(difference <= 40);
    }
}

void SpectrumWaterfallItemTest::mapsWaterfallDbfsToSlopSpectrumAndClamps()
{
    constexpr float minimumDbfs = -120.0F;
    constexpr float maximumDbfs = -20.0F;
    QCOMPARE(sdr::gui::waterfallPaletteIndex(-120.0F, minimumDbfs, maximumDbfs), 0);
    QCOMPARE(sdr::gui::waterfallPaletteIndex(-70.0F, minimumDbfs, maximumDbfs), 127);
    QCOMPARE(sdr::gui::waterfallPaletteIndex(-20.0F, minimumDbfs, maximumDbfs), 255);
    QCOMPARE(sdr::gui::waterfallPaletteIndex(-200.0F, minimumDbfs, maximumDbfs), 0);
    QCOMPARE(sdr::gui::waterfallPaletteIndex(10.0F, minimumDbfs, maximumDbfs), 255);
    QCOMPARE(
        sdr::gui::waterfallPaletteIndex(
            std::numeric_limits<float>::quiet_NaN(), minimumDbfs, maximumDbfs),
        0);
    QCOMPARE(sdr::gui::waterfallPaletteIndex(-70.0F, maximumDbfs, minimumDbfs), 0);
    QCOMPARE(
        sdr::gui::slopSpectrumColor(-70.0F, minimumDbfs, maximumDbfs),
        sdr::gui::slopSpectrumPalette()[127]);
}

void SpectrumWaterfallItemTest::clampsWaterfallDbRangeWithoutSwappingHandles()
{
    SpectrumWaterfallItem item;
    item.setWaterfallMinimumDbfs(-120.0F);
    item.setWaterfallMaximumDbfs(-20.0F);

    item.setWaterfallMinimumDbfs(-200.0F);
    QCOMPARE(item.waterfallMinimumDbfs(), -140.0F);
    item.setWaterfallMaximumDbfs(10.0F);
    QCOMPARE(item.waterfallMaximumDbfs(), 0.0F);

    item.setWaterfallMinimumDbfs(-1.0F);
    QCOMPARE(item.waterfallMinimumDbfs(), -5.0F);
    QCOMPARE(item.waterfallMaximumDbfs(), 0.0F);

    item.setWaterfallMinimumDbfs(-140.0F);
    item.setWaterfallMaximumDbfs(-140.0F);
    QCOMPARE(item.waterfallMinimumDbfs(), -140.0F);
    QCOMPARE(item.waterfallMaximumDbfs(), -135.0F);
}

void SpectrumWaterfallItemTest::persistsAndValidatesWaterfallDbRange()
{
    QSettings settings;
    const QString minimumKey = QStringLiteral("waterfall/minimumDbfs");
    const QString maximumKey = QStringLiteral("waterfall/maximumDbfs");
    const QVariant previousMinimum = settings.value(minimumKey);
    const QVariant previousMaximum = settings.value(maximumKey);

    settings.setValue(minimumKey, -105);
    settings.setValue(maximumKey, -35);
    {
        SpectrumWaterfallItem restored;
        QCOMPARE(restored.waterfallMinimumDbfs(), -105.0F);
        QCOMPARE(restored.waterfallMaximumDbfs(), -35.0F);
    }

    settings.setValue(minimumKey, QStringLiteral("not a number"));
    settings.setValue(maximumKey, -35);
    {
        SpectrumWaterfallItem invalid;
        QCOMPARE(invalid.waterfallMinimumDbfs(), -120.0F);
        QCOMPARE(invalid.waterfallMaximumDbfs(), -20.0F);
    }

    if (previousMinimum.isValid()) {
        settings.setValue(minimumKey, previousMinimum);
    } else {
        settings.remove(minimumKey);
    }
    if (previousMaximum.isValid()) {
        settings.setValue(maximumKey, previousMaximum);
    } else {
        settings.remove(maximumKey);
    }
}

void SpectrumWaterfallItemTest::recolorsExistingWaterfallHistoryWithoutChangingSpectrumScale()
{
    QQuickWindow window;
    window.resize(640, 240);
    auto* item = new SpectrumWaterfallItem(window.contentItem());
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(640.0, 240.0));
    item->setWaterfall(true);
    item->setWaterfallMinimumDbfs(-120.0F);
    item->setWaterfallMaximumDbfs(-20.0F);
    const float spectrumMinimum = item->spectrumMinimumDbfs();
    const float spectrumMaximum = item->spectrumMaximumDbfs();

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVector<float> magnitudes(2'048, 0.5F);
    QVERIFY(QMetaObject::invokeMethod(
        item,
        "receiveFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, magnitudes)));
    const auto waitForProjection = [item](std::uint64_t expected) {
        return QTest::qWaitFor(
            [item, expected] {
                return item->waterfallReprojectionCount() >= expected;
            },
            1'000);
    };
    QVERIFY(waitForProjection(1));
    const auto reprojections = item->waterfallReprojectionCount();
    const auto historyBytes = item->historyMemoryUsageBytes();
    const QRgb originalColor = item->waterfallColorForNormalizedMagnitude(0.5F);

    item->setWaterfallMinimumDbfs(-100.0F);
    item->setWaterfallMaximumDbfs(-40.0F);
    QVERIFY(waitForProjection(reprojections + 1));
    QCOMPARE(item->historyMemoryUsageBytes(), historyBytes);
    QVERIFY(item->waterfallColorForNormalizedMagnitude(0.5F) != originalColor);
    QCOMPARE(item->spectrumMinimumDbfs(), spectrumMinimum);
    QCOMPARE(item->spectrumMaximumDbfs(), spectrumMaximum);
    window.close();
}

void SpectrumWaterfallItemTest::persistsAggregationAndRerendersRetainedHistory()
{
    QSettings settings;
    const QString key = QStringLiteral("waterfall/aggregation");
    const QVariant previous = settings.value(key);
    settings.remove(key);
    {
        SpectrumWaterfallItem item;
        QCOMPARE(item.waterfallAggregation(), QStringLiteral("original"));
        item.setWaterfallAggregation(QStringLiteral("average"));
    }
    {
        SpectrumWaterfallItem restored;
        QCOMPARE(restored.waterfallAggregation(), QStringLiteral("average"));
    }
    settings.setValue(key, QStringLiteral("invalid"));
    {
        SpectrumWaterfallItem validated;
        QCOMPARE(validated.waterfallAggregation(), QStringLiteral("original"));
        QCOMPARE(settings.value(key).toString(), QStringLiteral("original"));
    }

    QQuickWindow window;
    window.resize(640, 240);
    auto* item = new SpectrumWaterfallItem(window.contentItem());
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(640.0, 240.0));
    item->setWaterfall(true);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVector<float> magnitudes(4'096, 0.35F);
    magnitudes[2'048] = 1.0F;
    QVERIFY(QMetaObject::invokeMethod(
        item,
        "receiveFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, magnitudes)));
    const auto waitForProjection = [item](std::uint64_t expected) {
        QElapsedTimer timer;
        timer.start();
        while (item->waterfallReprojectionCount() < expected &&
               timer.elapsed() < 1'000) {
            QCoreApplication::processEvents();
            QTest::qWait(5);
        }
        return item->waterfallReprojectionCount() >= expected;
    };
    QVERIFY(waitForProjection(1));
    const auto originalProjection = item->waterfallReprojectionCount();
    const auto historyBytes = item->historyMemoryUsageBytes();
    const auto storedBins = item->storedHistoryBins();

    item->setWaterfallAggregation(QStringLiteral("average"));
    QVERIFY(waitForProjection(originalProjection + 1));
    QCOMPARE(item->historyMemoryUsageBytes(), historyBytes);
    QCOMPARE(item->storedHistoryBins(), storedBins);
    const auto averageProjection = item->waterfallReprojectionCount();

    item->setWaterfallAggregation(QStringLiteral("original"));
    QVERIFY(waitForProjection(averageProjection + 1));
    QCOMPARE(item->historyMemoryUsageBytes(), historyBytes);
    QCOMPARE(item->storedHistoryBins(), storedBins);
    window.close();

    if (previous.isValid()) {
        settings.setValue(key, previous);
    } else {
        settings.remove(key);
    }
}

void SpectrumWaterfallItemTest::usesTheSamePaletteForLiveAndHistoricalRows()
{
    SpectrumWaterfallItem item;
    QCOMPARE(item.waterfallPaletteName(), QStringLiteral("Slop Spectrum"));
    QCOMPARE(item.emptyWaterfallColor(), qRgb(0, 0, 0));
    QCOMPARE(
        item.spectrumColorForDbfs(-70.0F),
        sdr::gui::slopSpectrumColor(-70.0F, -120.0F, -20.0F));

    const auto row = carrierFrame(100'000, 1'000, 11, 100'000, 1, 0);
    const sdr::radio::FrequencyAxisMapper axis(100'100, 1'000, {0.0, 10.0});
    const auto reprojected = sdr::gui::projectFrameToFrequencyAxis(row, axis, 11, 0.0F);
    QCOMPARE(item.waterfallColorForNormalizedMagnitude(row.normalizedMagnitudes[5]),
             item.waterfallColorForNormalizedMagnitude(reprojected[4]));
    QCOMPARE(item.waterfallColorForNormalizedMagnitude(reprojected.back()),
             item.emptyWaterfallColor());
}

void SpectrumWaterfallItemTest::leavesPersistedNonDefaultPalettePreferenceUntouched()
{
    QSettings settings;
    const QString key = QStringLiteral("waterfall/palette");
    const QVariant previous = settings.value(key);
    settings.setValue(key, QStringLiteral("Existing User Palette"));
    {
        SpectrumWaterfallItem item;
        QCOMPARE(item.waterfallPaletteName(), QStringLiteral("Slop Spectrum"));
    }
    QCOMPARE(settings.value(key).toString(), QStringLiteral("Existing User Palette"));
    if (previous.isValid()) {
        settings.setValue(key, previous);
    } else {
        settings.remove(key);
    }
}

void SpectrumWaterfallItemTest::shiftsRowsForPositiveNegativeAndFractionalRetunes()
{
    const auto frame = carrierFrame(100'000, 1'000, 11, 100'000, 1, 0);
    const sdr::radio::FrequencyAxisMapper shiftedUp(
        100'100, 1'000, {0.0, 10.0});
    const sdr::radio::FrequencyAxisMapper shiftedDown(
        99'900, 1'000, {0.0, 10.0});
    const auto positive = sdr::gui::projectFrameToFrequencyAxis(
        frame, shiftedUp, 11);
    const auto negative = sdr::gui::projectFrameToFrequencyAxis(
        frame, shiftedDown, 11);
    QCOMPARE(peakIndex(positive), std::size_t{4});
    QCOMPARE(peakIndex(negative), std::size_t{6});

    const sdr::radio::FrequencyAxisMapper halfPixelShift(
        100'050, 1'000, {0.0, 10.0});
    const auto fractional = sdr::gui::projectFrameToFrequencyAxis(
        frame, halfPixelShift, 11);
    QVERIFY(std::abs(fractional[4] - 0.5F) < 0.0001F);
    QVERIFY(std::abs(fractional[5] - 0.5F) < 0.0001F);
}

void SpectrumWaterfallItemTest::mapsSeveralCaptureCentersAndNewRowsToOneCarrier()
{
    constexpr std::uint64_t carrier = 100'100;
    const auto first = carrierFrame(100'000, 1'000, 101, carrier, 1, 0);
    const auto second = carrierFrame(100'200, 1'000, 101, carrier, 2, 1);
    const auto newest = carrierFrame(100'300, 1'000, 101, carrier, 3, 2);
    const sdr::radio::FrequencyAxisMapper currentAxis(
        100'300, 1'000, {0.0, 100.0});

    const auto firstProjected = sdr::gui::projectFrameToFrequencyAxis(
        first, currentAxis, 101);
    const auto secondProjected = sdr::gui::projectFrameToFrequencyAxis(
        second, currentAxis, 101);
    const auto newProjected = sdr::gui::projectFrameToFrequencyAxis(
        newest, currentAxis, 101);
    QCOMPARE(peakIndex(firstProjected), std::size_t{30});
    QCOMPARE(peakIndex(secondProjected), std::size_t{30});
    QCOMPARE(peakIndex(newProjected), std::size_t{30});
    QCOMPARE(
        currentAxis.positionForFrequency(static_cast<double>(carrier)),
        std::optional<double>{30.0});
}

void SpectrumWaterfallItemTest::fillsUncoveredPixelsWithTheMinimumLevel()
{
    sdr::radio::SpectrumFrame frame{
        .sequence = 1,
        .centerFrequency = 100'000,
        .sampleRate = 1'000,
        .fftSize = 11,
        .tuningGeneration = 0,
        .normalizedMagnitudes = std::vector<float>(11, 0.65F),
    };
    const sdr::radio::FrequencyAxisMapper target(
        100'500, 1'000, {0.0, 10.0});
    const auto projected = sdr::gui::projectFrameToFrequencyAxis(
        frame, target, 11, 0.0F);
    QCOMPARE(projected[5], 0.65F);
    for (std::size_t column = 6; column < projected.size(); ++column) {
        QCOMPARE(projected[column], 0.0F);
    }
}

void SpectrumWaterfallItemTest::repeatedlyProjectsOriginalRowsWithoutCumulativeBlur()
{
    const auto original = carrierFrame(100'000, 1'000, 101, 100'000, 7, 0);
    sdr::gui::WaterfallHistoryBuffer history(4);
    QVERIFY(history.append(original));

    for (int retune = -9; retune <= 9; ++retune) {
        const sdr::radio::FrequencyAxisMapper axis(
            static_cast<std::uint64_t>(100'000 + retune * 7),
            1'000,
            {0.0, 100.0});
        const auto projected = sdr::gui::projectFrameToFrequencyAxis(
            history.rows().front(), axis, 101);
        QVERIFY(!projected.empty());
    }

    const sdr::radio::FrequencyAxisMapper originalAxis(
        100'000, 1'000, {0.0, 100.0});
    const auto afterRetunes = sdr::gui::projectFrameToFrequencyAxis(
        history.rows().front(), originalAxis, 101);
    QCOMPARE(afterRetunes.size(), original.normalizedMagnitudes.size());
    for (std::size_t index = 0; index < afterRetunes.size(); ++index) {
        QVERIFY(std::abs(afterRetunes[index] - original.normalizedMagnitudes[index]) <=
                1.0F / 65'535.0F);
    }
    QCOMPARE(history.rows().front().normalizedMagnitudes.size(),
             original.normalizedMagnitudes.size());
    QCOMPARE(history.rows().front().centerFrequency, original.centerFrequency);
}

void SpectrumWaterfallItemTest::boundsCompactMixedResolutionHistoryByMemoryBudget()
{
    constexpr std::size_t metadataBytes =
        2U * sizeof(sdr::gui::WaterfallHistoryRow);
    constexpr std::size_t budget = 65'536U * sizeof(std::uint16_t) +
                                   4'096U * sizeof(std::uint16_t) +
                                   metadataBytes;
    sdr::gui::WaterfallHistoryBuffer history(128, budget);
    const auto small = carrierFrame(100'000, 2'400'000, 4'096, 100'000, 1, 0);
    const auto large = carrierFrame(100'000, 2'400'000, 65'536, 100'000, 2, 0);
    history.setStoredBinCount(4'096);
    QVERIFY(history.append(small));
    history.setStoredBinCount(65'536);
    QVERIFY(history.append(large));
    QCOMPARE(history.size(), std::size_t{2});
    QCOMPARE(history.memoryUsageBytes(), budget);
    QCOMPARE(history.rows().front().fftSize, std::size_t{65'536});
    QCOMPARE(history.rows().back().fftSize, std::size_t{4'096});

    QVERIFY(history.append(large));
    QCOMPARE(history.size(), std::size_t{1});
    QVERIFY(history.memoryUsageBytes() <= history.memoryBudgetBytes());
    QCOMPARE(history.rows().front().normalizedMagnitudes.size(),
             std::size_t{65'536});
}

void SpectrumWaterfallItemTest::selectsAdaptiveHistoryBinsAcrossSupportedSettings()
{
    constexpr std::size_t budget = 16U * 1'024U * 1'024U;
    for (const std::size_t fftSize : {
             1'024U, 2'048U, 4'096U, 8'192U,
             16'384U, 32'768U, 65'536U, 131'072U, 262'144U}) {
        for (const double seconds : {5.0, 10.0, 15.0, 30.0, 60.0}) {
            const double sourceCadence =
                sdr::dsp::adaptiveSpectrumFrameRate(seconds);
            const auto plan = sdr::gui::selectWaterfallHistoryPlan(
                fftSize, 640, 600, sourceCadence, seconds, budget);
            QCOMPARE(plan.sourceBins, fftSize);
            QVERIFY(plan.storedBins >= 2);
            QVERIFY(plan.storedBins <= fftSize);
            QVERIFY(plan.minimumStoredBins >= 2);
            QVERIFY(plan.minimumStoredBins <= plan.storedBins);
            QVERIFY(plan.requiredRows > static_cast<std::size_t>(
                                            sourceCadence * seconds));
            QVERIFY(plan.fitsMemoryBudget);
            QVERIFY(plan.requiredMemoryBytes <= budget);
            QCOMPARE(plan.retainedCapacitySeconds, seconds);
        }
    }

    for (const double seconds : {5.0, 10.0, 15.0, 30.0, 60.0}) {
        const auto acceptance = sdr::gui::selectWaterfallHistoryPlan(
            262'144,
            3'840,
            2'160,
            sdr::dsp::adaptiveSpectrumFrameRate(seconds),
            seconds,
            budget);
        QVERIFY(acceptance.fitsMemoryBudget);
        QCOMPARE(acceptance.retainedCapacitySeconds, seconds);
        QVERIFY(acceptance.storedBins >= 2);
        QVERIFY(acceptance.storedBins <= 8'192);
    }
}

void SpectrumWaterfallItemTest::preservesNarrowPeaksInReducedHistory()
{
    sdr::gui::WaterfallHistoryBuffer history(8);
    history.setStoredBinCount(2'048);
    const auto source = carrierFrame(
        100'000'000, 2'400'000, 65'536, 100'012'345, 1, 0, 1);
    QVERIFY(history.append(source));
    QCOMPARE(
        history.rows().front().normalizedMagnitudes.size(),
        std::size_t{2'048});
    QCOMPARE(
        *std::max_element(
            history.rows().front().normalizedMagnitudes.begin(),
            history.rows().front().normalizedMagnitudes.end()),
        std::uint16_t{65'535});
    QCOMPARE(history.rows().front().fftSize, std::size_t{65'536});
}

void SpectrumWaterfallItemTest::
    projectsSharpViewportRowsFromFullFftForBothAggregations()
{
    auto source = carrierFrame(
        100'000'000,
        2'400'000,
        4'096,
        100'000'000,
        1,
        4,
        1'000'000'000ULL);
    constexpr std::size_t physicalWidth = 64;
    const auto viewport = viewportDescriptor(
        source,
        {98'800'000, 101'200'000},
        physicalWidth,
        1.0,
        9);
    const auto highResolution =
        sdr::gui::projectFrameToWaterfallViewport(source, viewport);
    QVERIFY(highResolution.has_value());
    QVERIFY(sdr::gui::viewportWaterfallRowMatches(
        *highResolution, viewport));
    QCOMPARE(
        highResolution->peakMagnitudes.size(),
        physicalWidth);
    QCOMPARE(
        highResolution->meanLinearPowers.size(),
        physicalWidth);

    sdr::gui::WaterfallHistoryBuffer compact(2);
    compact.setStoredBinCount(4);
    QVERIFY(compact.append(source));
    const sdr::radio::FrequencyAxisMapper fullAxis(
        viewport.visibleRange,
        {0.0, static_cast<double>(physicalWidth - 1)});
    const auto compactOriginal =
        sdr::gui::projectWaterfallRowToFrequencyAxis(
            compact.rows().front(),
            fullAxis,
            physicalWidth);
    const auto compactAverage =
        sdr::gui::projectAverageWaterfallRowToFrequencyAxis(
            compact.rows().front(),
            fullAxis,
            physicalWidth);
    const auto highOriginalCount = std::count_if(
        highResolution->peakMagnitudes.begin(),
        highResolution->peakMagnitudes.end(),
        [](std::uint16_t value) { return value > 32'767; });
    const auto compactOriginalCount = std::count_if(
        compactOriginal.begin(),
        compactOriginal.end(),
        [](float value) { return value > 0.5F; });
    QVERIFY(highOriginalCount > 0);
    QVERIFY(highOriginalCount < compactOriginalCount);

    const float highAveragePeak = *std::max_element(
        highResolution->meanLinearPowers.begin(),
        highResolution->meanLinearPowers.end());
    const float compactAveragePeak =
        *std::max_element(compactAverage.begin(), compactAverage.end());
    QVERIFY(highAveragePeak > compactAveragePeak);
}

void SpectrumWaterfallItemTest::strictlyRejectsMismatchedViewportRows()
{
    const auto source = carrierFrame(
        100'000'000,
        2'000'000,
        4'096,
        100'000'000,
        7,
        3,
        5'000'000'000ULL);
    const auto viewport = viewportDescriptor(
        source, {99'500'000, 100'500'000}, 640, 1.25, 11);
    const auto projected =
        sdr::gui::projectFrameToWaterfallViewport(source, viewport);
    QVERIFY(projected.has_value());

    const auto rejects = [&](auto mutate) {
        auto mismatch = viewport;
        mutate(mismatch);
        QVERIFY(!sdr::gui::viewportWaterfallRowMatches(
            *projected, mismatch));
    };
    rejects([](auto& value) { ++value.generation; });
    rejects([](auto& value) { --value.visibleRange.minimum; });
    rejects([](auto& value) { ++value.visibleRange.maximum; });
    rejects([](auto& value) { ++value.physicalWidth; });
    rejects([](auto& value) { value.devicePixelRatio = 2.0; });
    rejects([](auto& value) { ++value.captureCenterFrequency; });
    rejects([](auto& value) { ++value.captureSampleRate; });
    rejects([](auto& value) { ++value.captureSpan; });
    rejects([](auto& value) { value.captureFftSize *= 2; });
    rejects([](auto& value) { ++value.tuningGeneration; });

    auto narrowCapture = source;
    narrowCapture.captureSpan = 400'000;
    const auto uncovered = viewportDescriptor(
        narrowCapture,
        viewport.visibleRange,
        viewport.physicalWidth,
        viewport.devicePixelRatio,
        viewport.generation);
    QVERIFY(!sdr::gui::projectFrameToWaterfallViewport(
        narrowCapture, uncovered).has_value());
}

void SpectrumWaterfallItemTest::boundsViewportHistoryByItsSeparateBudget()
{
    const auto source = carrierFrame(
        100'000'000,
        2'000'000,
        4'096,
        100'000'000,
        1,
        0,
        1);
    const auto viewport = viewportDescriptor(
        source, {99'000'000, 101'000'000}, 64, 1.0);
    const auto row =
        sdr::gui::projectFrameToWaterfallViewport(source, viewport);
    QVERIFY(row.has_value());
    const std::size_t oneRowBytes =
        sizeof(sdr::gui::ViewportWaterfallHistoryRow) +
        row->peakMagnitudes.capacity() * sizeof(std::uint16_t) +
        row->meanLinearPowers.capacity() * sizeof(float);
    sdr::gui::ViewportWaterfallHistoryBuffer history(
        3, oneRowBytes * 2);
    for (std::uint64_t sequence = 1; sequence <= 10; ++sequence) {
        auto next = *row;
        next.sequence = sequence;
        next.timestampNanoseconds = sequence;
        QVERIFY(history.append(std::move(next)));
        QVERIFY(history.size() <= 2);
        QVERIFY(history.memoryUsageBytes() <= history.memoryBudgetBytes());
    }
    QCOMPARE(history.rows().front().sequence, std::uint64_t{10});
    QCOMPARE(history.rows().back().sequence, std::uint64_t{9});

    history.setMemoryBudgetBytes(oneRowBytes);
    QCOMPARE(history.size(), std::size_t{1});
    QCOMPARE(history.rows().front().sequence, std::uint64_t{10});
    QCOMPARE(
        sdr::gui::viewportWaterfallHistoryMemoryBudgetBytes,
        std::size_t{8U * 1'024U * 1'024U});
}

void SpectrumWaterfallItemTest::
    fallsBackAcrossViewportChangesWithoutBlackSidesAndSwapsAtomically()
{
    for (const QString& aggregation :
         {QStringLiteral("original"), QStringLiteral("average")}) {
        ApplicationModel model;
        SpectrumWaterfallItem item;
        item.setSize(QSizeF(64.0, 24.0));
        item.setWaterfall(true);
        item.setApplicationModel(&model);
        item.setWaterfallAggregation(aggregation);
        item.setEffectiveRowsPerSecond(10.0);
        item.setVisibleHistorySeconds(5);

        model.requestWaterfallZoom(240);
        QVERIFY(QTest::qWaitFor(
            [&model] { return model.displayZoomFactor() > 1.0; },
            500));
        item.m_resizeCoalesceTimer.stop();
        item.commitRasterResize();
        QVector<float> populated(4'096, 0.5F);
        populated[2'048] = 1.0F;
        QVERIFY(deliverWaterfallFrame(
            item,
            populated,
            model.centerFrequency(),
            model.effectiveSampleRate(),
            1,
            1'000'000'000ULL));
        const auto geometry = sdr::gui::waterfallRasterGeometry(
            64.0, 24.0, 1.0, 10.0, 5.0);
        QVERIFY(item.rebuildWaterfallImage(
            1'000'000'000ULL, geometry));
        QVERIFY(item.m_lastHighResolutionRasterRows > 0);
        QCOMPARE(item.m_lastCompactRasterRows, std::uint64_t{0});
        const qint64 narrowImageKey = item.m_waterfallImage.cacheKey();
        const std::uint64_t narrowGeneration =
            item.m_completedWaterfallViewportGeneration;
        const quint64 narrowSpan = model.visibleSpan();
        QVERIFY(narrowGeneration > 0);

        model.requestWaterfallZoom(-120);
        QVERIFY(QTest::qWaitFor(
            [&model, narrowSpan] { return model.visibleSpan() > narrowSpan; },
            500));
        item.m_resizeCoalesceTimer.stop();
        item.commitRasterResize();
        QVERIFY(
            item.m_waterfallViewportGeneration > narrowGeneration);
        QCOMPARE(item.m_waterfallImage.cacheKey(), narrowImageKey);
        QCOMPARE(
            item.m_completedWaterfallViewportGeneration,
            narrowGeneration);

        QVERIFY(item.rebuildWaterfallImage(
            1'000'000'000ULL, geometry));
        QCOMPARE(item.m_lastHighResolutionRasterRows, std::uint64_t{0});
        QVERIFY(item.m_lastCompactRasterRows > 0);
        QVERIFY(item.m_waterfallImage.cacheKey() != narrowImageKey);
        QCOMPARE(
            item.m_completedWaterfallViewportGeneration,
            item.m_waterfallViewportGeneration);
        const QRgb empty = item.emptyWaterfallColor();
        for (int rowIndex = 0;
             rowIndex < item.m_waterfallImage.height();
             ++rowIndex) {
            QVERIFY(item.m_waterfallImage.pixel(0, rowIndex) != empty);
            QVERIFY(
                item.m_waterfallImage.pixel(
                    item.m_waterfallImage.width() - 1,
                    rowIndex) != empty);
        }

        QVERIFY(deliverWaterfallFrame(
            item,
            populated,
            model.centerFrequency(),
            model.effectiveSampleRate(),
            2,
            1'500'000'000ULL));
        QVERIFY(item.rebuildWaterfallImage(
            1'500'000'000ULL, geometry));
        QVERIFY(item.m_lastCompactRasterRows > 0);

        model.requestWaterfallZoom(240);
        QVERIFY(QTest::qWaitFor(
            [&model] { return model.displayZoomFactor() > 1.0; },
            500));
        item.m_resizeCoalesceTimer.stop();
        item.commitRasterResize();
        QVERIFY(deliverWaterfallFrame(
            item,
            populated,
            model.centerFrequency(),
            model.effectiveSampleRate(),
            3,
            2'000'000'000ULL));
        QVERIFY(item.rebuildWaterfallImage(
            2'000'000'000ULL, geometry));
        const std::uint64_t beforePan =
            item.m_waterfallViewportGeneration;
        model.handleFrequencyWheel(
            true, 120, Qt::ShiftModifier);
        QVERIFY(item.m_waterfallViewportGeneration > beforePan);

        const std::uint64_t beforeResize =
            item.m_waterfallViewportGeneration;
        item.setSize(QSizeF(80.0, 24.0));
        item.commitRasterResize();
        QVERIFY(item.m_waterfallViewportGeneration > beforeResize);
        QCOMPARE(
            item.m_waterfallViewport.physicalWidth,
            std::size_t{80});

        const std::uint64_t beforeCaptureGeometry =
            item.m_waterfallViewportGeneration;
        QVector<float> differentFftSize(2'048, 0.5F);
        QVERIFY(deliverWaterfallFrame(
            item,
            differentFftSize,
            model.centerFrequency(),
            model.effectiveSampleRate(),
            4,
            2'500'000'000ULL));
        QVERIFY(
            item.m_waterfallViewportGeneration >
            beforeCaptureGeometry);
    }
}

void SpectrumWaterfallItemTest::retainsRequestedDurationAfterWarmup()
{
    constexpr std::uint32_t rowsPerSecond = 60;
    constexpr double seconds = 10.0;
    constexpr std::size_t budget = 16U * 1'024U * 1'024U;
    const auto plan = sdr::gui::selectWaterfallHistoryPlan(
        65'536, 640, 600, rowsPerSecond, seconds, budget);
    QVERIFY(plan.fitsMemoryBudget);

    sdr::gui::WaterfallHistoryBuffer history(plan.requiredRows, budget);
    history.setStoredBinCount(plan.storedBins);
    const std::size_t stagingRows = sdr::gui::waterfallStagingPixelRows(
        600, rowsPerSecond, seconds);
    history.setRetentionDurationSeconds(
        seconds + static_cast<double>(stagingRows) * seconds / 600.0);
    constexpr std::uint64_t interval = 1'000'000'000ULL / rowsPerSecond;
    for (std::size_t row = 0; row < plan.requiredRows; ++row) {
        QVERIFY(history.append(carrierFrame(
            100'000'000,
            2'400'000,
            65'536,
            100'000'000,
            row + 1,
            0,
            1'000'000'000ULL + row * interval)));
    }
    QVERIFY(history.memoryUsageBytes() <= budget);
    QVERIFY2(
        history.retainedDurationSeconds() >= seconds,
        qPrintable(QStringLiteral(
            "retained=%1 rows=%2 capacity=%3 stored=%4 bytes=%5 plan-bytes=%6")
                       .arg(history.retainedDurationSeconds(), 0, 'f', 9)
                       .arg(history.size())
                       .arg(plan.maximumRowsWithinBudget)
                       .arg(plan.storedBins)
                       .arg(history.memoryUsageBytes())
                       .arg(plan.requiredMemoryBytes)));
    QCOMPARE(
        std::min(seconds, history.retainedDurationSeconds()),
        seconds);

    const auto lowerRatePlan = sdr::gui::selectWaterfallHistoryPlan(
        65'536, 640, 600, 30, seconds, budget);
    history.setCapacity(std::max(
        lowerRatePlan.maximumRowsWithinBudget, history.size()));
    QVERIFY(history.retainedDurationSeconds() >= seconds);
}

void SpectrumWaterfallItemTest::boundsHistoryByTimestampDuration()
{
    sdr::gui::WaterfallHistoryBuffer history(1'000);
    history.setRetentionDurationSeconds(2.0);
    QVERIFY(history.append(carrierFrame(
        100'000, 1'000, 1'024, 100'000, 1, 0, 1'000'000'000ULL)));
    QVERIFY(history.append(carrierFrame(
        100'000, 1'000, 1'024, 100'000, 2, 0, 4'000'000'000ULL)));
    QVERIFY(history.append(carrierFrame(
        100'000, 1'000, 1'024, 100'000, 3, 0, 5'000'000'000ULL)));
    QCOMPARE(history.size(), std::size_t{2});
    QCOMPARE(history.rows().front().timestampNanoseconds, 5'000'000'000ULL);
    QCOMPARE(history.rows().back().timestampNanoseconds, 4'000'000'000ULL);
    QCOMPARE(history.retainedDurationSeconds(), 1.0);

    history.setRetentionDurationSeconds(0.5);
    QCOMPARE(history.size(), std::size_t{1});
}

void SpectrumWaterfallItemTest::mapsTimestampedRowsToVerticalPixels()
{
    std::deque<sdr::gui::WaterfallHistoryRow> rows;
    for (const auto timestamp : {
             10'000'000'000ULL,
             9'750'000'000ULL,
             9'500'000'000ULL,
             9'250'000'000ULL}) {
        sdr::gui::WaterfallHistoryRow row;
        row.timestampNanoseconds = timestamp;
        rows.push_back(std::move(row));
    }
    const auto reduced = sdr::gui::mapWaterfallRowsToPixels(
        rows, 10'000'000'000ULL, 4.0, 4);
    QCOMPARE(reduced.size(), std::size_t{4});
    QVERIFY(reduced[0].hasData);
    QVERIFY(reduced[0].reduce);
    QCOMPARE(reduced[0].firstRow, std::size_t{0});
    QCOMPARE(reduced[0].lastRow, std::size_t{3});

    std::deque<sdr::gui::WaterfallHistoryRow> sparse(2);
    sparse[0].timestampNanoseconds = 10'000'000'000ULL;
    sparse[1].timestampNanoseconds = 9'000'000'000ULL;
    const auto expanded = sdr::gui::mapWaterfallRowsToPixels(
        sparse, 10'000'000'000ULL, 4.0, 8);
    QVERIFY(expanded[1].hasData);
    QVERIFY(expanded[1].interpolate);
    QVERIFY(!expanded[1].reduce);
    QCOMPARE(expanded[1].firstRow, std::size_t{1});
    QCOMPARE(expanded[1].newerRow, std::size_t{0});
    QCOMPARE(expanded[1].olderRow, std::size_t{1});
    QVERIFY(std::abs(expanded[1].interpolation - 0.75F) < 0.0001F);

    for (const std::size_t pixelRows : {7U, 31U, 127U}) {
        for (const double seconds : {5.0, 15.0, 60.0}) {
            constexpr std::uint64_t anchor = 120'000'000'000ULL;
            const double nanosecondsPerPixel =
                seconds * 1.0e9 / static_cast<double>(pixelRows);
            std::deque<sdr::gui::WaterfallHistoryRow> edgeRows;
            for (std::size_t pixel = 0; pixel < pixelRows; ++pixel) {
                sdr::gui::WaterfallHistoryRow row;
                row.timestampNanoseconds =
                    anchor - static_cast<std::uint64_t>(std::llround(
                                 (static_cast<double>(pixel) + 0.5) *
                                 nanosecondsPerPixel));
                edgeRows.push_back(std::move(row));
            }
            const auto edgeMapping = sdr::gui::mapWaterfallRowsToPixels(
                edgeRows, anchor, seconds, pixelRows);
            QCOMPARE(edgeMapping.size(), pixelRows);
            QVERIFY(std::all_of(
                edgeMapping.begin(),
                edgeMapping.end(),
                [](const auto& sample) { return sample.hasData; }));
            QCOMPARE(edgeMapping.front().firstRow, std::size_t{0});
            QCOMPARE(edgeMapping.back().firstRow, pixelRows - 1);
        }
    }
}

void SpectrumWaterfallItemTest::
    showsNewStepsWithoutVisibleHistoryStartupDelay()
{
    std::deque<sdr::gui::WaterfallHistoryRow> rows(2);
    rows[0].sequence = 2;
    rows[0].timestampNanoseconds = 1'000'000'000ULL;
    rows[0].centerFrequency = 100'000'000;
    rows[0].sampleRate = 2'000'000;
    rows[0].captureSpan = 2'000'000;
    rows[0].fftSize = 4'096;
    rows[0].tuningGeneration = 1;
    rows[1] = rows[0];
    rows[1].sequence = 1;
    rows[1].timestampNanoseconds = 920'000'000ULL;

    for (const double visibleSeconds : {1.0, 2.5}) {
        const auto mapping = sdr::gui::mapWaterfallRowsToPixels(
            rows,
            rows.front().timestampNanoseconds,
            visibleSeconds,
            100);
        QVERIFY(mapping.front().hasData);
        QCOMPARE(mapping.front().firstRow, std::size_t{0});
        QVERIFY(!mapping.front().interpolate);
        QVERIFY(!mapping.front().reduce);
    }
}

void SpectrumWaterfallItemTest::weightsAverageSmoothingByElapsedTime()
{
    std::deque<sdr::gui::WaterfallHistoryRow> rows(3);
    rows[0].timestampNanoseconds = 1'000'000'000ULL;
    rows[1].timestampNanoseconds = 900'000'000ULL;
    rows[2].timestampNanoseconds = 500'000'000ULL;
    for (auto& row : rows) {
        row.centerFrequency = 100'000'000;
        row.sampleRate = 2'000'000;
        row.captureSpan = 2'000'000;
        row.fftSize = 4'096;
        row.tuningGeneration = 1;
    }

    const double first =
        sdr::gui::waterfallTemporalWeightNanoseconds(
            rows, 0, 1'000'000'000ULL, 0.0, 500'000'000.0);
    const double second =
        sdr::gui::waterfallTemporalWeightNanoseconds(
            rows, 1, 1'000'000'000ULL, 0.0, 500'000'000.0);
    const double third =
        sdr::gui::waterfallTemporalWeightNanoseconds(
            rows, 2, 1'000'000'000ULL, 0.0, 500'000'000.0);
    QCOMPARE(first, 50'000'000.0);
    QCOMPARE(second, 250'000'000.0);
    QCOMPARE(third, 200'000'000.0);
    QCOMPARE(first + second + third, 500'000'000.0);

    const double weightedStep =
        (first + second) / (first + second + third);
    QVERIFY(std::abs(weightedStep - 0.6) < 1.0e-12);
    QVERIFY(std::abs(weightedStep - (2.0 / 3.0)) > 0.05);
}

void SpectrumWaterfallItemTest::doesNotBlendDifferentTuningGenerations()
{
    std::deque<sdr::gui::WaterfallHistoryRow> rows(3);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        rows[index].sequence = 3 - index;
        rows[index].timestampNanoseconds =
            1'000'000'000ULL - index * 50'000'000ULL;
        rows[index].centerFrequency =
            index == 0 ? 101'000'000 : 100'000'000;
        rows[index].sampleRate = 2'000'000;
        rows[index].captureSpan = 2'000'000;
        rows[index].fftSize = 4'096;
        rows[index].tuningGeneration = index == 0 ? 2 : 1;
    }

    const auto mapping = sdr::gui::mapWaterfallRowsToPixels(
        rows, 1'000'000'000ULL, 1.0, 1);
    QVERIFY(mapping.front().hasData);
    QCOMPARE(mapping.front().firstRow, std::size_t{0});
    QCOMPARE(mapping.front().lastRow, std::size_t{0});
    QVERIFY(!mapping.front().reduce);
    QVERIFY(!mapping.front().interpolate);
}

void SpectrumWaterfallItemTest::scrollsByStableFractionalPixels()
{
    QCOMPARE(
        sdr::gui::waterfallFractionalScrollPixels(16'666'667ULL, 10.0, 600.0),
        1.00000002);
    const double first = sdr::gui::waterfallFractionalScrollPixels(
        8'000'000ULL, 15.0, 500.0);
    const double second = sdr::gui::waterfallFractionalScrollPixels(
        16'000'000ULL, 15.0, 500.0);
    QVERIFY(first > 0.0 && first < 1.0);
    QVERIFY(std::abs(second - first * 2.0) < 1.0e-9);
}

void SpectrumWaterfallItemTest::buildsPixelNativeRasterGeometryAcrossDurationsAndDpr()
{
    for (const double logicalHeight : {240.0, 600.0, 937.25}) {
        for (const double devicePixelRatio : {1.0, 1.25, 2.0}) {
            for (const double seconds : {5.0, 10.0, 15.0, 30.0, 60.0}) {
                const double sourceCadence =
                    sdr::dsp::adaptiveSpectrumFrameRate(seconds);
                const auto geometry = sdr::gui::waterfallRasterGeometry(
                    640.25,
                    logicalHeight,
                    devicePixelRatio,
                    sourceCadence,
                    seconds);
                QCOMPARE(
                    geometry.physicalWidth,
                    static_cast<std::size_t>(
                        std::ceil(640.25 * devicePixelRatio)));
                QCOMPARE(
                    geometry.visiblePixelRows,
                    static_cast<std::size_t>(
                        std::ceil(logicalHeight * devicePixelRatio)));
                QVERIFY(geometry.stagingPixelRows > 0);
                const auto overscanDuration =
                    sdr::gui::waterfallStagingDurationNanoseconds(
                        geometry.visiblePixelRows,
                        geometry.stagingPixelRows,
                        seconds);
                QVERIFY(overscanDuration >= 100'000'000ULL);
                QVERIFY(
                    static_cast<double>(overscanDuration) + 1.0 >=
                    2.0e9 / sourceCadence);
            }
        }
    }
}

void SpectrumWaterfallItemTest::preservesMonotonicRasterPhaseAcrossResize()
{
    constexpr std::uint64_t newestRowTimestamp = 20'000'000'000ULL;
    constexpr std::uint64_t renderClockOrigin = 50'000'000'000ULL;
    constexpr std::uint64_t rebuildInterval = 33'000'000ULL;

    for (const std::size_t physicalRows : {300U, 600U, 1'200U}) {
        for (const double seconds : {5.0, 10.0, 15.0, 30.0, 60.0}) {
            const double sourceCadence =
                sdr::dsp::adaptiveSpectrumFrameRate(seconds);
            const std::size_t stagingRows =
                sdr::gui::waterfallStagingPixelRows(
                    physicalRows, sourceCadence, seconds);
            const std::uint64_t stagingDuration =
                sdr::gui::waterfallStagingDurationNanoseconds(
                    physicalRows, stagingRows, seconds);
            QVERIFY(stagingDuration >= 100'000'000ULL);
            QVERIFY(
                static_cast<double>(stagingDuration) + 1.0 >=
                2.0e9 / sourceCadence);

            const std::uint64_t initialRenderTimestamp = newestRowTimestamp;
            const std::uint64_t firstRenderTimestamp =
                sdr::gui::waterfallRenderTimestamp(
                    initialRenderTimestamp,
                    renderClockOrigin,
                    renderClockOrigin);
            const std::uint64_t secondRenderTimestamp =
                sdr::gui::waterfallRenderTimestamp(
                    initialRenderTimestamp,
                    renderClockOrigin,
                    renderClockOrigin + rebuildInterval);
            QCOMPARE(
                secondRenderTimestamp - firstRenderTimestamp,
                rebuildInterval);

            const double pixelsPerNanosecond =
                static_cast<double>(physicalRows) / (seconds * 1.0e9);
            const std::uint64_t retainedRowTimestamp =
                initialRenderTimestamp - 250'000'000ULL;
            const double beforeRebuildY =
                static_cast<double>(
                    firstRenderTimestamp - retainedRowTimestamp) *
                pixelsPerNanosecond;
            const double afterRebuildY =
                static_cast<double>(
                    secondRenderTimestamp - retainedRowTimestamp) *
                pixelsPerNanosecond;
            QCOMPARE(
                afterRebuildY - beforeRebuildY,
                static_cast<double>(rebuildInterval) * pixelsPerNanosecond);

            const auto geometry = sdr::gui::waterfallRasterGeometry(
                640.0,
                static_cast<double>(physicalRows) / 2.0,
                2.0,
                sourceCadence,
                seconds);
            QCOMPARE(geometry.visiblePixelRows, physicalRows);
            QCOMPARE(geometry.stagingPixelRows, stagingRows);
        }
    }
}

void SpectrumWaterfallItemTest::mapsEveryVisiblePixelRowAcrossDurations()
{
    constexpr std::size_t visibleRows = 600;
    constexpr std::uint64_t firstSourceTimestamp = 120'000'000'000ULL;
    constexpr std::uint64_t sourceInterval = 109'226'667ULL;
    constexpr std::uint64_t rasterInterval = 16'000'000ULL;

    for (const double seconds : {5.0, 10.0, 30.0, 60.0}) {
        std::deque<sdr::gui::WaterfallHistoryRow> rows(1);
        rows.front().timestampNanoseconds = firstSourceTimestamp;
        std::uint64_t nextSourceTimestamp =
            firstSourceTimestamp + sourceInterval;

        for (std::uint64_t tick = 0; tick < 32; ++tick) {
            const std::uint64_t rasterTimestamp =
                firstSourceTimestamp + tick * rasterInterval;
            while (nextSourceTimestamp <= rasterTimestamp) {
                sdr::gui::WaterfallHistoryRow row;
                row.timestampNanoseconds = nextSourceTimestamp;
                rows.push_front(std::move(row));
                nextSourceTimestamp += sourceInterval;
            }

            const std::uint64_t renderTimestamp =
                sdr::gui::clampWaterfallRenderTimestamp(
                    rasterTimestamp, rows);
            QVERIFY(renderTimestamp >= rows.front().timestampNanoseconds);
            QVERIFY(
                renderTimestamp - rows.front().timestampNanoseconds <=
                sourceInterval);

            const auto mapping = sdr::gui::mapWaterfallRowsToPixels(
                rows, renderTimestamp, seconds, visibleRows);
            QCOMPARE(mapping.size(), visibleRows);
            QVERIFY(std::all_of(
                mapping.begin(),
                mapping.end(),
                [](const auto& sample) { return sample.hasData; }));
        }

        const std::uint64_t predictedFarAhead =
            rows.front().timestampNanoseconds + 5 * sourceInterval;
        QCOMPARE(
            sdr::gui::clampWaterfallRenderTimestamp(
                predictedFarAhead, rows),
            rows.front().timestampNanoseconds + sourceInterval);
        const auto heldMapping = sdr::gui::mapWaterfallRowsToPixels(
            rows,
            rows.front().timestampNanoseconds + sourceInterval,
            seconds,
            visibleRows);
        QVERIFY(heldMapping.front().hasData);
        QVERIFY(!heldMapping.front().interpolate);
        QVERIFY(!heldMapping.front().reduce);
        QCOMPARE(heldMapping.front().firstRow, std::size_t{0});
        QVERIFY(heldMapping.back().hasData);
        QCOMPARE(heldMapping.back().firstRow, rows.size() - 1);
    }
}

void SpectrumWaterfallItemTest::
    rendersEveryPhysicalWaterfallRowAcrossHistoryChanges()
{
    constexpr quint64 centerFrequency = 100'000'000;
    constexpr quint64 sampleRate = 2'400'000;
    constexpr quint64 firstTimestamp = 120'000'000'000ULL;
    constexpr quint64 rowInterval = 109'226'667ULL;
    constexpr double rowsPerSecond =
        1'000'000'000.0 / static_cast<double>(rowInterval);
    const QVector<float> populatedFrame(8, 0.8F);
    const auto feedHistory = [&](SpectrumWaterfallItem& item,
                                 quint64 firstSequence,
                                 quint64 firstFrameTimestamp,
                                 std::size_t rowCount,
                                 const QVector<float>& frame) {
        for (std::size_t row = 0; row < rowCount; ++row) {
            QVERIFY(deliverWaterfallFrame(
                item,
                frame,
                centerFrequency,
                sampleRate,
                firstSequence + row,
                firstFrameTimestamp + row * rowInterval));
        }
    };
    const auto verifyFullyPopulated =
        [](const SpectrumWaterfallItem& item,
           const sdr::gui::WaterfallRasterGeometry& geometry) {
            QCOMPARE(
                item.m_waterfallImage.size(),
                QSize(
                    static_cast<int>(geometry.physicalWidth),
                    static_cast<int>(geometry.visiblePixelRows)));
            const QRgb emptyColor = item.emptyWaterfallColor();
            for (int row = 0; row < item.m_waterfallImage.height(); ++row) {
                QCOMPARE(qAlpha(item.m_waterfallImage.pixel(0, row)), 255);
                QVERIFY(item.m_waterfallImage.pixel(0, row) != emptyColor);
            }
        };

    for (const quint32 seconds : {5U, 10U, 30U, 60U}) {
        SpectrumWaterfallItem item;
        item.setWaterfall(true);
        item.setEffectiveRowsPerSecond(rowsPerSecond);
        item.setVisibleHistorySeconds(seconds);
        item.setSize(QSizeF(8.25, 63.5));
        const std::size_t rowCount = static_cast<std::size_t>(
                                         std::ceil(
                                             static_cast<double>(seconds) *
                                             rowsPerSecond)) +
                                     1;
        feedHistory(item, 1, firstTimestamp, rowCount, populatedFrame);
        const quint64 newestTimestamp =
            firstTimestamp + (rowCount - 1) * rowInterval;

        for (const double logicalHeight : {13.25, 37.0, 63.5}) {
            for (const double devicePixelRatio : {1.0, 1.25, 2.0}) {
                const auto geometry = sdr::gui::waterfallRasterGeometry(
                    8.25,
                    logicalHeight,
                    devicePixelRatio,
                    rowsPerSecond,
                    static_cast<double>(seconds));
                QVERIFY(item.rebuildWaterfallImage(
                    sdr::gui::clampWaterfallRenderTimestamp(
                        newestTimestamp + 16'000'000ULL,
                        item.m_waterfallHistory.rows()),
                    geometry));
                verifyFullyPopulated(item, geometry);
            }
        }
    }

    SpectrumWaterfallItem changing;
    changing.setWaterfall(true);
    changing.setEffectiveRowsPerSecond(rowsPerSecond);
    changing.setSize(QSizeF(8.0, 40.0));
    changing.setVisibleHistorySeconds(60);
    const std::size_t sixtySecondRows = static_cast<std::size_t>(
                                           std::ceil(60.0 * rowsPerSecond)) +
                                       1;
    feedHistory(
        changing,
        1,
        firstTimestamp,
        sixtySecondRows,
        populatedFrame);
    const quint64 newestTimestamp =
        firstTimestamp + (sixtySecondRows - 1) * rowInterval;
    const std::size_t retainedRows = changing.m_waterfallHistory.size();
    const quint64 oldestTimestamp =
        changing.m_waterfallHistory.rows().back().timestampNanoseconds;

    changing.setVisibleHistorySeconds(5);
    QCOMPARE(changing.m_retainedHistoryDurationSeconds, 60.0);
    QCOMPARE(changing.m_waterfallHistory.size(), retainedRows);
    QVERIFY(changing.retainedHistorySeconds() > 59.0);
    QCOMPARE(
        changing.m_waterfallHistory.rows().back().timestampNanoseconds,
        oldestTimestamp);
    const auto reducedGeometry = sdr::gui::waterfallRasterGeometry(
        8.0, 40.0, 1.0, rowsPerSecond, 5.0);
    QCOMPARE(
        changing.m_rasterGeometry.physicalWidth,
        reducedGeometry.physicalWidth);
    QCOMPARE(
        changing.m_rasterGeometry.visiblePixelRows,
        reducedGeometry.visiblePixelRows);
    QCOMPARE(
        changing.m_rasterGeometry.stagingPixelRows,
        reducedGeometry.stagingPixelRows);

    changing.setVisibleHistorySeconds(60);
    QCOMPARE(changing.m_waterfallHistory.size(), retainedRows);
    QCOMPARE(
        changing.m_waterfallHistory.rows().back().timestampNanoseconds,
        oldestTimestamp);
    const auto restoredGeometry = sdr::gui::waterfallRasterGeometry(
        8.0, 40.0, 1.0, rowsPerSecond, 60.0);
    QVERIFY(changing.rebuildWaterfallImage(
        newestTimestamp, restoredGeometry));
    verifyFullyPopulated(changing, restoredGeometry);

    SpectrumWaterfallItem warming;
    warming.setWaterfall(true);
    warming.setEffectiveRowsPerSecond(rowsPerSecond);
    warming.setSize(QSizeF(8.0, 40.0));
    warming.setVisibleHistorySeconds(5);
    const QVector<float> oldestFrame(8, 0.25F);
    feedHistory(warming, 1, firstTimestamp, 1, oldestFrame);
    feedHistory(
        warming,
        2,
        firstTimestamp + rowInterval,
        1,
        populatedFrame);
    warming.setVisibleHistorySeconds(60);
    const auto warmupGeometry = sdr::gui::waterfallRasterGeometry(
        8.0, 40.0, 1.0, rowsPerSecond, 60.0);
    QVERIFY(warming.rebuildWaterfallImage(
        firstTimestamp + rowInterval, warmupGeometry));
    verifyFullyPopulated(warming, warmupGeometry);
    QCOMPARE(
        warming.m_waterfallImage.pixel(
            0, warming.m_waterfallImage.height() - 1),
        warming.waterfallColorForNormalizedMagnitude(0.25F));

    warming.clearWaterfallFrames();
    QCOMPARE(warming.m_retainedHistoryDurationSeconds, 60.0);
    warming.setVisibleHistorySeconds(5);
    QCOMPARE(warming.m_retainedHistoryDurationSeconds, 5.0);
}

void SpectrumWaterfallItemTest::recalculatesForResizeAndHighDpiWithoutResamplingRows()
{
    constexpr std::size_t budget = 16U * 1'024U * 1'024U;
    const auto normal = sdr::gui::selectWaterfallHistoryPlan(
        65'536, 640, 600, 60, 10.0, budget);
    const auto highDpi = sdr::gui::selectWaterfallHistoryPlan(
        65'536, 1'281, 1'200, 60, 10.0, budget);
    const auto resized = sdr::gui::selectWaterfallHistoryPlan(
        65'536, 1'920, 1'200, 60, 10.0, budget);
    QVERIFY(highDpi.minimumStoredBins > normal.minimumStoredBins);
    QVERIFY(resized.minimumStoredBins > highDpi.minimumStoredBins);
    QVERIFY(normal.fitsMemoryBudget);
    QVERIFY(highDpi.fitsMemoryBudget);
    QVERIFY(resized.fitsMemoryBudget);

    sdr::gui::WaterfallHistoryBuffer history(8, budget);
    history.setStoredBinCount(normal.minimumStoredBins);
    QVERIFY(history.append(carrierFrame(
        100'000, 1'000, 65'536, 100'000, 1, 0, 1)));
    const std::size_t firstStored =
        history.rows().front().normalizedMagnitudes.size();
    history.setStoredBinCount(resized.minimumStoredBins);
    QVERIFY(history.append(carrierFrame(
        100'000, 1'000, 65'536, 100'000, 2, 0, 2)));
    QCOMPARE(
        history.rows().back().normalizedMagnitudes.size(),
        firstStored);
    QCOMPARE(
        history.rows().front().normalizedMagnitudes.size(),
        resized.minimumStoredBins);
}

void SpectrumWaterfallItemTest::reprojectsReducedHistoryAcrossRetunes()
{
    constexpr std::uint64_t carrier = 100'123'456;
    sdr::gui::WaterfallHistoryBuffer history(8);
    history.setStoredBinCount(2'048);
    QVERIFY(history.append(carrierFrame(
        100'000'000, 2'400'000, 65'536, carrier, 1, 3, 1)));
    const auto& stored = history.rows().front();
    QCOMPARE(stored.centerFrequency, std::uint64_t{100'000'000});
    QCOMPARE(stored.captureSpan, std::uint64_t{2'400'000});
    QCOMPARE(stored.tuningGeneration, std::uint64_t{3});
    QCOMPARE(stored.fftSize, std::size_t{65'536});

    const sdr::radio::FrequencyAxisMapper retuned(
        100'200'000, 2'400'000, {0.0, 639.0});
    const auto projected = sdr::gui::projectFrameToFrequencyAxis(
        stored, retuned, 640);
    QVERIFY(!projected.empty());
    const auto expected = retuned.positionForFrequency(
        static_cast<double>(carrier));
    QVERIFY(expected.has_value());
    QVERIFY(std::abs(
        static_cast<double>(peakIndex(projected)) - *expected) <= 1.0);
}

void SpectrumWaterfallItemTest::sharesTheFullWidthPlotAcrossSpectrumWaterfallAndOverlay()
{
    QCOMPARE(
        sdr::gui::displayColumnCountForWidth(640.0, 1.0),
        std::size_t{640});
    QCOMPARE(
        sdr::gui::displayColumnCountForWidth(640.25, 2.0),
        std::size_t{1'281});

    SpectrumWaterfallItem item;
    item.setSize(QSizeF(640.0, 240.0));
    QCOMPARE(item.frequencyPlotRect(), QRectF(0.0, 0.0, 640.0, 240.0));
    QCOMPARE(item.recommendedAmplitudeScaleMargin(640.0F, 2.0F), 48.0F);

    QVector<float> frame(101, 0.0F);
    frame[50] = 1.0F;
    QVERIFY(QMetaObject::invokeMethod(
        &item,
        "receiveFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, frame)));
    QCOMPARE(item.xForFrequency(100'000'000), 320.0F);
}

void SpectrumWaterfallItemTest::sharesZoomedFrequencyAxisBetweenSpectrumAndWaterfall()
{
    ApplicationModel model;
    SpectrumWaterfallItem spectrum;
    SpectrumWaterfallItem waterfall;
    spectrum.setSize(QSizeF(640.0, 240.0));
    waterfall.setSize(QSizeF(640.0, 240.0));
    waterfall.setWaterfall(true);
    spectrum.setApplicationModel(&model);
    waterfall.setApplicationModel(&model);

    model.selectListeningFrequencyAt(480.0, 640.0);
    const float listeningX = waterfall.xForFrequency(model.listeningFrequency());
    model.requestWaterfallZoom(120);
    QElapsedTimer timer;
    timer.start();
    while (model.displayZoomFactor() <= 1.0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }

    QCOMPARE(
        spectrum.xForFrequency(model.listeningFrequency()),
        waterfall.xForFrequency(model.listeningFrequency()));
    QVERIFY(std::abs(
        waterfall.xForFrequency(model.listeningFrequency()) - listeningX) < 0.01F);
    QCOMPARE(
        spectrum.xForFrequency(model.filterLowerFrequency()),
        waterfall.xForFrequency(model.filterLowerFrequency()));
    QCOMPARE(
        spectrum.xForFrequency(model.filterUpperFrequency()),
        waterfall.xForFrequency(model.filterUpperFrequency()));

    const quint64 beforeListening = model.listeningFrequency();
    const quint64 beforeCenter = model.centerFrequency();
    const quint64 beforeSpan = model.visibleSpan();
    model.handleFrequencyWheel(false, 120, Qt::ShiftModifier);
    QCOMPARE(model.listeningFrequency(), beforeListening + 10'000);
    QCOMPARE(model.centerFrequency(), beforeCenter);
    QCOMPARE(model.visibleSpan(), beforeSpan);
    QVERIFY(model.visibleLowerFrequency() <= model.listeningFrequency());
    QVERIFY(model.visibleUpperFrequency() >= model.listeningFrequency());
    QCOMPARE(model.visibleUpperFrequency(), quint64{101'000'000});
    QCOMPARE(
        spectrum.xForFrequency(model.listeningFrequency()),
        waterfall.xForFrequency(model.listeningFrequency()));
    QCOMPARE(
        spectrum.xForFrequency(model.filterLowerFrequency()),
        waterfall.xForFrequency(model.filterLowerFrequency()));
    model.handleFrequencyWheel(true, -120, Qt::ShiftModifier);
    QCOMPARE(model.listeningFrequency(), beforeListening);
    QCOMPARE(model.centerFrequency(), beforeCenter);
    QVERIFY(model.visibleLowerFrequency() <= model.listeningFrequency());
    QVERIFY(model.visibleUpperFrequency() >= model.listeningFrequency());
    QCOMPARE(
        spectrum.xForFrequency(model.listeningFrequency()),
        waterfall.xForFrequency(model.listeningFrequency()));
}

void SpectrumWaterfallItemTest::coalescesRapidAxisChangesIntoOneRenderedProjection()
{
    QQuickWindow window;
    window.resize(640, 240);
    auto* item = new SpectrumWaterfallItem(window.contentItem());
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(640.0, 240.0));
    item->setWaterfall(true);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVector<float> magnitudes(101, 0.1F);
    magnitudes[50] = 1.0F;
    QVERIFY(QMetaObject::invokeMethod(
        item,
        "receiveFrame",
        Qt::DirectConnection,
        Q_ARG(QVector<float>, magnitudes)));
    const auto waitForProjectionCount = [item](std::uint64_t expected) {
        QElapsedTimer timer;
        timer.start();
        while (item->waterfallReprojectionCount() < expected &&
               timer.elapsed() < 1'000) {
            QCoreApplication::processEvents();
            QTest::qWait(5);
        }
        return item->waterfallReprojectionCount() == expected;
    };
    QVERIFY(waitForProjectionCount(1));
    const auto before = item->waterfallReprojectionCount();

    QVERIFY(QMetaObject::invokeMethod(
        item, "frequencyAxisChanged", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        item, "frequencyAxisChanged", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        item, "frequencyAxisChanged", Qt::DirectConnection));
    QVERIFY(waitForProjectionCount(before + 1));
    window.close();
}

void SpectrumWaterfallItemTest::replacesOwnedTexturesSafely()
{
    QQuickWindow window;
    window.resize(640, 240);

    auto* item = new SpectrumWaterfallItem(window.contentItem());
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(640.0, 240.0));
    item->setWaterfall(true);

    QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QVector<float> magnitudes(2'048);
    for (int frame = 0; frame < 20; ++frame) {
        for (qsizetype index = 0; index < magnitudes.size(); ++index) {
            magnitudes[index] = static_cast<float>(
                (index + static_cast<qsizetype>(frame)) % magnitudes.size()) /
                                static_cast<float>(magnitudes.size() - 1);
        }
        QVERIFY(QMetaObject::invokeMethod(
            item,
            "receiveFrame",
            Qt::DirectConnection,
            Q_ARG(QVector<float>, magnitudes)));
        if (frame == 10) {
            item->setWaterfallMinimumDbfs(-105.0F);
            item->setWaterfallMaximumDbfs(-35.0F);
        }
        if (frames.isEmpty()) {
            QVERIFY(frames.wait(1'000));
        }
        frames.clear();
    }

    const auto retainedHistoryBytes = item->historyMemoryUsageBytes();
    const auto beforeResize = item->waterfallReprojectionCount();
    for (const QSize size : {
             QSize(680, 260),
             QSize(760, 320),
             QSize(720, 280),
             QSize(900, 420),
             QSize(640, 240)}) {
        window.resize(size);
        item->setSize(size);
        QCoreApplication::processEvents();
    }
    QElapsedTimer resizeTimer;
    resizeTimer.start();
    while (item->waterfallReprojectionCount() <= beforeResize &&
           resizeTimer.elapsed() < 1'000) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QVERIFY(item->waterfallReprojectionCount() > beforeResize);
    QCOMPARE(item->historyMemoryUsageBytes(), retainedHistoryBytes);

    window.close();
}

void SpectrumWaterfallItemTest::drawsExactSlimFilterGateAtMultipleDprValues()
{
    const sdr::radio::FrequencyAxisMapper axis(100'000, 1'000, {0.0, 640.0});
    for (const float dpr : {1.0F, 1.5F, 2.0F}) {
        const auto gate = sdr::gui::filterGate(
            axis, 99'900, 100'000, 100'100, 240.0F, dpr);
        QCOMPARE(gate.mode, sdr::gui::FilterGateMode::Wide);
        QCOMPARE(gate.edgeLinePattern, sdr::gui::FilterGateLinePattern::Full);
        QVERIFY(std::abs(gate.edgeLineOpacity - 0.65F) < 0.001F);
        QVERIFY(gate.physicalPassbandWidth >= 12.0F);
        QCOMPARE(gate.lowerEdge.centerX, 256.0);
        QCOMPARE(gate.upperEdge.centerX, 384.0);
        QVERIFY(std::abs(gate.lowerEdge.width * dpr - 1.0F) < 0.001F);
        QVERIFY(std::abs(gate.upperEdge.width * dpr - 1.0F) < 0.001F);
        QCOMPARE(gate.markers[0].tipX, gate.lowerEdge.centerX);
        QCOMPARE(gate.markers[1].tipX, gate.upperEdge.centerX);
        QCOMPARE(gate.markers[2].tipX, gate.lowerEdge.centerX);
        QCOMPARE(gate.markers[3].tipX, gate.upperEdge.centerX);
        QVERIFY(gate.markers[0].pointsDown);
        QVERIFY(!gate.markers[2].pointsDown);
    }
}

void SpectrumWaterfallItemTest::adaptsFilterGateLinesToPhysicalPassbandWidth()
{
    const sdr::radio::FrequencyAxisMapper axis(100'200, 1'000, {0.0, 1'200.0});
    const auto narrow = sdr::gui::filterGate(
        axis, 100'199, 100'200, 100'201, 120.0F, 1.5F);
    const auto medium = sdr::gui::filterGate(
        axis, 100'197, 100'200, 100'203, 120.0F, 1.5F);
    const auto wide = sdr::gui::filterGate(
        axis, 100'195, 100'200, 100'205, 120.0F, 1.5F);

    QCOMPARE(narrow.mode, sdr::gui::FilterGateMode::Narrow);
    QCOMPARE(narrow.edgeLinePattern, sdr::gui::FilterGateLinePattern::Stubs);
    QVERIFY(std::abs(narrow.edgeLineOpacity - 0.65F) < 0.001F);
    QVERIFY(narrow.physicalPassbandWidth < 5.0F);
    QCOMPARE(medium.mode, sdr::gui::FilterGateMode::Medium);
    QCOMPARE(medium.edgeLinePattern, sdr::gui::FilterGateLinePattern::Dashed);
    QVERIFY(std::abs(medium.edgeLineOpacity - 0.35F) < 0.001F);
    QVERIFY(medium.physicalPassbandWidth >= 5.0F);
    QVERIFY(medium.physicalPassbandWidth < 12.0F);
    QCOMPARE(wide.mode, sdr::gui::FilterGateMode::Wide);
    QCOMPARE(wide.edgeLinePattern, sdr::gui::FilterGateLinePattern::Full);
    QVERIFY(wide.physicalPassbandWidth >= 12.0F);

    for (const auto* gate : {&narrow, &medium, &wide}) {
        QCOMPARE(gate->markers[0].tipX, gate->lowerEdge.centerX);
        QCOMPARE(gate->markers[1].tipX, gate->upperEdge.centerX);
        QCOMPARE(gate->markers[2].tipX, gate->lowerEdge.centerX);
        QCOMPARE(gate->markers[3].tipX, gate->upperEdge.centerX);
        QVERIFY(std::abs(gate->lowerEdge.width * 1.5F - 1.0F) < 0.001F);
        QVERIFY(std::abs(gate->upperEdge.width * 1.5F - 1.0F) < 0.001F);
    }
}

void SpectrumWaterfallItemTest::revealsFullLinesDuringActiveWidthAdjustment()
{
    const sdr::radio::FrequencyAxisMapper axis(100'200, 1'000, {0.0, 1'200.0});
    const auto gate = sdr::gui::filterGate(
        axis, 100'199, 100'200, 100'201, 120.0F, 2.0F, true);
    QCOMPARE(gate.mode, sdr::gui::FilterGateMode::Narrow);
    QCOMPARE(gate.edgeLinePattern, sdr::gui::FilterGateLinePattern::Full);
    QVERIFY(std::abs(gate.edgeLineOpacity - 0.22F) < 0.001F);
    QVERIFY(gate.activeAdjustment);
    QCOMPARE(gate.markers[0].tipX, gate.lowerEdge.centerX);
    QCOMPARE(gate.markers[3].tipX, gate.upperEdge.centerX);
}

void SpectrumWaterfallItemTest::expiresFilterWidthLabelLifetimeWithControllableClock()
{
    using Milliseconds = std::chrono::milliseconds;
    sdr::gui::FilterWidthLabelLifetime lifetime;
    lifetime.trigger(Milliseconds(100));
    QVERIFY(lifetime.visibleAt(Milliseconds(1'099)));
    QVERIFY(!lifetime.visibleAt(Milliseconds(1'100)));
}

void SpectrumWaterfallItemTest::rendersFilterGateWithoutInputItems()
{
    SpectrumWaterfallItem item;
    QCOMPARE(item.childItems().size(), 0);
}

QTEST_MAIN(SpectrumWaterfallItemTest)

#include "SpectrumWaterfallItemTest.moc"
