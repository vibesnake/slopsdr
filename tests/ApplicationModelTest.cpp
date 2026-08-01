// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationModel.hpp"
#include "FrequencyDigitController.hpp"
#include "FrequencyViewport.hpp"
#include "MockReceiverBackend.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QUrl>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

QVariantMap scanPresetAt(const ApplicationModel& model, qsizetype index)
{
    return model.scanPresets().at(index).toMap();
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMilliseconds = 1'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

}  // namespace

class ApplicationModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void usesSlopSdrPersistencePaths();
    void hasSafeDefaults();
    void persistsAndClampsSpectrumWaterfallSplitRatio();
    void persistsAndClampsSidebarState();
    void usesPassbandDefaultsForFirstScanConfiguration();
    void persistsScanConfigurationAcrossApplicationModels();
    void persistsExactWideScanFrequencies();
    void fallsBackFromMalformedScanSettings();
    void preservesRestoredInvalidScanBoundsWithoutStarting();
    void createsLoadsUpdatesDeletesAndOrdersScanPresets();
    void persistsScanPresetsAcrossApplicationModels();
    void reordersScanPresetsAndPersistsStableIdentity();
    void validatesUniqueScanPresetNames();
    void roundTripsExactWidePresetFrequencies();
    void editsLoadedScanSettingsWithoutMutatingPreset();
    void ignoresMalformedStoredScanPresets();
    void loadsOutOfPassbandPresetWithoutStartingScanner();
    void persistsWideRangeScanTypeAndLoadsLegacyPresets();
    void updatesBookmarkScanAvailabilityFromInclusion();
    void scansCheckedBookmarkSnapshotsWithSavedReceiverSettings();
    void advancesAndWrapsBookmarkScanAfterDwell();
    void holdsBookmarkScanOnSquelchActivity();
    void retunesWideRangeOnlyBetweenCaptureBlocksAndWraps();
    void recentersCurrentWideChannelAfterDynamicFilterWidening();
    void suppressesSquelchWhileWideRangeTunerSettles();
    void rejectsScannerRangesThatCannotFitAfterCentering();
    void centersScannerRangeWithIntegerMidpoint();
    void givesActiveScannerExclusiveTuningControl();
    void disablesAutoForUnavailableMeasurementAndScannerOwnership();
    void autoSquelchDoesNotMutateLoadedBookmark();
    void persistsAndValidatesDsdFmeBinaryPath();
    void persistsAndValidatesRecordingsFolder();
    void namesBookmarksBeforeCreatingCapturedReceiverState();
    void updatesBookmarksByStableIdentityAndPreservesMetadata();
    void supportsDigitTuning();
    void supportsCenterFrequencyDigitEditSessions();
    void rejectsInvalidCenterFrequencyDigitEdits();
    void honorsDeviceSpecificDigitLimits();
    void enforcesFrequencyLimits();
    void keepsSpectrumTuningDistinctFromWaterfallZoom();
    void reportsDisplayZoomPercentage();
    void pansSharedViewportWithoutChangingReceiverTuning();
    void preservesViewportPanAcrossCaptureChangesAndScannerActivity();
    void anchorsWaterfallZoomAtListeningFrequency();
    void clampsZoomAndViewport();
    void supportsPartialPassbandAtDeviceRfLimit();
    void coalescesFractionalWaterfallZoomInput();
    void preservesOrClampsZoomAcrossCaptureAndFftChanges();
    void clicksWaterfallWithoutChangingCenterFrequency();
    void mapsThreeLineOverlayPositionsAndClipsEdges();
    void supportsConfigurableSpectrumTuning();
    void acceleratesRapidSpectrumWheelTuning();
    void normalizesHighResolutionAndTouchpadWheelInput();
    void routesModifierWheelActionsWithoutStateLeakage();
    void shiftWheelTunesListeningAndRecentersWhenZoomed();
    void accumulatesFilterWheelStepsAndPreservesSidebands();
    void preservesDisplayFramesDuringOrdinaryRetunes();
    void changesFftResolutionWithoutClearingHistoryAndRejectsOldSizeFrames();
    void keepsSpectrumLiveDuringContinuousTuning();
    void coalescesRapidRuntimeWheelTuningRequests();
    void recentersListeningFrequencyAfterDirectCenterChange();
    void updatesReceiverState();
    void tunesBookmarksExactlyWithoutStartingReception();
    void forwardsControlsToMockBackend();
    void defersGainApplicationUntilSliderCommit();
    void exposesModeSpecificControls();
    void filtersPresetWidthsAndAcceptsCustomWidths();
    void reportsUnsupportedPpmWithoutChangingState();
    void synchronizesLifecycleOnlyAfterBackendConfirmation();

private:
    QTemporaryDir m_settingsDirectory;
};

void ApplicationModelTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("slopSDR"));
    QCoreApplication::setApplicationName(QStringLiteral("slopSDR"));
    qputenv("XDG_CONFIG_HOME", m_settingsDirectory.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        m_settingsDirectory.path());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::SystemScope,
        m_settingsDirectory.path());
}

void ApplicationModelTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(QFile::remove(sdr::app::BookmarkTreeModel::defaultFilePath()) ||
            !QFile::exists(sdr::app::BookmarkTreeModel::defaultFilePath()));
}

void ApplicationModelTest::usesSlopSdrPersistencePaths()
{
    QSettings settings;
    settings.setValue(QStringLiteral("identity/pathTest"), true);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    const QString settingsPath = QDir::cleanPath(settings.fileName());
    const QString bookmarkPath = QDir::cleanPath(
        sdr::app::BookmarkTreeModel::defaultFilePath());
    QCOMPARE(
        settingsPath,
        QDir(m_settingsDirectory.path())
            .filePath(QStringLiteral("slopSDR/slopSDR.conf")));
    QCOMPARE(
        bookmarkPath,
        QDir(m_settingsDirectory.path())
            .filePath(QStringLiteral("slopSDR/bookmarks.json")));
    QVERIFY(QFile::exists(settingsPath));

    sdr::app::BookmarkTreeModel bookmarks;
    QVERIFY(QTest::qWaitFor([&bookmarks] { return !bookmarks.loading(); }));
    QVERIFY(!bookmarks.addGroup(-1, QStringLiteral("Path test")).isEmpty());
    QVERIFY(QTest::qWaitFor(
        [&bookmarks] { return !bookmarks.persistencePending(); }));
    QVERIFY(bookmarks.lastError().isEmpty());
    QVERIFY(QFile::exists(bookmarkPath));
    QVERIFY(!QDir(
                 QDir(m_settingsDirectory.path())
                     .filePath(QStringLiteral("slopSDR/slopSDR")))
                 .exists());
}

void ApplicationModelTest::hasSafeDefaults()
{
    const ApplicationModel model;

    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'000'000});
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000000"));
    QCOMPARE(model.listeningPosition(), 0.5);
    QCOMPARE(model.visibleLowerFrequency(), quint64{99'000'000});
    QCOMPARE(model.visibleUpperFrequency(), quint64{101'000'000});
    QCOMPARE(model.sampleRate(), quint64{2'000'000});
    QCOMPARE(model.spectrumFftSize(), quint64{4'096});
    QCOMPARE(model.spectrumHertzPerBin(), 2'000'000.0 / 4'096.0);
    QCOMPARE(model.spectrumFftSizeOptions().size(), 9);
    QCOMPARE(model.spectrumFftSizeOptions().constLast(), QStringLiteral("262144"));
    QCOMPARE(model.effectiveSpectrumFftSize(), quint64{4'096});
    QCOMPARE(model.effectiveWaterfallRowsPerSecond(), 60.0);
    QCOMPARE(model.filterWidth(), quint64{12'500});
    QCOMPARE(model.demodulationModeName(), QStringLiteral("AM"));
    QVERIFY(!model.squelchDisabled());
    QVERIFY(!model.receiverRunning());
    QCOMPARE(
        model.statusText(), QStringLiteral("Mock backend ready - no hardware device"));
}

void ApplicationModelTest::persistsAndClampsSpectrumWaterfallSplitRatio()
{
    constexpr auto setting = "display/spectrumWaterfallSplitRatio";
    QSettings settings;
    settings.remove(QString::fromLatin1(setting));
    settings.sync();

    {
        ApplicationModel model;
        QCOMPARE(model.spectrumWaterfallSplitRatio(), 0.5);
        model.setSpectrumWaterfallSplitRatio(0.63);
        QCOMPARE(model.spectrumWaterfallSplitRatio(), 0.63);
        model.commitSpectrumWaterfallSplitRatio();
        QCOMPARE(settings.value(QString::fromLatin1(setting)).toDouble(), 0.63);
    }

    {
        ApplicationModel restored;
        QCOMPARE(restored.spectrumWaterfallSplitRatio(), 0.63);
    }

    settings.setValue(QString::fromLatin1(setting), 0.05);
    settings.sync();
    {
        ApplicationModel clamped;
        QCOMPARE(clamped.spectrumWaterfallSplitRatio(), 0.2);
        clamped.setSpectrumWaterfallSplitRatio(0.95);
        QCOMPARE(clamped.spectrumWaterfallSplitRatio(), 0.8);
        clamped.setSpectrumWaterfallSplitRatio(400.0);
        QCOMPARE(clamped.spectrumWaterfallSplitRatio(), 0.5);
    }

    settings.setValue(QString::fromLatin1(setting), QStringLiteral("obsolete-pixels"));
    settings.sync();
    {
        ApplicationModel obsolete;
        QCOMPARE(obsolete.spectrumWaterfallSplitRatio(), 0.5);
        QCOMPARE(
            settings.value(QString::fromLatin1(setting)).toDouble(),
            0.5);
    }

    settings.remove(QString::fromLatin1(setting));
    settings.sync();
}

void ApplicationModelTest::persistsAndClampsSidebarState()
{
    const QString modeKey = QStringLiteral("display/sidebarMode");
    const QString legacyOpenKey = QStringLiteral("display/bookmarksPanelOpen");
    const QString bookmarksWidthKey = QStringLiteral("display/bookmarksPanelWidth");
    const QString scanWidthKey = QStringLiteral("display/scanPanelWidth");
    const QString settingsWidthKey = QStringLiteral("display/settingsPanelWidth");
    const QString consoleWidthKey = QStringLiteral("display/consolePanelWidth");
    QSettings settings;
    settings.remove(modeKey);
    settings.remove(legacyOpenKey);
    settings.remove(bookmarksWidthKey);
    settings.remove(scanWidthKey);
    settings.remove(settingsWidthKey);
    settings.remove(consoleWidthKey);
    settings.sync();

    {
        ApplicationModel model;
        QCOMPARE(model.sidebarMode(), QStringLiteral("none"));
        QVERIFY(!model.bookmarksPanelOpen());
        QVERIFY(!model.scanPanelOpen());
        QVERIFY(!model.settingsPanelOpen());
        QVERIFY(!model.consolePanelOpen());
        QCOMPARE(model.bookmarksPanelWidth(), 280.0);
        QCOMPARE(model.scanPanelWidth(), 320.0);
        QCOMPARE(model.settingsPanelWidth(), 320.0);
        QCOMPARE(model.consolePanelWidth(), 420.0);
        const quint64 originalCenter = model.centerFrequency();
        const quint64 originalListening = model.listeningFrequency();
        auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
            model.bookmarkModel());
        QVERIFY(bookmarks);
        QVERIFY(QTest::qWaitFor([bookmarks] { return !bookmarks->loading(); }));
        const QString bookmarkName = model.beginAddCurrentBookmark(-1);
        QVERIFY(!bookmarkName.isEmpty());
        QVERIFY(model.confirmAddCurrentBookmark(bookmarkName));
        QCOMPARE(bookmarks->rowCount(), 1);
        model.setSidebarMode(QStringLiteral("bookmarks"));
        QVERIFY(model.bookmarksPanelOpen());
        QVERIFY(!model.settingsPanelOpen());
        model.setBookmarksPanelWidth(120.0);
        QCOMPARE(model.bookmarksPanelWidth(), 220.0);
        model.setBookmarksPanelWidth(700.0);
        QCOMPARE(model.bookmarksPanelWidth(), 520.0);
        model.setBookmarksPanelWidth(333.0);
        model.commitBookmarksPanelWidth();
        model.setSidebarMode(QStringLiteral("settings"));
        QVERIFY(!model.bookmarksPanelOpen());
        QVERIFY(!model.scanPanelOpen());
        QVERIFY(model.settingsPanelOpen());
        QCOMPARE(model.centerFrequency(), originalCenter);
        QCOMPARE(model.listeningFrequency(), originalListening);
        QCOMPARE(model.bookmarkModel(), bookmarks);
        QCOMPARE(bookmarks->rowCount(), 1);
        model.setSettingsPanelWidth(120.0);
        QCOMPARE(model.settingsPanelWidth(), 220.0);
        model.setSettingsPanelWidth(700.0);
        QCOMPARE(model.settingsPanelWidth(), 520.0);
        model.setSettingsPanelWidth(366.0);
        model.commitSettingsPanelWidth();
        model.setSidebarMode(QStringLiteral("scan"));
        QVERIFY(!model.bookmarksPanelOpen());
        QVERIFY(model.scanPanelOpen());
        QVERIFY(!model.settingsPanelOpen());
        model.setScanPanelWidth(120.0);
        QCOMPARE(model.scanPanelWidth(), 220.0);
        model.setScanPanelWidth(700.0);
        QCOMPARE(model.scanPanelWidth(), 520.0);
        model.setScanPanelWidth(388.0);
        model.commitScanPanelWidth();
        model.setSidebarMode(QStringLiteral("console"));
        QVERIFY(!model.bookmarksPanelOpen());
        QVERIFY(!model.scanPanelOpen());
        QVERIFY(!model.settingsPanelOpen());
        QVERIFY(model.consolePanelOpen());
        model.setConsolePanelWidth(120.0);
        QCOMPARE(model.consolePanelWidth(), 220.0);
        model.setConsolePanelWidth(700.0);
        QCOMPARE(model.consolePanelWidth(), 520.0);
        model.setConsolePanelWidth(444.0);
        model.commitConsolePanelWidth();
        model.setSidebarMode(QStringLiteral("bookmarks"));
        model.setSidebarMode(QStringLiteral("console"));
        model.setSidebarMode(QStringLiteral("bookmarks"));
        QCOMPARE(model.sidebarMode(), QStringLiteral("bookmarks"));
        model.setSidebarMode(QStringLiteral("none"));
        QVERIFY(!model.bookmarksPanelOpen());
        QVERIFY(!model.settingsPanelOpen());
        model.setSidebarMode(QStringLiteral("scan"));
    }

    {
        ApplicationModel restored;
        QCOMPARE(restored.sidebarMode(), QStringLiteral("scan"));
        QVERIFY(!restored.bookmarksPanelOpen());
        QVERIFY(restored.scanPanelOpen());
        QVERIFY(!restored.settingsPanelOpen());
        QVERIFY(!restored.consolePanelOpen());
        QCOMPARE(restored.bookmarksPanelWidth(), 333.0);
        QCOMPARE(restored.scanPanelWidth(), 388.0);
        QCOMPARE(restored.settingsPanelWidth(), 366.0);
        QCOMPARE(restored.consolePanelWidth(), 444.0);
    }

    settings.setValue(modeKey, QStringLiteral("invalid"));
    settings.setValue(bookmarksWidthKey, QStringLiteral("obsolete"));
    settings.setValue(scanWidthKey, QStringLiteral("obsolete"));
    settings.setValue(settingsWidthKey, QStringLiteral("obsolete"));
    settings.setValue(consoleWidthKey, QStringLiteral("obsolete"));
    settings.sync();
    {
        ApplicationModel invalid;
        QCOMPARE(invalid.sidebarMode(), QStringLiteral("none"));
        QVERIFY(!invalid.bookmarksPanelOpen());
        QVERIFY(!invalid.scanPanelOpen());
        QVERIFY(!invalid.settingsPanelOpen());
        QVERIFY(!invalid.consolePanelOpen());
        QCOMPARE(invalid.bookmarksPanelWidth(), 280.0);
        QCOMPARE(invalid.scanPanelWidth(), 320.0);
        QCOMPARE(invalid.settingsPanelWidth(), 320.0);
        QCOMPARE(invalid.consolePanelWidth(), 420.0);
    }

    settings.remove(modeKey);
    settings.setValue(legacyOpenKey, true);
    settings.sync();
    {
        ApplicationModel migrated;
        QCOMPARE(migrated.sidebarMode(), QStringLiteral("bookmarks"));
        QVERIFY(migrated.bookmarksPanelOpen());
        QVERIFY(!migrated.settingsPanelOpen());
    }

    settings.remove(modeKey);
    settings.remove(legacyOpenKey);
    settings.remove(bookmarksWidthKey);
    settings.remove(scanWidthKey);
    settings.remove(settingsWidthKey);
    settings.remove(consoleWidthKey);
    settings.sync();
}

void ApplicationModelTest::usesPassbandDefaultsForFirstScanConfiguration()
{
    QSettings settings;
    QVERIFY(!settings.contains(QStringLiteral("scanner/lowerFrequencyHz")));
    QVERIFY(!settings.contains(QStringLiteral("scanner/upperFrequencyHz")));
    QVERIFY(!settings.contains(QStringLiteral("scanner/stepSizeHz")));
    QVERIFY(!settings.contains(QStringLiteral("scanner/dwellMilliseconds")));
    QVERIFY(!settings.contains(
        QStringLiteral("scanner/resumeDelayMilliseconds")));

    ApplicationModel model;
    const quint64 center = model.centerFrequency();
    QCOMPARE(model.scanLowerFrequency(), center - model.sampleRate() / 2);
    QCOMPARE(model.scanUpperFrequency(), center + model.sampleRate() / 2);
    QCOMPARE(model.scanStepSize(), quint64{12'500});
    QCOMPARE(model.scanDwellMilliseconds(), 250);
    QCOMPARE(model.scanResumeDelayMilliseconds(), 1'000);
    QCOMPARE(model.scanState(), QStringLiteral("Stopped"));
    QCOMPARE(model.scanCurrentFrequency(), quint64{0});
}

void ApplicationModelTest::persistsScanConfigurationAcrossApplicationModels()
{
    ApplicationModel defaults;
    const quint64 center = defaults.centerFrequency();
    const quint64 lower = center - 150'000;
    const quint64 upper = center + 175'000;
    {
        ApplicationModel model;
        model.setScanLowerFrequency(lower);
        model.setScanUpperFrequency(upper);
        model.setScanStepSize(25'000);
        model.setScanDwellMilliseconds(75);
        model.setScanResumeDelayMilliseconds(325);
        model.startReception();
        model.startScan();
        QCOMPARE(model.scanState(), QStringLiteral("Running"));
    }

    QSettings settings;
    settings.sync();
    QCOMPARE(
        settings.value(QStringLiteral("scanner/lowerFrequencyHz"))
            .toULongLong(),
        lower);
    QCOMPARE(
        settings.value(QStringLiteral("scanner/upperFrequencyHz"))
            .toULongLong(),
        upper);
    QCOMPARE(
        settings.value(QStringLiteral("scanner/stepSizeHz")).toULongLong(),
        quint64{25'000});
    QCOMPARE(
        settings.value(QStringLiteral("scanner/dwellMilliseconds")).toInt(),
        75);
    QCOMPARE(
        settings.value(QStringLiteral("scanner/resumeDelayMilliseconds"))
            .toInt(),
        325);

    ApplicationModel restored;
    QCOMPARE(restored.scanLowerFrequency(), lower);
    QCOMPARE(restored.scanUpperFrequency(), upper);
    QCOMPARE(restored.scanStepSize(), quint64{25'000});
    QCOMPARE(restored.scanDwellMilliseconds(), 75);
    QCOMPARE(restored.scanResumeDelayMilliseconds(), 325);
    QCOMPARE(restored.scanState(), QStringLiteral("Stopped"));
    QCOMPARE(restored.scanCurrentFrequency(), quint64{0});
}

void ApplicationModelTest::persistsExactWideScanFrequencies()
{
    constexpr quint64 lower = 4'448'000'123;
    constexpr quint64 upper = 4'472'000'987;
    constexpr quint64 step = 4'294'967'297;
    QSettings settings;
    settings.setValue(QStringLiteral("scanner/lowerFrequencyHz"), lower);
    settings.setValue(QStringLiteral("scanner/upperFrequencyHz"), upper);
    settings.setValue(QStringLiteral("scanner/stepSizeHz"), step);
    settings.sync();

    ApplicationModel model;
    QCOMPARE(model.scanLowerFrequency(), lower);
    QCOMPARE(model.scanUpperFrequency(), upper);
    QCOMPARE(model.scanStepSize(), step);
    QVERIFY(!model.scanValidationError().isEmpty());
}

void ApplicationModelTest::fallsBackFromMalformedScanSettings()
{
    QSettings settings;
    settings.setValue(
        QStringLiteral("scanner/lowerFrequencyHz"),
        QStringLiteral("not-a-frequency"));
    settings.setValue(
        QStringLiteral("scanner/upperFrequencyHz"),
        QStringLiteral("not-a-frequency"));
    settings.setValue(QStringLiteral("scanner/stepSizeHz"), qulonglong{0});
    settings.setValue(QStringLiteral("scanner/dwellMilliseconds"), -10);
    settings.setValue(
        QStringLiteral("scanner/resumeDelayMilliseconds"),
        QStringLiteral("negative is invalid"));
    settings.sync();

    ApplicationModel model;
    const quint64 center = model.centerFrequency();
    QCOMPARE(model.scanLowerFrequency(), center - model.sampleRate() / 2);
    QCOMPARE(model.scanUpperFrequency(), center + model.sampleRate() / 2);
    QCOMPARE(model.scanStepSize(), quint64{12'500});
    QCOMPARE(model.scanDwellMilliseconds(), 250);
    QCOMPARE(model.scanResumeDelayMilliseconds(), 1'000);
    QVERIFY(model.scanValidationError().isEmpty());
    QVERIFY(!model.scanCanStart());
    model.startReception();
    QVERIFY(model.scanCanStart());
}

void ApplicationModelTest::preservesRestoredInvalidScanBoundsWithoutStarting()
{
    ApplicationModel defaults;
    const quint64 lower = defaults.scanLowerFrequency();
    const quint64 upper = defaults.scanUpperFrequency();
    QSettings settings;
    settings.setValue(QStringLiteral("scanner/lowerFrequencyHz"), lower - 1);
    settings.setValue(QStringLiteral("scanner/upperFrequencyHz"), upper + 1);
    settings.sync();

    ApplicationModel model;
    QCOMPARE(model.scanLowerFrequency(), lower - 1);
    QCOMPARE(model.scanUpperFrequency(), upper + 1);
    QVERIFY(model.scanValidationError().contains(
        QStringLiteral("requires 2000002 Hz")));
    QVERIFY(model.scanValidationError().contains(
        QStringLiteral("provides 2000000 Hz")));
    QVERIFY(!model.scanCanStart());
    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Stopped"));
    QVERIFY(model.scanStatusMessage().contains(
        QStringLiteral("Scanner not started")));
}

void ApplicationModelTest::createsLoadsUpdatesDeletesAndOrdersScanPresets()
{
    ApplicationModel model;
    const quint64 center = model.centerFrequency();
    model.setScanLowerFrequency(center - 100'000);
    model.setScanUpperFrequency(center + 100'000);
    model.setScanStepSize(12'500);
    model.setScanDwellMilliseconds(80);
    model.setScanResumeDelayMilliseconds(240);
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Morning")));
    const QVariantMap morning = scanPresetAt(model, 0);
    const QString morningId = morning.value(QStringLiteral("presetId")).toString();

    model.setScanLowerFrequency(center + 200'000);
    model.setScanUpperFrequency(center + 300'000);
    model.setScanStepSize(25'000);
    model.setScanDwellMilliseconds(120);
    model.setScanResumeDelayMilliseconds(360);
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Evening")));
    const QString eveningId = scanPresetAt(model, 1)
                                  .value(QStringLiteral("presetId"))
                                  .toString();

    const QVariantList presets = model.scanPresets();
    QCOMPARE(presets.size(), 2);
    QCOMPARE(presets.at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Morning"));
    QCOMPARE(presets.at(1).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Evening"));

    model.selectScanPreset(morningId);
    QVERIFY(model.loadSelectedScanPreset());
    QCOMPARE(model.scanLowerFrequency(), center - 100'000);
    QCOMPARE(model.scanUpperFrequency(), center + 100'000);
    QCOMPARE(model.scanStepSize(), quint64{12'500});
    QCOMPARE(model.scanState(), QStringLiteral("Stopped"));

    model.setScanStepSize(10'000);
    QVERIFY(model.updateSelectedScanPreset(QStringLiteral("Morning renamed")));
    const QVariantMap updated = scanPresetAt(model, 0);
    QCOMPARE(updated.value(QStringLiteral("presetId")).toString(), morningId);
    QCOMPARE(updated.value(QStringLiteral("name")).toString(),
             QStringLiteral("Morning renamed"));
    QCOMPARE(updated.value(QStringLiteral("stepSizeHz")).toULongLong(),
             quint64{10'000});

    model.selectScanPreset(eveningId);
    QVERIFY(model.deleteSelectedScanPreset());
    QCOMPARE(model.scanPresets().size(), 1);
    QCOMPARE(scanPresetAt(model, 0).value(QStringLiteral("presetId")).toString(),
             morningId);
    QVERIFY(model.selectedScanPresetId().isEmpty());
}

void ApplicationModelTest::persistsScanPresetsAcrossApplicationModels()
{
    QString firstId;
    QString secondId;
    {
        ApplicationModel model;
        const quint64 center = model.centerFrequency();
        model.setScanLowerFrequency(center - 75'000);
        model.setScanUpperFrequency(center + 25'000);
        QVERIFY(model.saveNewScanPreset(QStringLiteral("First")));
        firstId = scanPresetAt(model, 0).value(QStringLiteral("presetId")).toString();
        model.setScanLowerFrequency(center + 100'000);
        model.setScanUpperFrequency(center + 175'000);
        model.setScanStepSize(5'000);
        QVERIFY(model.saveNewScanPreset(QStringLiteral("Second")));
        secondId = scanPresetAt(model, 1).value(QStringLiteral("presetId")).toString();
    }

    ApplicationModel restored;
    QCOMPARE(restored.scanPresets().size(), 2);
    QCOMPARE(scanPresetAt(restored, 0).value(QStringLiteral("name")).toString(),
             QStringLiteral("First"));
    QCOMPARE(scanPresetAt(restored, 1).value(QStringLiteral("name")).toString(),
             QStringLiteral("Second"));
    QCOMPARE(
        scanPresetAt(restored, 0).value(QStringLiteral("presetId")).toString(),
        firstId);
    QCOMPARE(
        scanPresetAt(restored, 1).value(QStringLiteral("presetId")).toString(),
        secondId);
    QVERIFY(restored.selectedScanPresetId().isEmpty());
    QCOMPARE(restored.scanState(), QStringLiteral("Stopped"));
}

void ApplicationModelTest::reordersScanPresetsAndPersistsStableIdentity()
{
    ApplicationModel model;
    QVERIFY(model.saveNewScanPreset(QStringLiteral("First")));
    const QString firstId =
        scanPresetAt(model, 0).value(QStringLiteral("presetId")).toString();
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Second")));
    const QString secondId =
        scanPresetAt(model, 1).value(QStringLiteral("presetId")).toString();
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Third")));
    const QString thirdId =
        scanPresetAt(model, 2).value(QStringLiteral("presetId")).toString();

    QVERIFY(model.moveScanPreset(thirdId, firstId, QStringLiteral("before")));
    QCOMPARE(
        scanPresetAt(model, 0).value(QStringLiteral("presetId")).toString(),
        thirdId);
    QCOMPARE(
        scanPresetAt(model, 1).value(QStringLiteral("presetId")).toString(),
        firstId);
    QCOMPARE(
        scanPresetAt(model, 2).value(QStringLiteral("presetId")).toString(),
        secondId);

    ApplicationModel restored;
    QCOMPARE(
        scanPresetAt(restored, 0).value(QStringLiteral("presetId")).toString(),
        thirdId);
    QCOMPARE(
        scanPresetAt(restored, 1).value(QStringLiteral("presetId")).toString(),
        firstId);
    QCOMPARE(
        scanPresetAt(restored, 2).value(QStringLiteral("presetId")).toString(),
        secondId);
}

void ApplicationModelTest::validatesUniqueScanPresetNames()
{
    ApplicationModel model;
    QVERIFY(model.saveNewScanPreset(QStringLiteral("City watch")));
    QVERIFY(!model.saveNewScanPreset(QStringLiteral(" city watch ")));
    QVERIFY(model.scanPresetStatusMessage().contains(
        QStringLiteral("already exists")));

    model.selectScanPreset(
        scanPresetAt(model, 0).value(QStringLiteral("presetId")).toString());
    QVERIFY(!model.updateSelectedScanPreset(QStringLiteral("   ")));
    QVERIFY(model.scanPresetStatusMessage().contains(
        QStringLiteral("non-empty")));
}

void ApplicationModelTest::roundTripsExactWidePresetFrequencies()
{
    constexpr quint64 lower = 4'448'000'123;
    constexpr quint64 upper = 4'472'000'987;
    constexpr quint64 step = 4'294'967'297;
    QString presetId;
    {
        ApplicationModel model;
        model.setScanLowerFrequency(lower);
        model.setScanUpperFrequency(upper);
        model.setScanStepSize(step);
        QVERIFY(model.saveNewScanPreset(QStringLiteral("Wide integers")));
        const QVariantMap preset = scanPresetAt(model, 0);
        presetId = preset.value(QStringLiteral("presetId")).toString();
        QCOMPARE(preset.value(QStringLiteral("lowerFrequencyHz")).toULongLong(),
                 lower);
        QCOMPARE(preset.value(QStringLiteral("upperFrequencyHz")).toULongLong(),
                 upper);
        QCOMPARE(preset.value(QStringLiteral("stepSizeHz")).toULongLong(), step);
    }

    ApplicationModel restored;
    restored.selectScanPreset(presetId);
    QVERIFY(restored.loadSelectedScanPreset());
    QCOMPARE(restored.scanLowerFrequency(), lower);
    QCOMPARE(restored.scanUpperFrequency(), upper);
    QCOMPARE(restored.scanStepSize(), step);
}

void ApplicationModelTest::editsLoadedScanSettingsWithoutMutatingPreset()
{
    ApplicationModel model;
    const quint64 center = model.centerFrequency();
    model.setScanLowerFrequency(center - 50'000);
    model.setScanUpperFrequency(center + 50'000);
    model.setScanStepSize(12'500);
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Editable copy")));
    const QVariantMap saved = scanPresetAt(model, 0);
    model.setScanLowerFrequency(center + 100'000);
    model.setScanUpperFrequency(center + 200'000);
    model.setScanStepSize(20'000);

    model.selectScanPreset(saved.value(QStringLiteral("presetId")).toString());
    QVERIFY(model.loadSelectedScanPreset());
    model.setScanStepSize(7'500);
    model.setScanDwellMilliseconds(333);

    const QVariantMap unchanged = scanPresetAt(model, 0);
    QCOMPARE(unchanged.value(QStringLiteral("presetId")).toString(),
             saved.value(QStringLiteral("presetId")).toString());
    QCOMPARE(unchanged.value(QStringLiteral("stepSizeHz")).toULongLong(),
             quint64{12'500});
    QCOMPARE(unchanged.value(QStringLiteral("dwellMilliseconds")).toInt(), 250);
}

void ApplicationModelTest::ignoresMalformedStoredScanPresets()
{
    QVariantMap valid;
    valid.insert(
        QStringLiteral("id"),
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    valid.insert(QStringLiteral("name"), QStringLiteral("Valid preset"));
    valid.insert(QStringLiteral("scanType"), QStringLiteral("currentPassband"));
    valid.insert(QStringLiteral("lowerFrequencyHz"), qulonglong{99'900'000});
    valid.insert(QStringLiteral("upperFrequencyHz"), qulonglong{100'100'000});
    valid.insert(QStringLiteral("stepSizeHz"), qulonglong{12'500});
    valid.insert(QStringLiteral("dwellMilliseconds"), 250);
    valid.insert(QStringLiteral("resumeDelayMilliseconds"), 1'000);
    QVariantMap malformed;
    malformed.insert(QStringLiteral("id"), QStringLiteral("not-a-uuid"));
    malformed.insert(QStringLiteral("name"), QStringLiteral("Broken"));

    QSettings settings;
    settings.setValue(
        QStringLiteral("scanner/presets"),
        QVariantList{malformed, valid});
    settings.sync();

    ApplicationModel model;
    QCOMPARE(model.scanPresets().size(), 1);
    QCOMPARE(scanPresetAt(model, 0).value(QStringLiteral("name")).toString(),
             QStringLiteral("Valid preset"));
    QVERIFY(model.scanPresetStatusMessage().contains(
        QStringLiteral("Ignored invalid")));
}

void ApplicationModelTest::loadsOutOfPassbandPresetWithoutStartingScanner()
{
    constexpr quint64 lower = 4'448'000'000;
    constexpr quint64 upper = 4'472'000'000;
    ApplicationModel model;
    model.setScanLowerFrequency(lower);
    model.setScanUpperFrequency(upper);
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Out of passband")));
    const QString presetId = scanPresetAt(model, 0)
                                 .value(QStringLiteral("presetId"))
                                 .toString();
    model.selectScanPreset(presetId);
    model.setScanDwellMilliseconds(321);
    QVERIFY(model.updateSelectedScanPreset(QStringLiteral("Out of passband")));

    const quint64 center = model.centerFrequency();
    model.setScanLowerFrequency(center - 100'000);
    model.setScanUpperFrequency(center + 100'000);
    model.selectScanPreset(presetId);
    QVERIFY(model.loadSelectedScanPreset());
    QCOMPARE(model.scanLowerFrequency(), lower);
    QCOMPARE(model.scanUpperFrequency(), upper);
    QCOMPARE(model.scanDwellMilliseconds(), 321);
    QVERIFY(!model.scanValidationError().isEmpty());
    QVERIFY(!model.scanCanStart());
    QCOMPARE(model.scanState(), QStringLiteral("Stopped"));
}

void ApplicationModelTest::persistsWideRangeScanTypeAndLoadsLegacyPresets()
{
    ApplicationModel model;
    model.setScanTypeIndex(1);
    QCOMPARE(model.scanTypeIndex(), 1);
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Wide")));
    QCOMPARE(
        scanPresetAt(model, 0).value(QStringLiteral("scanType")).toString(),
        QStringLiteral("wideRange"));

    ApplicationModel restored;
    QCOMPARE(restored.scanTypeIndex(), 1);
    restored.selectScanPreset(
        scanPresetAt(restored, 0).value(QStringLiteral("presetId")).toString());
    QVERIFY(restored.loadSelectedScanPreset());
    QCOMPARE(restored.scanTypeIndex(), 1);
    QCOMPARE(restored.scanState(), QStringLiteral("Stopped"));

    QVariantMap legacy;
    legacy.insert(
        QStringLiteral("id"),
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    legacy.insert(QStringLiteral("name"), QStringLiteral("Legacy"));
    legacy.insert(QStringLiteral("lowerFrequencyHz"), qulonglong{100'000'000});
    legacy.insert(QStringLiteral("upperFrequencyHz"), qulonglong{101'000'000});
    legacy.insert(QStringLiteral("stepSizeHz"), qulonglong{12'500});
    legacy.insert(QStringLiteral("dwellMilliseconds"), 100);
    legacy.insert(QStringLiteral("resumeDelayMilliseconds"), 200);
    QSettings settings;
    settings.setValue(QStringLiteral("scanner/presets"), QVariantList{legacy});
    settings.setValue(QStringLiteral("scanner/scanType"), QStringLiteral("currentPassband"));
    settings.sync();

    ApplicationModel legacyRestored;
    QCOMPARE(legacyRestored.scanPresets().size(), 1);
    QCOMPARE(
        scanPresetAt(legacyRestored, 0)
            .value(QStringLiteral("scanType"))
            .toString(),
        QStringLiteral("currentPassband"));
    legacyRestored.selectScanPreset(
        scanPresetAt(legacyRestored, 0)
            .value(QStringLiteral("presetId"))
            .toString());
    QVERIFY(legacyRestored.loadSelectedScanPreset());
    QCOMPARE(legacyRestored.scanTypeIndex(), 0);
}

void ApplicationModelTest::scansCheckedBookmarkSnapshotsWithSavedReceiverSettings()
{
    ApplicationModel model;
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
        model.bookmarkModel());
    QVERIFY(bookmarks);
    const auto add = [bookmarks](QString name, quint64 frequency,
                                 QString mode, qint64 low, qint64 high,
                                 double squelch, bool legacy = false) {
        sdr::app::BookmarkData bookmark;
        bookmark.name = std::move(name);
        bookmark.listeningFrequency = frequency;
        bookmark.requestedGainDb = 18.0;
        bookmark.demodulatorId = std::move(mode);
        bookmark.filterLowHz = low;
        bookmark.filterHighHz = high;
        bookmark.squelchThresholdDb = squelch;
        bookmark.squelchEnabled = true;
        bookmark.hasSavedSquelch = !legacy;
        bookmark.scannerIncluded = true;
        return bookmarks->addBookmark(-1, bookmark);
    };
    const QString legacy = add(QStringLiteral("Legacy AM"), 100'000'000,
                               QStringLiteral("am"), -2'500, 2'500, -70.0, true);
    const QString duplicate = add(QStringLiteral("USB duplicate"), 100'000'000,
                                  QStringLiteral("usb"), 0, 2'700, -64.0);
    const QString distant = add(QStringLiteral("Distant AM"), 103'000'000,
                                QStringLiteral("am"), -2'500, 2'500, -58.0);
    QVERIFY(!legacy.isEmpty());
    QVERIFY(!duplicate.isEmpty());
    QVERIFY(!distant.isEmpty());

    model.setSquelchLevel(-52.0);
    model.setBookmarkScanDwellMilliseconds(100'000);
    model.setBookmarkScanResumeDelayMilliseconds(321);
    ApplicationModel restoredTiming;
    QCOMPARE(restoredTiming.bookmarkScanDwellMilliseconds(), 100'000);
    QCOMPARE(restoredTiming.bookmarkScanResumeDelayMilliseconds(), 321);

    model.setScanTypeIndex(2);
    QCOMPARE(model.scanTypeIndex(), 0);
    QVERIFY(model.saveNewScanPreset(QStringLiteral("Normal scanner only")));
    const QString presetId = scanPresetAt(model, 0)
                                 .value(QStringLiteral("presetId")).toString();
    QCOMPARE(scanPresetAt(model, 0).value(QStringLiteral("scanType")).toString(),
             QStringLiteral("currentPassband"));
    model.selectScanPreset(presetId);
    QVERIFY(model.loadSelectedScanPreset());
    QCOMPARE(model.scanTypeIndex(), 0);
    QVERIFY(bookmarks->bookmarkAt(bookmarks->visibleRowForUuid(legacy))
                ->scannerIncluded);
    QVERIFY(!model.bookmarkScanCanStart());
    model.startReception();
    QVERIFY(model.bookmarkScanCanStart());
    const quint64 initialCenter = model.centerFrequency();
    model.startBookmarkScan();
    QVERIFY2(waitFor([&model] { return model.bookmarkScanState() == QLatin1String("Running"); }),
             qPrintable(model.bookmarkScanStatusMessage()));
    QCOMPARE(model.bookmarkScanCurrentName(), QStringLiteral("Legacy AM"));
    QCOMPARE(model.bookmarkScanPosition(), QStringLiteral("1 of 3"));
    QCOMPARE(model.squelchLevel(), -52.0);
    QCOMPARE(model.centerFrequency(), initialCenter);
    QVERIFY(!model.scanCanStart());
    model.startScan();
    QCOMPARE(model.bookmarkScanCurrentName(), QStringLiteral("Legacy AM"));

    model.pauseOrResumeBookmarkScan();
    QCOMPARE(model.bookmarkScanState(), QStringLiteral("Paused"));
    QVERIFY(bookmarks->moveBookmark(distant, legacy, QStringLiteral("before")));
    model.skipBookmarkScan();
    QVERIFY(waitFor([&model] { return model.bookmarkScanState() == QLatin1String("Paused"); }));
    QCOMPARE(model.bookmarkScanCurrentName(), QStringLiteral("USB duplicate"));
    QCOMPARE(model.bookmarkScanPosition(), QStringLiteral("2 of 3"));
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.squelchLevel(), -64.0);

    model.skipBookmarkScan();
    QVERIFY(waitFor([&model] { return model.bookmarkScanState() == QLatin1String("Paused"); }));
    QCOMPARE(model.bookmarkScanCurrentName(), QStringLiteral("Distant AM"));
    QCOMPARE(model.centerFrequency(), quint64{103'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{103'000'000});
    model.skipBookmarkScan();
    QVERIFY(waitFor([&model] { return model.bookmarkScanState() == QLatin1String("Paused"); }));
    QCOMPARE(model.bookmarkScanCurrentName(), QStringLiteral("Legacy AM"));
    QCOMPARE(model.bookmarkScanPosition(), QStringLiteral("1 of 3"));
    model.stopBookmarkScan();
    QVERIFY(!model.scannerOwnsTuning());
    QCOMPARE(model.bookmarkScanState(), QStringLiteral("Stopped"));

    model.setScanDwellMilliseconds(100'000);
    model.startScan();
    QVERIFY(model.scannerOwnsTuning());
    QVERIFY(!model.bookmarkScanCanStart());
    const quint64 normalScanFrequency = model.scanCurrentFrequency();
    model.startBookmarkScan();
    QCOMPARE(model.scanCurrentFrequency(), normalScanFrequency);
    QCOMPARE(model.bookmarkScanState(), QStringLiteral("Stopped"));
    model.stopScan();
}

void ApplicationModelTest::updatesBookmarkScanAvailabilityFromInclusion()
{
    ApplicationModel model;
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(model.bookmarkModel());
    QVERIFY(bookmarks);
    const QString group = bookmarks->addGroup(-1, QStringLiteral("Local group"));
    sdr::app::BookmarkData bookmark;
    bookmark.name = QStringLiteral("Reachable");
    bookmark.listeningFrequency = 100'000'000;
    bookmark.demodulatorId = QStringLiteral("am");
    bookmark.filterLowHz = -2'500;
    bookmark.filterHighHz = 2'500;
    const QString child = bookmarks->addBookmark(
        bookmarks->visibleRowForUuid(group), bookmark);
    QVERIFY(!child.isEmpty());
    model.startReception();
    QVERIFY(!model.bookmarkScanCanStart());
    QSignalSpy availabilityChanges(
        &model, &ApplicationModel::bookmarkScannerChanged);
    QVERIFY(bookmarks->toggleScannerInclusion(
        bookmarks->visibleRowForUuid(group)));
    QVERIFY(availabilityChanges.count() > 0);
    QVERIFY(model.bookmarkScanCanStart());
    availabilityChanges.clear();
    QVERIFY(bookmarks->toggleScannerInclusion(
        bookmarks->visibleRowForUuid(group)));
    QVERIFY(availabilityChanges.count() > 0);
    QVERIFY(!model.bookmarkScanCanStart());

    bookmark.name = QStringLiteral("Unsupported");
    bookmark.demodulatorId = QStringLiteral("missing-mode");
    bookmark.scannerIncluded = true;
    QVERIFY(!bookmarks->addBookmark(-1, bookmark).isEmpty());
    QVERIFY(!model.bookmarkScanCanStart());
}

void ApplicationModelTest::holdsBookmarkScanOnSquelchActivity()
{
    auto backend = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{.squelchOpen = true});
    ApplicationModel model(std::move(backend));
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(model.bookmarkModel());
    QVERIFY(bookmarks);
    sdr::app::BookmarkData bookmark;
    bookmark.name = QStringLiteral("Active channel");
    bookmark.listeningFrequency = 100'000'000;
    bookmark.demodulatorId = QStringLiteral("am");
    bookmark.filterLowHz = -2'500;
    bookmark.filterHighHz = 2'500;
    bookmark.scannerIncluded = true;
    QVERIFY(!bookmarks->addBookmark(-1, bookmark).isEmpty());
    model.startReception();
    model.startBookmarkScan();
    QVERIFY(waitFor([&model] {
        return model.bookmarkScanState() ==
               QLatin1String("Holding on squelch activity");
    }));
    model.pauseOrResumeBookmarkScan();
    QCOMPARE(model.bookmarkScanState(), QStringLiteral("Paused"));
    model.stopBookmarkScan();
    QVERIFY(!model.scannerOwnsTuning());
}

void ApplicationModelTest::advancesAndWrapsBookmarkScanAfterDwell()
{
    ApplicationModel model;
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(model.bookmarkModel());
    QVERIFY(bookmarks);
    const auto add = [bookmarks](const QString& name, quint64 frequency) {
        sdr::app::BookmarkData bookmark;
        bookmark.name = name;
        bookmark.listeningFrequency = frequency;
        bookmark.demodulatorId = QStringLiteral("am");
        bookmark.filterLowHz = -2'500;
        bookmark.filterHighHz = 2'500;
        bookmark.scannerIncluded = true;
        return bookmarks->addBookmark(-1, bookmark);
    };
    QVERIFY(!add(QStringLiteral("First"), 100'000'000).isEmpty());
    QVERIFY(!add(QStringLiteral("Second"), 100'010'000).isEmpty());
    model.setBookmarkScanDwellMilliseconds(20);
    model.startReception();
    model.startBookmarkScan();
    QVERIFY(waitFor([&model] {
        return model.bookmarkScanCurrentName() == QLatin1String("Second");
    }));
    QVERIFY(waitFor([&model] {
        return model.bookmarkScanCurrentName() == QLatin1String("First");
    }));
    model.stopBookmarkScan();
}

void ApplicationModelTest::retunesWideRangeOnlyBetweenCaptureBlocksAndWraps()
{
    ApplicationModel model;
    model.startReception();
    model.setScanTypeIndex(1);
    model.setScanLowerFrequency(100'000'000);
    model.setScanUpperFrequency(104'000'000);
    model.setScanStepSize(1'000'000);
    model.setScanDwellMilliseconds(100'000);
    QVERIFY(model.scanValidationError().isEmpty());

    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Retuning"));
    QVERIFY(model.scannerOwnsTuning());
    QVERIFY(waitFor([&model] {
        return model.scanState() == QLatin1String("Running");
    }));
    QCOMPARE(model.scanCurrentFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'000'000});
    QCOMPARE(model.scanCaptureBlockProgress(), QStringLiteral("1 of 3"));
    const quint64 firstBlockCenter = model.centerFrequency();

    model.pauseOrResumeScan();
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));
    model.skipScanFrequency();
    QCOMPARE(model.scanCurrentFrequency(), quint64{101'000'000});
    QCOMPARE(model.centerFrequency(), firstBlockCenter);
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));

    model.skipScanFrequency();
    QCOMPARE(model.scanCurrentFrequency(), quint64{102'000'000});
    QCOMPARE(model.scanState(), QStringLiteral("Retuning"));
    const quint64 secondBlockCenter = model.centerFrequency();
    QVERIFY(secondBlockCenter != firstBlockCenter);
    QVERIFY(waitFor([&model] {
        return model.scanState() == QLatin1String("Paused");
    }));
    QCOMPARE(model.listeningFrequency(), quint64{102'000'000});
    QCOMPARE(model.scanCaptureBlockProgress(), QStringLiteral("2 of 3"));

    model.skipScanFrequency();
    QCOMPARE(model.centerFrequency(), secondBlockCenter);
    model.skipScanFrequency();
    QCOMPARE(model.scanState(), QStringLiteral("Retuning"));
    QVERIFY(waitFor([&model] {
        return model.scanState() == QLatin1String("Paused");
    }));
    QCOMPARE(model.scanCurrentFrequency(), quint64{104'000'000});
    QCOMPARE(model.scanCaptureBlockProgress(), QStringLiteral("3 of 3"));

    model.skipScanFrequency();
    QCOMPARE(model.scanState(), QStringLiteral("Retuning"));
    QVERIFY(waitFor([&model] {
        return model.scanState() == QLatin1String("Paused");
    }));
    QCOMPARE(model.scanCurrentFrequency(), quint64{100'000'000});
    QCOMPARE(model.centerFrequency(), firstBlockCenter);
    QCOMPARE(model.scanCaptureBlockProgress(), QStringLiteral("1 of 3"));
}

void ApplicationModelTest::recentersCurrentWideChannelAfterDynamicFilterWidening()
{
    ApplicationModel model;
    model.startReception();
    model.setScanTypeIndex(1);
    model.setScanLowerFrequency(100'000'000);
    model.setScanUpperFrequency(103'800'000);
    model.setScanStepSize(1'900'000);
    model.setScanDwellMilliseconds(100'000);
    model.startScan();
    QVERIFY(waitFor([&model] {
        return model.scanState() == QLatin1String("Running");
    }));
    model.pauseOrResumeScan();
    model.skipScanFrequency();
    QCOMPARE(model.scanCurrentFrequency(), quint64{101'900'000});
    const quint64 narrowCenter = model.centerFrequency();

    model.setDemodulationModeIndex(
        static_cast<int>(sdr::radio::DemodulationMode::Wfm));
    QCOMPARE(model.scanCurrentFrequency(), quint64{101'900'000});
    QCOMPARE(model.scanState(), QStringLiteral("Retuning"));
    QVERIFY(model.centerFrequency() != narrowCenter);
    QVERIFY(waitFor([&model] {
        return model.scanState() == QLatin1String("Paused");
    }));
    QCOMPARE(model.scanCurrentFrequency(), quint64{101'900'000});
    QCOMPARE(model.listeningFrequency(), quint64{101'900'000});
    const quint64 wideCenter = model.centerFrequency();

    model.setDemodulationModeIndex(
        static_cast<int>(sdr::radio::DemodulationMode::Am));
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));
    QCOMPARE(model.centerFrequency(), wideCenter);
}

void ApplicationModelTest::suppressesSquelchWhileWideRangeTunerSettles()
{
    auto backend = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{
            .ppmCorrectionSupported = true,
            .startSucceeds = true,
            .stopSucceeds = true,
            .squelchOpen = true,
        });
    ApplicationModel model(std::move(backend));
    model.startReception();
    model.setScanTypeIndex(1);
    model.setScanLowerFrequency(100'000'000);
    model.setScanUpperFrequency(102'000'000);
    model.setScanStepSize(1'000'000);
    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Retuning"));
    QVERIFY(!model.scanState().contains(QStringLiteral("Holding")));
    QVERIFY(waitFor([&model] {
        return model.scanState() ==
               QLatin1String("Holding on squelch activity");
    }));
    QCOMPARE(model.scanCurrentFrequency(), quint64{100'000'000});
}

void ApplicationModelTest::rejectsScannerRangesThatCannotFitAfterCentering()
{
    ApplicationModel model;
    model.startReception();
    const quint64 center = model.centerFrequency();
    const quint64 lower = center - model.sampleRate() / 2;
    const quint64 upper = center + model.sampleRate() / 2 + 1;

    model.setScanLowerFrequency(lower);
    model.setScanUpperFrequency(upper);
    QCOMPARE(model.scanLowerFrequency(), lower);
    QCOMPARE(model.scanUpperFrequency(), upper);
    QVERIFY(model.scanValidationError().contains(
        QStringLiteral("requires 2000001 Hz")));
    QVERIFY(model.scanValidationError().contains(
        QStringLiteral("provides 2000000 Hz")));
    QVERIFY(!model.scanCanStart());

    const quint64 originalCenter = model.centerFrequency();
    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Stopped"));
    QCOMPARE(model.centerFrequency(), originalCenter);
    QCOMPARE(model.scanLowerFrequency(), lower);
    QCOMPARE(model.scanUpperFrequency(), upper);
}

void ApplicationModelTest::centersScannerRangeWithIntegerMidpoint()
{
    ApplicationModel model;
    const quint64 originalCenter = model.centerFrequency();
    const quint64 lower = originalCenter + 5'000'000;
    const quint64 upper = lower + 3;

    model.setScanLowerFrequency(lower);
    model.setScanUpperFrequency(upper);
    model.setScanStepSize(1);
    model.setScanDwellMilliseconds(100'000);
    QVERIFY(model.scanValidationError().isEmpty());
    QVERIFY(!model.scanCanStart());
    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Stopped"));
    QVERIFY(model.scanStatusMessage().contains(
        QStringLiteral("start reception")));

    model.startReception();
    QVERIFY(model.scanCanStart());
    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Running"));
    QCOMPARE(model.centerFrequency(), lower + 1);
    QCOMPARE(model.scanLowerFrequency(), lower);
    QCOMPARE(model.scanUpperFrequency(), upper);
    QCOMPARE(model.scanCurrentFrequency(), lower);
    QCOMPARE(model.listeningFrequency(), lower);
}

void ApplicationModelTest::disablesAutoForUnavailableMeasurementAndScannerOwnership()
{
    auto unavailable = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{
            .squelchSignalStrengthDb = std::nullopt});
    ApplicationModel unavailableModel(std::move(unavailable));
    unavailableModel.startReception();
    QVERIFY(!unavailableModel.autoSquelchAvailable());
    unavailableModel.autoSquelch();
    QVERIFY(unavailableModel.statusText().contains(QStringLiteral("unavailable")));

    ApplicationModel model;
    model.startReception();
    QVERIFY(model.autoSquelchAvailable());
    const quint64 lower = model.centerFrequency() + 4'000;
    model.setScanLowerFrequency(lower);
    model.setScanUpperFrequency(lower + 20'000);
    model.setScanStepSize(10'000);
    model.setScanDwellMilliseconds(100'000);
    QVERIFY(model.scanCanStart());
    model.startScan();
    QVERIFY(model.scannerOwnsTuning());
    QVERIFY(!model.autoSquelchAvailable());
    model.autoSquelch();
    QVERIFY(model.statusText().contains(QStringLiteral("exclusive tuning")));
    model.stopScan();
}

void ApplicationModelTest::autoSquelchDoesNotMutateLoadedBookmark()
{
    auto backend = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{
            .squelchSignalStrengthDb = -60.0});
    ApplicationModel model(std::move(backend));
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
        model.bookmarkModel());
    QVERIFY(bookmarks);
    sdr::app::BookmarkData bookmark;
    bookmark.name = QStringLiteral("Saved threshold");
    bookmark.listeningFrequency = 100'000'000;
    bookmark.demodulatorId = QStringLiteral("am");
    bookmark.filterLowHz = -5'000;
    bookmark.filterHighHz = 5'000;
    bookmark.squelchThresholdDb = -80.0;
    bookmark.squelchEnabled = true;
    const QString uuid = bookmarks->addBookmark(-1, bookmark);
    QVERIFY(!uuid.isEmpty());

    model.startReception();
    model.tuneBookmark(bookmarks->visibleRowForUuid(uuid));
    QVERIFY(model.bookmarkUpdateAvailable());
    model.autoSquelch();
    QCOMPARE(model.squelchLevel(), -58.0);
    const auto unchanged = bookmarks->bookmarkAt(
        bookmarks->visibleRowForUuid(uuid));
    QVERIFY(unchanged.has_value());
    QCOMPARE(unchanged->squelchThresholdDb, -80.0);
    QVERIFY(model.bookmarkUpdateAvailable());
}

void ApplicationModelTest::givesActiveScannerExclusiveTuningControl()
{
    ApplicationModel model;
    model.startReception();
    const quint64 originalCenter = model.centerFrequency();
    QVERIFY(!model.beginAddCurrentBookmark().isEmpty());
    QVERIFY(model.confirmAddCurrentBookmark(QStringLiteral("Before scan")));

    const quint64 lower = originalCenter + 4'000'000;
    const quint64 upper = lower + 20'000;

    model.setScanLowerFrequency(lower);
    model.setScanUpperFrequency(upper);
    model.setScanStepSize(10'000);
    model.setScanDwellMilliseconds(100'000);
    model.setScanResumeDelayMilliseconds(10);
    QVERIFY(model.scanValidationError().isEmpty());
    QVERIFY(model.scanCanStart());

    model.startScan();
    QCOMPARE(model.scanState(), QStringLiteral("Running"));
    QVERIFY(model.scannerOwnsTuning());
    const quint64 scannerCenter = lower + 10'000;
    QCOMPARE(model.centerFrequency(), scannerCenter);
    QCOMPARE(model.scanCurrentFrequency(), lower);
    QCOMPARE(model.listeningFrequency(), lower);

    model.setCenterFrequencyText(QString::number(scannerCenter + 1'000'000));
    model.adjustCenterFrequencyDigit(9, 1);
    model.zeroCenterFrequencyFromDigit(9);
    model.beginCenterFrequencyDigitEdit(8);
    model.replaceCenterFrequencyDigitInEdit(5);
    model.replaceHoveredCenterFrequencyDigit(8, 5);
    model.commitCenterFrequencyDigitEdit();
    QVERIFY(!model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequencyDigitEditIndex(), -1);
    QCOMPARE(model.centerFrequencyDigitEditStartIndex(), -1);
    model.shiftCenterFromSpectrum(120);
    model.handleFrequencyWheel(false, 120);
    model.setListeningFrequency(lower + 5'000);
    model.selectListeningFrequencyAt(10.0, 100.0);
    model.handleFrequencyWheel(true, 120, Qt::ShiftModifier);
    model.tuneBookmark(0);
    QCoreApplication::processEvents();
    QCOMPARE(model.centerFrequency(), scannerCenter);
    QCOMPARE(model.listeningFrequency(), lower);

    model.pauseOrResumeScan();
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));
    model.skipScanFrequency();
    QCOMPARE(model.scanCurrentFrequency(), lower + 10'000);
    QCOMPARE(model.listeningFrequency(), lower + 10'000);
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));

    model.setCenterFrequencyText(QString::number(scannerCenter + 1'000'000));
    model.setListeningFrequency(lower + 5'000);
    QCOMPARE(model.centerFrequency(), scannerCenter);
    QCOMPARE(model.listeningFrequency(), lower + 10'000);

    const quint64 stoppedCenter = model.centerFrequency();
    const quint64 stoppedListening = model.listeningFrequency();
    model.stopScan();
    QVERIFY(!model.scannerOwnsTuning());
    QCOMPARE(model.centerFrequency(), stoppedCenter);
    QCOMPARE(model.listeningFrequency(), stoppedListening);

    model.setCenterFrequencyText(QString::number(originalCenter));
    QCOMPARE(model.centerFrequency(), originalCenter);
    model.setListeningFrequency(originalCenter + 5'000);
    QCOMPARE(model.listeningFrequency(), originalCenter + 5'000);
}

void ApplicationModelTest::persistsAndValidatesDsdFmeBinaryPath()
{
    const QString key = QStringLiteral("externalDecoder/dsdFmeBinaryPath");
    QSettings settings;
    settings.remove(key);
    settings.sync();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executablePath = directory.filePath(QStringLiteral("dsd-fme"));
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    QVERIFY(executable.write("#!/bin/sh\nexit 0\n") > 0);
    executable.close();
    QVERIFY(executable.setPermissions(
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    {
        ApplicationModel model;
        QCOMPARE(model.dsdFmeBinaryPath(), QString());
        QCOMPARE(
            model.dsdFmeBinaryStatus(),
            QStringLiteral("No DSD-FME binary configured"));
        QVERIFY(!model.dsdFmeBinaryValid());

        model.setDsdFmeBinaryPath(
            QStringLiteral("  %1  ").arg(executablePath));
        QCOMPARE(model.dsdFmeBinaryPath(), executablePath);
        QCOMPARE(model.dsdFmeBinaryStatus(), QStringLiteral("Valid executable"));
        QVERIFY(model.dsdFmeBinaryValid());

    }

    {
        ApplicationModel restored;
        QCOMPARE(restored.dsdFmeBinaryPath(), executablePath);
        QCOMPARE(restored.dsdFmeBinaryStatus(), QStringLiteral("Valid executable"));
        QVERIFY(restored.dsdFmeBinaryValid());

        restored.setDsdFmeBinaryPath(directory.filePath(QStringLiteral("missing")));
        QCOMPARE(restored.dsdFmeBinaryStatus(), QStringLiteral("File not found"));
        QVERIFY(!restored.dsdFmeBinaryValid());

        restored.setDsdFmeBinaryPath(directory.path());
        QCOMPARE(restored.dsdFmeBinaryStatus(), QStringLiteral("Not a regular file"));

        restored.setDsdFmeBinaryPath(QStringLiteral("~"));
        QCOMPARE(restored.dsdFmeBinaryPath(), QStringLiteral("~"));
        QCOMPARE(restored.dsdFmeBinaryStatus(), QStringLiteral("Not a regular file"));

        const QString nonExecutablePath = directory.filePath(QStringLiteral("not-executable"));
        QFile nonExecutable(nonExecutablePath);
        QVERIFY(nonExecutable.open(QIODevice::WriteOnly));
        nonExecutable.close();
        QVERIFY(nonExecutable.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        restored.setDsdFmeBinaryPath(nonExecutablePath);
        QCOMPARE(restored.dsdFmeBinaryStatus(), QStringLiteral("Not executable"));

        restored.setDsdFmeBinaryPath(QString());
        QCOMPARE(
            restored.dsdFmeBinaryStatus(),
            QStringLiteral("No DSD-FME binary configured"));
    }

    settings.remove(key);
    settings.sync();
}

void ApplicationModelTest::persistsAndValidatesRecordingsFolder()
{
    const QString key = QStringLiteral("recording/folder");
    const QString skipKey = QStringLiteral("recording/skipQuietParts");
    const QString preRollKey = QStringLiteral("recording/preRollSeconds");
    const QString tailKey = QStringLiteral("recording/tailSeconds");
    const QString scannerKey = QStringLiteral("recording/scannerActivity");
    QSettings settings;
    settings.remove(key);
    settings.remove(skipKey);
    settings.remove(preRollKey);
    settings.remove(tailKey);
    settings.remove(scannerKey);
    settings.sync();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString invalidPath = directory.filePath(QStringLiteral("missing"));
    {
        ApplicationModel model;
        model.setRecordingsFolder(
            QStringLiteral("  %1  ").arg(directory.path()));
        QCOMPARE(model.recordingsFolder(), QDir::cleanPath(directory.path()));
        QCOMPARE(model.recordingsFolderStatus(),
                 QStringLiteral("Ready for WAV recordings"));
        QVERIFY(model.recordingsFolderValid());
        QVERIFY(!model.skipQuietRecordingParts());
        QCOMPARE(model.recordingPreRollSeconds(), 1);
        QCOMPARE(model.recordingTailSeconds(), 2);
        QVERIFY(!model.recordScannerActivity());
        model.setSkipQuietRecordingParts(true);
        model.setRecordingPreRollSeconds(99);
        model.setRecordingTailSeconds(-2);
        model.setRecordScannerActivity(true);
    }
    {
        ApplicationModel restored;
        QCOMPARE(restored.recordingsFolder(), QDir::cleanPath(directory.path()));
        QVERIFY(restored.recordingsFolderValid());
        QVERIFY(restored.skipQuietRecordingParts());
        QCOMPARE(restored.recordingPreRollSeconds(), 10);
        QCOMPARE(restored.recordingTailSeconds(), 0);
        QVERIFY(restored.recordScannerActivity());
        restored.setRecordingsFolder(invalidPath);
        QCOMPARE(restored.recordingsFolderStatus(),
                 QStringLiteral("Folder does not exist"));
        QVERIFY(!restored.recordingsFolderValid());
    }
    settings.remove(key);
    settings.remove(skipKey);
    settings.remove(preRollKey);
    settings.remove(tailKey);
    settings.remove(scannerKey);
    settings.sync();
}

void ApplicationModelTest::namesBookmarksBeforeCreatingCapturedReceiverState()
{
    ApplicationModel model;
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
        model.bookmarkModel());
    QVERIFY(bookmarks);
    QVERIFY(QTest::qWaitFor([bookmarks] { return !bookmarks->loading(); }));

    const quint64 capturedFrequency = model.listeningFrequency();
    const QString suggestion = model.beginAddCurrentBookmark(-1);
    QVERIFY(!suggestion.isEmpty());
    QCOMPARE(bookmarks->rowCount(), 0);
    model.setListeningFrequency(capturedFrequency + 25'000);
    QVERIFY(model.confirmAddCurrentBookmark(QStringLiteral("  Edited name  ")));
    QCOMPARE(bookmarks->rowCount(), 1);
    QCOMPARE(bookmarks->itemDetails(0).value(QStringLiteral("name")).toString(),
             QStringLiteral("Edited name"));
    QCOMPARE(bookmarks->bookmarkAt(0)->listeningFrequency, capturedFrequency);

    const QString secondSuggestion = model.beginAddCurrentBookmark(-1);
    QVERIFY(!secondSuggestion.isEmpty());
    QVERIFY(!model.confirmAddCurrentBookmark(QStringLiteral("   ")));
    QCOMPARE(bookmarks->rowCount(), 1);
    QVERIFY(model.confirmAddCurrentBookmark(secondSuggestion));
    QCOMPARE(bookmarks->rowCount(), 2);

    QVERIFY(!model.beginAddCurrentBookmark(-1).isEmpty());
    QVERIFY(model.confirmAddCurrentBookmark(QStringLiteral("Edited name")));
    QCOMPARE(bookmarks->rowCount(), 3);
    QCOMPARE(bookmarks->itemDetails(2).value(QStringLiteral("name")).toString(),
             QStringLiteral("Edited name"));

    QVERIFY(!model.beginAddCurrentBookmark(-1).isEmpty());
    model.cancelAddCurrentBookmark();
    QVERIFY(!model.confirmAddCurrentBookmark(QStringLiteral("Cancelled")));
    QCOMPARE(bookmarks->rowCount(), 3);
}

void ApplicationModelTest::updatesBookmarksByStableIdentityAndPreservesMetadata()
{
    ApplicationModel model;
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
        model.bookmarkModel());
    QVERIFY(bookmarks);

    const auto add = [bookmarks](QString name, QString mode) {
        sdr::app::BookmarkData data;
        data.name = std::move(name);
        data.listeningFrequency = 100'000'000;
        data.requestedGainDb = 10.0;
        data.demodulatorId = std::move(mode);
        data.filterLowHz = 0;
        data.filterHighHz = 2'700;
        data.squelchThresholdDb = -70.0;
        data.squelchEnabled = true;
        data.modeSpecificSettings = {
            {QStringLiteral("version"), 3},
            {QStringLiteral("keep"), QStringLiteral("metadata")},
        };
        return bookmarks->addBookmark(-1, data);
    };

    const QString firstUuid = add(QStringLiteral("First"), QStringLiteral("am"));
    const QString secondUuid = add(QStringLiteral("Second"), QStringLiteral("usb"));
    const int secondRow = bookmarks->visibleRowForUuid(secondUuid);
    model.tuneBookmark(secondRow);
    QVERIFY(model.bookmarkUpdateAvailable());

    model.setListeningFrequency(101'000'000);
    model.setGain(24.0);
    model.setFilterWidth(2'700);
    model.setSquelchLevel(-55.0);
    model.disableSquelch();
    model.updateCurrentBookmark();
    QCOMPARE(model.statusText(), QStringLiteral("Bookmark updated"));

    const auto first = bookmarks->bookmarkAt(
        bookmarks->visibleRowForUuid(firstUuid));
    const auto second = bookmarks->bookmarkAt(
        bookmarks->visibleRowForUuid(secondUuid));
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QCOMPARE(first->name, QStringLiteral("First"));
    QCOMPARE(first->listeningFrequency, quint64{100'000'000});
    QCOMPARE(second->name, QStringLiteral("Second"));
    QCOMPARE(second->listeningFrequency, quint64{101'000'000});
    QCOMPARE(second->requestedGainDb, 24.0);
    QCOMPARE(second->demodulatorId, QStringLiteral("usb"));
    QCOMPARE(second->squelchThresholdDb, -55.0);
    QVERIFY(!second->squelchEnabled);
    QCOMPARE(
        second->modeSpecificSettings.value(QStringLiteral("keep")).toString(),
        QStringLiteral("metadata"));

    sdr::app::BookmarkData legacy;
    legacy.name = QStringLiteral("Legacy");
    legacy.listeningFrequency = 102'000'000;
    legacy.demodulatorId = QStringLiteral("am");
    legacy.filterHighHz = 12'500;
    legacy.hasSavedSquelch = false;
    const QString legacyUuid = bookmarks->addBookmark(-1, legacy);
    model.setSquelchLevel(-61.0);
    model.tuneBookmark(bookmarks->visibleRowForUuid(legacyUuid));
    QCOMPARE(model.squelchLevel(), -61.0);
}

void ApplicationModelTest::changesFftResolutionWithoutClearingHistoryAndRejectsOldSizeFrames()
{
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    QSignalSpy resolutionChanges(&model, &ApplicationModel::spectrumFftSizeChanged);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    QSignalSpy waterfallFrames(&model, &ApplicationModel::waterfallFrameReady);
    quint64 confirmedFftSize = 0;
    quint64 confirmedTuningGeneration = 0;
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::snapshotChanged,
        this,
        [&confirmedFftSize, &confirmedTuningGeneration](
            const sdr::app::ReceiverRuntimeSnapshot& snapshot) {
            confirmedFftSize = snapshot.effectiveSpectrumFftSize;
            confirmedTuningGeneration = snapshot.tuningGeneration;
        });

    runtime.start();
    QVERIFY(QTest::qWaitFor(
        [&confirmedFftSize] { return confirmedFftSize == 4'096; }, 1'000));
    QCOMPARE(confirmedFftSize, quint64{4'096});
    QCOMPARE(model.effectiveSpectrumFftSize(), quint64{4'096});
    QCOMPARE(model.spectrumHertzPerBin(), 2'000'000.0 / 4'096.0);
    QVERIFY(resolutionChanges.count() >= 1);
    model.setSpectrumFftSize(8'192);
    QVERIFY(QTest::qWaitFor(
        [&confirmedFftSize] { return confirmedFftSize == 8'192; }, 1'000));
    QCOMPARE(confirmedFftSize, quint64{8'192});
    QCOMPARE(model.effectiveSpectrumFftSize(), quint64{8'192});
    QVERIFY(!model.runtimeBusy());
    QCOMPARE(model.spectrumHertzPerBin(), 2'000'000.0 / 8'192.0);
    QVERIFY(spectrumResets.count() >= 1);
    QCOMPARE(waterfallResets.count(), 0);
    spectrumFrames.clear();
    waterfallFrames.clear();

    const QVector<float> oldFrame(4'096, 0.25F);
    constexpr quint64 oldSizeSequence =
        std::numeric_limits<quint64>::max() - 2;
    runtime.spectrumFrameReady(
        oldFrame,
        model.centerFrequency(),
        model.effectiveSampleRate(),
        4'096,
        oldSizeSequence,
        oldSizeSequence,
        confirmedTuningGeneration);
    runtime.waterfallFrameReady(
        oldFrame,
        model.centerFrequency(),
        model.effectiveSampleRate(),
        4'096,
        oldSizeSequence,
        oldSizeSequence,
        confirmedTuningGeneration);
    QCOMPARE(spectrumFrames.count(), 0);
    QCOMPARE(waterfallFrames.count(), 0);

    const QVector<float> currentFrame(8'192, 0.25F);
    constexpr quint64 currentSizeSequence =
        std::numeric_limits<quint64>::max() - 1;
    runtime.spectrumFrameReady(
        currentFrame,
        model.centerFrequency(),
        model.effectiveSampleRate(),
        8'192,
        currentSizeSequence,
        currentSizeSequence,
        confirmedTuningGeneration);
    QCOMPARE(spectrumFrames.count(), 1);
    runtime.shutdown();
}

void ApplicationModelTest::supportsDigitTuning()
{
    ApplicationModel model;
    QSignalSpy frequencySpy(&model, &ApplicationModel::centerFrequencyChanged);

    model.setCenterFrequencyText(QStringLiteral("199999999"));
    model.adjustCenterFrequencyDigit(9, 1);
    QCOMPARE(model.centerFrequency(), quint64{200'000'000});

    model.adjustCenterFrequencyDigit(9, -1);
    QCOMPARE(model.centerFrequency(), quint64{199'999'999});

    model.setCenterFrequencyText(QStringLiteral("145678901"));
    model.zeroCenterFrequencyFromDigit(4);
    QCOMPARE(model.centerFrequency(), quint64{145'000'000});
    QCOMPARE(model.listeningFrequency(), model.centerFrequency());
    QCOMPARE(frequencySpy.count(), 5);
}

void ApplicationModelTest::supportsCenterFrequencyDigitEditSessions()
{
    ApplicationModel model;
    QSignalSpy frequencySpy(&model, &ApplicationModel::centerFrequencyChanged);

    model.setCenterFrequencyText(QStringLiteral("123456789"));
    frequencySpy.clear();
    model.replaceHoveredCenterFrequencyDigit(4, 9);
    QCOMPARE(model.centerFrequency(), quint64{123'956'789});
    QCOMPARE(model.listeningFrequency(), quint64{123'956'789});
    QCOMPARE(frequencySpy.count(), 1);

    model.setCenterFrequencyText(QStringLiteral("100000000"));
    frequencySpy.clear();
    model.beginCenterFrequencyDigitEdit(8);
    QVERIFY(model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 8);
    QCOMPARE(model.centerFrequencyDigitEditStartIndex(), 8);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000000"));
    model.replaceCenterFrequencyDigitInEdit(4);
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 9);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000040"));
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(frequencySpy.count(), 0);
    model.adjustCenterFrequencyDigit(9, 1);
    model.adjustCenterFrequencyDigit(9, -1);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000040"));
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 9);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    model.replaceCenterFrequencyDigitInEdit(2);
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 10);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000042"));
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    model.commitCenterFrequencyDigitEdit();
    QVERIFY(!model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequencyDigitEditIndex(), -1);
    QCOMPARE(model.centerFrequencyDigitEditStartIndex(), -1);
    QCOMPARE(model.centerFrequency(), quint64{100'000'042});
    QCOMPARE(model.listeningFrequency(), quint64{100'000'042});
    QCOMPARE(frequencySpy.count(), 1);

    model.beginCenterFrequencyDigitEdit(9);
    model.replaceCenterFrequencyDigitInEdit(9);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000049"));
    model.cancelCenterFrequencyDigitEdit();
    QVERIFY(!model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000042"));
    QCOMPARE(model.centerFrequency(), quint64{100'000'042});

    model.beginCenterFrequencyDigitEdit(4);
    model.replaceCenterFrequencyDigitInEdit(7);
    QVERIFY(model.centerFrequencyDigitEditActive());
    model.zeroCenterFrequencyFromDigit(4);
    QVERIFY(!model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});

    model.setCenterFrequencyText(QStringLiteral("100000000"));
    model.beginCenterFrequencyDigitEdit(7);
    model.replaceCenterFrequencyDigitInEdit(4);
    model.replaceCenterFrequencyDigitInEdit(5);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000450"));
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 9);
    QVERIFY(model.beginCenterFrequencyDigitEdit(6) == false);
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 9);
    QVERIFY(model.beginCenterFrequencyDigitEdit(7));
    QCOMPARE(model.centerFrequencyDigitEditIndex(), 7);
    QVERIFY(model.beginCenterFrequencyDigitEdit(8));
    model.replaceCenterFrequencyDigitInEdit(6);
    model.replaceCenterFrequencyDigitInEdit(7);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000467"));
    model.beginCenterFrequencyDigitEdit(9);
    model.replaceCenterFrequencyDigitInEdit(8);
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000468"));
    model.commitCenterFrequencyDigitEdit();
    QCOMPARE(model.centerFrequency(), quint64{100'000'468});

    model.beginCenterFrequencyDigitEdit(7);
    model.replaceCenterFrequencyDigitInEdit(9);
    model.beginCenterFrequencyDigitEdit(8);
    model.replaceCenterFrequencyDigitInEdit(1);
    model.cancelCenterFrequencyDigitEdit();
    QVERIFY(!model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequencyDigitEditIndex(), -1);
    QCOMPARE(model.centerFrequencyDigitEditStartIndex(), -1);
    QCOMPARE(model.centerFrequency(), quint64{100'000'468});
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000468"));

    model.setCenterFrequencyText(QStringLiteral("100000000"));
    model.adjustCenterFrequencyDigit(9, 1);
    QCOMPARE(model.centerFrequency(), quint64{100'000'001});
    model.adjustCenterFrequencyDigit(9, -1);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
}

void ApplicationModelTest::rejectsInvalidCenterFrequencyDigitEdits()
{
    ApplicationModel model;
    model.setDeviceFrequencyRanges({{88'000'000, 108'000'000}});
    model.setCenterFrequencyText(QStringLiteral("100000000"));

    model.beginCenterFrequencyDigitEdit(1);
    model.replaceCenterFrequencyDigitInEdit(7);
    for (int index = 2;
         index < sdr::app::FrequencyDigitController::digitCount;
         ++index) {
        model.replaceCenterFrequencyDigitInEdit(0);
    }
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0700000000"));
    model.commitCenterFrequencyDigitEdit();
    QVERIFY(model.centerFrequencyDigitEditActive());
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QVERIFY(model.statusText().contains(QStringLiteral("outside")));
    model.cancelCenterFrequencyDigitEdit();
    QCOMPARE(model.centerFrequencyDigits(), QStringLiteral("0100000000"));
}

void ApplicationModelTest::honorsDeviceSpecificDigitLimits()
{
    ApplicationModel model;
    model.setDeviceFrequencyRanges({{88'000'000, 108'000'000}});

    model.setCenterFrequencyText(QStringLiteral("108000000"));
    QCOMPARE(model.centerFrequency(), quint64{108'000'000});
    QCOMPARE(model.listeningFrequency(), model.centerFrequency());

    model.adjustCenterFrequencyDigit(9, 1);
    QCOMPARE(model.centerFrequency(), quint64{108'000'000});
    QVERIFY(model.statusText().contains(QStringLiteral("device limits")));

    model.setCenterFrequencyText(QStringLiteral("50000000"));
    QCOMPARE(model.centerFrequency(), quint64{88'000'000});
    QCOMPARE(model.listeningFrequency(), model.centerFrequency());
    QVERIFY(model.statusText().contains(QStringLiteral("device limits")));

    model.setCenterFrequencyText(QStringLiteral("100123456"));
    model.zeroCenterFrequencyFromDigit(1);
    QCOMPARE(model.centerFrequency(), quint64{100'123'456});
    QVERIFY(model.statusText().contains(QStringLiteral("Zeroing")));

    model.clearDeviceFrequencyRanges();
    model.setCenterFrequencyText(QStringLiteral("50000000"));
    QCOMPARE(model.centerFrequency(), quint64{50'000'000});
}

void ApplicationModelTest::enforcesFrequencyLimits()
{
    ApplicationModel model;
    QSignalSpy frequencySpy(&model, &ApplicationModel::centerFrequencyChanged);

    model.setCenterFrequencyText(QStringLiteral("not a frequency"));
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(frequencySpy.count(), 0);

    model.setCenterFrequencyText(QStringLiteral("0"));
    QCOMPARE(model.centerFrequency(), quint64{1'000'000});
    QCOMPARE(frequencySpy.count(), 1);

    model.adjustCenterFrequencyDigit(9, -1);
    QCOMPARE(model.centerFrequency(), quint64{1'000'000});
    QCOMPARE(frequencySpy.count(), 1);

    model.setCenterFrequencyText(QStringLiteral("9999999999"));
    QCOMPARE(model.centerFrequency(), quint64{9'998'999'999});
    QCOMPARE(frequencySpy.count(), 2);

    model.adjustCenterFrequencyDigit(9, 1);
    QCOMPARE(model.centerFrequency(), quint64{9'998'999'999});
    QCOMPARE(frequencySpy.count(), 2);
}

void ApplicationModelTest::keepsSpectrumTuningDistinctFromWaterfallZoom()
{
    ApplicationModel model;

    model.selectListeningFrequencyAt(75.0, 100.0);
    QCOMPARE(model.listeningFrequency(), quint64{100'500'000});
    QCOMPARE(model.listeningPosition(), 0.75);

    model.requestWaterfallZoom(120);
    QElapsedTimer zoomTimer;
    zoomTimer.start();
    while (model.displayZoomFactor() <= 1.0 && zoomTimer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QVERIFY(model.displayZoomFactor() > 1.0);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'500'000});
    QVERIFY(std::abs(model.listeningPosition() - 0.75) < 0.001);
    const double zoomFactor = model.displayZoomFactor();

    model.shiftCenterFromSpectrum(120);
    QCOMPARE(model.centerFrequency(), quint64{100'010'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'010'000});
    QCOMPARE(model.listeningPosition(), 0.5);
    QVERIFY(std::abs(model.displayZoomFactor() - zoomFactor) < 0.001);

    model.shiftCenterFromSpectrum(-120);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningPosition(), 0.5);
}

void ApplicationModelTest::reportsDisplayZoomPercentage()
{
    ApplicationModel model;
    QSignalSpy viewportChanges(
        &model, &ApplicationModel::visibleRangeChanged);

    QCOMPARE(model.displayZoomPercentage(), quint64{100});

    model.requestWaterfallZoom(240);
    QVERIFY(QTest::qWaitFor(
        [&model] { return model.displayZoomPercentage() > 100; }, 500));
    QCOMPARE(
        model.displayZoomPercentage(),
        static_cast<quint64>(std::llround(model.displayZoomFactor() * 100.0)));
    const auto afterZoom = viewportChanges.count();

    model.setSampleRate(1'000'000);
    QVERIFY(viewportChanges.count() > afterZoom);
    QCOMPARE(
        model.displayZoomPercentage(),
        static_cast<quint64>(std::llround(model.displayZoomFactor() * 100.0)));

    const auto afterCaptureBandwidth = viewportChanges.count();
    model.setCenterFrequencyText(QStringLiteral("101000000"));
    QVERIFY(viewportChanges.count() > afterCaptureBandwidth);
    QCOMPARE(
        model.displayZoomPercentage(),
        static_cast<quint64>(std::llround(model.displayZoomFactor() * 100.0)));

    sdr::radio::ReceiverLimits limits;
    limits.frequency = {500'000, 1'766'000'000};
    limits.allowsPartialPassbandAtFrequencyEdges = true;
    auto backend = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{}, limits);
    ApplicationModel limitedModel(std::move(backend));
    limitedModel.setDeviceFrequencyRanges({limits.frequency});
    limitedModel.setSampleRate(2'400'000);
    limitedModel.setCenterFrequencyText(QStringLiteral("500000"));
    limitedModel.requestWaterfallZoom(120);
    QVERIFY(QTest::qWaitFor(
        [&limitedModel] { return limitedModel.displayZoomPercentage() > 100; },
        500));
    QSignalSpy deviceLimitChanges(
        &limitedModel, &ApplicationModel::visibleRangeChanged);
    const auto beforeDeviceLimits = deviceLimitChanges.count();
    limitedModel.setDeviceFrequencyRanges({{500'000, 1'200'000}});
    QVERIFY(deviceLimitChanges.count() > beforeDeviceLimits);
    QCOMPARE(
        limitedModel.displayZoomPercentage(),
        static_cast<quint64>(
            std::llround(limitedModel.displayZoomFactor() * 100.0)));
}

void ApplicationModelTest::pansSharedViewportWithoutChangingReceiverTuning()
{
    ApplicationModel model;
    model.requestWaterfallZoom(240);
    QVERIFY(QTest::qWaitFor(
        [&model] { return model.displayPanEnabled(); }, 500));

    const quint64 center = model.centerFrequency();
    const quint64 listening = model.listeningFrequency();
    const quint64 visibleSpan = model.visibleSpan();
    const double zoom = model.displayZoomFactor();
    QSignalSpy centerChanges(&model, &ApplicationModel::centerFrequencyChanged);
    QSignalSpy listeningChanges(
        &model, &ApplicationModel::listeningFrequencyChanged);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);

    QVERIFY(std::abs(
                model.displayPanPageSize() -
                static_cast<double>(visibleSpan) /
                    static_cast<double>(model.sampleRate())) < 0.000001);
    model.setDisplayPanPosition(-1.0);
    QCOMPARE(model.visibleLowerFrequency(), center - model.sampleRate() / 2);
    QCOMPARE(model.displayPanPosition(), 0.0);
    model.setDisplayPanPosition(1.0);
    QCOMPARE(model.visibleUpperFrequency(), center + model.sampleRate() / 2);
    QCOMPARE(model.displayPanPosition(), 1.0);
    QCOMPARE(model.visibleSpan(), visibleSpan);
    QCOMPARE(model.displayZoomFactor(), zoom);
    QCOMPARE(model.centerFrequency(), center);
    QCOMPARE(model.listeningFrequency(), listening);
    QCOMPARE(centerChanges.count(), 0);
    QCOMPARE(listeningChanges.count(), 0);
    QCOMPARE(waterfallResets.count(), 0);

    const auto axis = model.frequencyViewportAxis({0.0, 1'000.0});
    QCOMPARE(
        axis.roundedFrequencyForPosition(0.0).value(),
        model.visibleLowerFrequency());
    QCOMPARE(
        axis.roundedFrequencyForPosition(1'000.0).value(),
        model.visibleUpperFrequency());
}

void ApplicationModelTest::
    preservesViewportPanAcrossCaptureChangesAndScannerActivity()
{
    sdr::app::FrequencyViewport viewport(
        100'000'000, 2'000'000, 4'096, 12'500);
    QVERIFY(viewport.zoomBySteps(100'000'000, 2.0));
    QVERIFY(viewport.setPanPosition(0.25));
    const quint64 oldCenter = viewport.visibleCenter();
    QVERIFY(viewport.configureCapture(
        100'000'000,
        2'400'000,
        100'000'000,
        {0, std::numeric_limits<quint64>::max()},
        true));
    QCOMPARE(viewport.visibleCenter(), oldCenter);
    QVERIFY(viewport.panPosition() > 0.0);
    QVERIFY(viewport.panPosition() < 1.0);

    ApplicationModel model;
    model.requestWaterfallZoom(240);
    QVERIFY(QTest::qWaitFor(
        [&model] { return model.displayPanEnabled(); }, 500));
    model.startReception();
    model.setScanLowerFrequency(model.centerFrequency() - 100'000);
    model.setScanUpperFrequency(model.centerFrequency() + 100'000);
    model.setScanStepSize(25'000);
    model.setScanDwellMilliseconds(100'000);
    QVERIFY(model.scanCanStart());
    model.startScan();
    QVERIFY(model.scannerOwnsTuning());
    const quint64 scannerCenter = model.centerFrequency();
    const quint64 scannerListening = model.listeningFrequency();
    const quint64 scannerPosition = model.scanCurrentFrequency();
    model.setDisplayPanPosition(1.0);
    QCOMPARE(model.centerFrequency(), scannerCenter);
    QCOMPARE(model.listeningFrequency(), scannerListening);
    QCOMPARE(model.scanCurrentFrequency(), scannerPosition);
    QVERIFY(model.scannerOwnsTuning());
    model.stopScan();
}

void ApplicationModelTest::anchorsWaterfallZoomAtListeningFrequency()
{
    sdr::app::FrequencyViewport viewport(
        100'000'000, 2'000'000, 4'096, 12'500);
    const quint64 listening = 100'500'000;
    const double anchor = viewport.normalizedPosition(listening);

    QVERIFY(viewport.zoomBySteps(listening, 1.0));
    QVERIFY(std::abs(viewport.normalizedPosition(listening) - anchor) < 0.000001);
    QCOMPARE(viewport.visibleSpan(), quint64{1'666'667});

    QVERIFY(viewport.zoomBySteps(listening, -1.0));
    QCOMPARE(viewport.visibleSpan(), quint64{2'000'000});
    QCOMPARE(viewport.visibleRange().minimum, quint64{99'000'000});
    QCOMPARE(viewport.visibleRange().maximum, quint64{101'000'000});
}

void ApplicationModelTest::clampsZoomAndViewport()
{
    sdr::app::FrequencyViewport viewport(
        100'000'000, 2'000'000, 4'096, 12'500);
    QVERIFY(viewport.zoomBySteps(99'000'000, 80.0));
    QCOMPARE(viewport.visibleRange().minimum, quint64{99'000'000});
    QVERIFY(viewport.visibleSpan() >= viewport.minimumVisibleSpan());
    QVERIFY(viewport.visibleSpan() >= quint64{12'500});
    QVERIFY(viewport.zoomBySteps(99'000'000, -80.0));
    QCOMPARE(viewport.visibleSpan(), quint64{2'000'000});
    QVERIFY(!viewport.zoomBySteps(100'000'000, -1.0));

    sdr::app::FrequencyViewport upperEdgeViewport(
        100'000'000, 2'000'000, 4'096, 12'500);
    QVERIFY(upperEdgeViewport.zoomBySteps(101'000'000, 80.0));
    QCOMPARE(
        upperEdgeViewport.visibleRange().maximum, quint64{101'000'000});

    sdr::app::FrequencyViewport centeredViewport(
        100'000'000, 2'000'000, 4'096, 12'500);
    QVERIFY(centeredViewport.zoomBySteps(100'000'000, 2.0));
    const quint64 centeredSpan = centeredViewport.visibleSpan();
    QVERIFY(centeredViewport.centerOn(101'000'000));
    QCOMPARE(
        centeredViewport.visibleRange().maximum, quint64{101'000'000});
    QCOMPARE(centeredViewport.visibleSpan(), centeredSpan);
    QVERIFY(!centeredViewport.centerOn(101'000'000));

    sdr::app::FrequencyViewport usbViewport(
        100'000'000, 2'000'000, 65'536, 2'400);
    QVERIFY(!usbViewport.configureDetail(
        65'536,
        2'400,
        100'000'000,
        false,
        sdr::app::FrequencyViewport::PassbandAlignment::Upper));
    QVERIFY(usbViewport.zoomBySteps(100'000'000, 80.0));
    QCOMPARE(usbViewport.visibleRange().minimum, quint64{100'000'000});
    QCOMPARE(usbViewport.visibleRange().maximum, quint64{100'002'400});

    sdr::app::FrequencyViewport lsbViewport(
        100'000'000, 2'000'000, 65'536, 2'400);
    QVERIFY(!lsbViewport.configureDetail(
        65'536,
        2'400,
        100'000'000,
        false,
        sdr::app::FrequencyViewport::PassbandAlignment::Lower));
    QVERIFY(lsbViewport.zoomBySteps(100'000'000, 80.0));
    QCOMPARE(lsbViewport.visibleRange().minimum, quint64{99'997'600});
    QCOMPARE(lsbViewport.visibleRange().maximum, quint64{100'000'000});

    sdr::app::FrequencyViewport highResolutionViewport(
        100'000'000, 2'000'000, 262'144, 1);
    QCOMPARE(highResolutionViewport.minimumVisibleSpan(), quint64{245});
    QVERIFY(highResolutionViewport.zoomBySteps(100'000'000, 80.0));
    QCOMPARE(highResolutionViewport.visibleSpan(), quint64{245});
}

void ApplicationModelTest::supportsPartialPassbandAtDeviceRfLimit()
{
    sdr::radio::ReceiverLimits limits;
    limits.frequency = {500'000, 1'766'000'000};
    limits.allowsPartialPassbandAtFrequencyEdges = true;
    auto backend = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{},
        limits);
    ApplicationModel model(std::move(backend));
    model.setDeviceFrequencyRanges({limits.frequency});
    model.setSampleRate(2'400'000);
    model.setCenterFrequencyText(QStringLiteral("500000"));

    QCOMPARE(model.centerFrequency(), quint64{500'000});
    QCOMPARE(model.listeningFrequency(), quint64{500'000});
    QCOMPARE(model.visibleLowerFrequency(), quint64{500'000});
    QCOMPARE(model.visibleUpperFrequency(), quint64{1'700'000});
    QCOMPARE(model.visibleSpan(), quint64{1'200'000});
    QCOMPARE(model.listeningPosition(), 0.0);

    const auto axis = model.frequencyViewportAxis({0.0, 120.0});
    QVERIFY(axis.valid());
    QCOMPARE(
        axis.roundedFrequencyForPosition(0.0),
        std::optional<std::uint64_t>{500'000});
    QCOMPARE(
        axis.roundedFrequencyForPosition(120.0),
        std::optional<std::uint64_t>{1'700'000});

    model.setListeningFrequency(499'999);
    QCOMPARE(model.listeningFrequency(), quint64{500'000});
    QVERIFY(model.statusText().contains(QStringLiteral("outside")));

    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    model.startReception();
    QVERIFY(QTest::qWaitFor(
        [&spectrumFrames] { return spectrumFrames.count() > 0; }, 500));
    const auto publishedFrame = spectrumFrames.constLast();
    QCOMPARE(publishedFrame.at(1).toULongLong(), quint64{500'000});
    QCOMPARE(publishedFrame.at(2).toULongLong(), quint64{2'400'000});
    const auto magnitudes = publishedFrame.at(0).value<QVector<float>>();
    QVERIFY(std::any_of(
        magnitudes.begin(),
        magnitudes.end(),
        [](float magnitude) { return magnitude > 0.0F; }));
}

void ApplicationModelTest::coalescesFractionalWaterfallZoomInput()
{
    ApplicationModel model;
    QSignalSpy viewportChanges(
        &model, &ApplicationModel::frequencyViewportChanged);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    for (int event = 0; event < 4; ++event) {
        model.requestWaterfallZoom(30);
    }
    QElapsedTimer timer;
    timer.start();
    while (viewportChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(viewportChanges.count(), 1);
    QVERIFY(std::abs(model.displayZoomFactor() - 1.2) < 0.001);
    QTest::qWait(40);
    QCOMPARE(viewportChanges.count(), 1);
    QCOMPARE(waterfallResets.count(), 0);
}

void ApplicationModelTest::preservesOrClampsZoomAcrossCaptureAndFftChanges()
{
    ApplicationModel model;
    model.requestWaterfallZoom(240);
    QElapsedTimer timer;
    timer.start();
    while (model.displayZoomFactor() <= 1.0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    const double zoomFactor = model.displayZoomFactor();

    model.setSampleRate(1'000'000);
    QVERIFY(std::abs(model.displayZoomFactor() - zoomFactor) < 0.001);
    QVERIFY(model.visibleLowerFrequency() >= quint64{99'500'000});
    QVERIFY(model.visibleUpperFrequency() <= quint64{100'500'000});

    model.requestWaterfallZoom(9'600);
    timer.restart();
    while (model.visibleSpan() > model.filterWidth() &&
           timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(model.visibleSpan(), model.filterWidth());
    model.setSpectrumFftSize(1'024);
    QCOMPARE(model.visibleSpan(), quint64{31'250});
    QVERIFY(model.displayZoomFactor() <= 32.0);
}

void ApplicationModelTest::clicksWaterfallWithoutChangingCenterFrequency()
{
    ApplicationModel model;

    model.selectListeningFrequencyAt(200.0, 800.0);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{99'500'000});
    QCOMPARE(model.listeningPosition(), 0.25);

    model.requestWaterfallZoom(120);
    QElapsedTimer timer;
    timer.start();
    while (model.displayZoomFactor() <= 1.0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    const quint64 beforePan = model.visibleCenterFrequency();
    model.handleFrequencyWheel(false, 120, Qt::ShiftModifier);
    timer.restart();
    while (model.visibleCenterFrequency() == beforePan &&
           timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    const quint64 expected = model.frequencyViewportAxis({0.0, 800.0})
                                 .roundedFrequencyForPosition(600.0)
                                 .value();
    model.selectListeningFrequencyAt(600.0, 800.0);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), expected);
    const double listeningPosition = model.listeningPosition();
    const quint64 span = model.visibleSpan();
    model.handleFrequencyWheel(true, 120, Qt::NoModifier);
    timer.restart();
    while (model.visibleSpan() == span && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QVERIFY(
        std::abs(model.listeningPosition() - listeningPosition) < 0.000001);
}

void ApplicationModelTest::mapsThreeLineOverlayPositionsAndClipsEdges()
{
    ApplicationModel model;

    QCOMPARE(model.listeningPosition(), 0.5);
    QVERIFY(std::abs(model.filterLowerPosition() - 0.496875) < 0.000001);
    QVERIFY(std::abs(model.filterUpperPosition() - 0.503125) < 0.000001);

    model.selectListeningFrequencyAt(25.0, 100.0);
    QCOMPARE(model.listeningPosition(), 0.25);
    QVERIFY(std::abs(model.filterLowerPosition() - 0.246875) < 0.000001);
    QVERIFY(std::abs(model.filterUpperPosition() - 0.253125) < 0.000001);

    model.setFilterWidth(15'000);
    QCOMPARE(model.filterWidth(), quint64{15'000});
    QVERIFY(std::abs(model.filterLowerPosition() - 0.24625) < 0.000001);
    QVERIFY(std::abs(model.filterUpperPosition() - 0.25375) < 0.000001);

    model.selectListeningFrequencyAt(0.0, 100.0);
    QCOMPARE(model.listeningPosition(), 0.0);
    QCOMPARE(model.filterLowerPosition(), 0.0);
    QVERIFY(std::abs(model.filterUpperPosition() - 0.00375) < 0.000001);
}

void ApplicationModelTest::supportsConfigurableSpectrumTuning()
{
    ApplicationModel model;
    model.setTuningWheelStep(20'000);

    model.handleFrequencyWheel(false, 120, Qt::NoModifier);
    QCOMPARE(model.centerFrequency(), quint64{100'020'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'020'000});

    model.handleFrequencyWheel(false, -120, Qt::AltModifier);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'000'000});
}

void ApplicationModelTest::acceleratesRapidSpectrumWheelTuning()
{
    ApplicationModel model;
    quint64 timestamp = 0;
    model.setWheelClockForTests([&timestamp] { return timestamp; });

    model.handleFrequencyWheelWithDeltas(false, 120, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'010'000});

    timestamp = 100'000'000;
    model.handleFrequencyWheelWithDeltas(false, 120, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'030'000});

    timestamp = 150'000'000;
    model.handleFrequencyWheelWithDeltas(false, 120, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'080'000});

    timestamp = 170'000'000;
    model.handleFrequencyWheelWithDeltas(false, 120, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'180'000});

    timestamp = 600'000'000;
    model.handleFrequencyWheelWithDeltas(false, 120, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'190'000});

    timestamp = 610'000'000;
    model.handleFrequencyWheelWithDeltas(false, -120, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'180'000});

    model.setTuningWheelStep(1'000'000'000);
    model.setCenterFrequencyText(QStringLiteral("9998000000"));
    timestamp = 1'020'000'000;
    model.handleFrequencyWheelWithDeltas(false, 120, 0);
    QCOMPARE(model.centerFrequency(), quint64{9'998'999'999});
}

void ApplicationModelTest::normalizesHighResolutionAndTouchpadWheelInput()
{
    ApplicationModel model;
    quint64 timestamp = 0;
    model.setWheelClockForTests([&timestamp] { return timestamp; });

    model.handleFrequencyWheelWithDeltas(false, 30, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    timestamp = 400'000'000;
    model.handleFrequencyWheelWithDeltas(false, 90, 0);
    QCOMPARE(model.centerFrequency(), quint64{100'010'000});

    timestamp = 800'000'000;
    model.handleFrequencyWheelWithDeltas(false, 0, 20);
    QCOMPARE(model.centerFrequency(), quint64{100'010'000});
    timestamp = 1'200'000'000;
    model.handleFrequencyWheelWithDeltas(false, 0, 20);
    QCOMPARE(model.centerFrequency(), quint64{100'020'000});

    timestamp = 1'600'000'000;
    model.handleFrequencyWheelWithDeltas(false, 0, 200);
    QCOMPARE(model.centerFrequency(), quint64{100'050'000});
}

void ApplicationModelTest::routesModifierWheelActionsWithoutStateLeakage()
{
    ApplicationModel model;
    QSignalSpy centerChanges(&model, &ApplicationModel::centerFrequencyChanged);
    QSignalSpy listeningChanges(
        &model, &ApplicationModel::listeningFrequencyChanged);
    QSignalSpy filterChanges(&model, &ApplicationModel::filterWidthChanged);
    QSignalSpy viewportChanges(
        &model, &ApplicationModel::frequencyViewportChanged);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);

    model.handleFrequencyWheel(false, 120, Qt::NoModifier);
    QCOMPARE(centerChanges.count(), 1);
    QCOMPARE(listeningChanges.count(), 1);
    QCOMPARE(filterChanges.count(), 0);

    const quint64 tunedCenter = model.centerFrequency();
    const quint64 tunedListening = model.listeningFrequency();
    const quint64 originalSpan = model.visibleSpan();
    const quint64 originalVisibleCenter = model.visibleCenterFrequency();
    model.handleFrequencyWheel(
        false,
        120,
        Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    QElapsedTimer timer;
    timer.start();
    while (filterChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(filterChanges.count(), 1);
    QCOMPARE(model.filterWidth(), quint64{13'000});
    QCOMPARE(model.centerFrequency(), tunedCenter);
    QCOMPARE(model.listeningFrequency(), tunedListening);
    QCOMPARE(model.visibleSpan(), originalSpan);
    QCOMPARE(model.visibleCenterFrequency(), originalVisibleCenter);
    QCOMPARE(viewportChanges.count(), 1);

    filterChanges.clear();
    model.handleFrequencyWheel(true, -120, Qt::ControlModifier);
    timer.restart();
    while (filterChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(filterChanges.count(), 1);
    QCOMPARE(model.filterWidth(), quint64{12'500});
    QCOMPARE(model.centerFrequency(), tunedCenter);
    QCOMPARE(model.listeningFrequency(), tunedListening);
    QCOMPARE(model.visibleSpan(), originalSpan);
    QCOMPARE(model.visibleCenterFrequency(), originalVisibleCenter);
    QCOMPARE(waterfallResets.count(), 0);
}

void ApplicationModelTest::shiftWheelTunesListeningAndRecentersWhenZoomed()
{
    ApplicationModel model;
    model.setTuningWheelStep(20'000);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    QSignalSpy viewportChanges(
        &model, &ApplicationModel::frequencyViewportChanged);

    const quint64 center = model.centerFrequency();
    const quint64 filterWidth = model.filterWidth();
    const quint64 fullSpan = model.visibleSpan();
    const quint64 fullSpanCenter = model.visibleCenterFrequency();
    model.handleFrequencyWheel(false, 120, Qt::ShiftModifier);
    QCOMPARE(model.centerFrequency(), center);
    QCOMPARE(model.listeningFrequency(), center + 20'000);
    QCOMPARE(model.filterWidth(), filterWidth);
    QCOMPARE(model.visibleSpan(), fullSpan);
    QCOMPARE(model.visibleCenterFrequency(), fullSpanCenter);
    QCOMPARE(viewportChanges.count(), 0);

    model.handleFrequencyWheel(true, -120, Qt::ShiftModifier);
    QCOMPARE(model.centerFrequency(), center);
    QCOMPARE(model.listeningFrequency(), center);
    QCOMPARE(model.visibleCenterFrequency(), fullSpanCenter);

    model.handleFrequencyWheel(true, 240, Qt::NoModifier);
    QElapsedTimer timer;
    timer.start();
    while (model.displayZoomFactor() <= 1.0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    const quint64 span = model.visibleSpan();
    const double zoom = model.displayZoomFactor();

    viewportChanges.clear();
    model.handleFrequencyWheel(false, 120, Qt::ShiftModifier);
    QCOMPARE(model.centerFrequency(), center);
    QCOMPARE(model.listeningFrequency(), center + 20'000);
    QCOMPARE(model.visibleCenterFrequency(), center + 20'000);
    QCOMPARE(model.visibleSpan(), span);
    QCOMPARE(model.displayZoomFactor(), zoom);
    QCOMPARE(model.filterWidth(), filterWidth);
    QCOMPARE(model.listeningPosition(), 0.5);
    QCOMPARE(viewportChanges.count(), 1);

    model.handleFrequencyWheel(true, -120, Qt::ShiftModifier);
    QCOMPARE(model.centerFrequency(), center);
    QCOMPARE(model.listeningFrequency(), center);
    QCOMPARE(model.visibleCenterFrequency(), center);
    QCOMPARE(model.visibleSpan(), span);
    QCOMPARE(model.displayZoomFactor(), zoom);

    model.setTuningWheelStep(1'000'000);
    model.handleFrequencyWheel(true, 120, Qt::ShiftModifier);
    QCOMPARE(model.listeningFrequency(), quint64{101'000'000});
    QCOMPARE(model.visibleUpperFrequency(), quint64{101'000'000});
    QCOMPARE(model.visibleSpan(), span);
    QCOMPARE(model.displayZoomFactor(), zoom);

    model.requestWaterfallZoom(-240);
    QVERIFY(QTest::qWaitFor(
        [&model] { return model.displayZoomPercentage() == 100; }, 500));
    viewportChanges.clear();
    model.handleFrequencyWheel(true, -120, Qt::ShiftModifier);
    QCOMPARE(model.listeningFrequency(), center);
    QCOMPARE(model.centerFrequency(), center);
    QCOMPARE(model.visibleSpan(), fullSpan);
    QCOMPARE(model.visibleCenterFrequency(), fullSpanCenter);
    QCOMPARE(viewportChanges.count(), 0);
    QCOMPARE(waterfallResets.count(), 0);
}

void ApplicationModelTest::accumulatesFilterWheelStepsAndPreservesSidebands()
{
    ApplicationModel model;
    QSignalSpy filterChanges(&model, &ApplicationModel::filterWidthChanged);
    QSignalSpy viewportChanges(
        &model, &ApplicationModel::frequencyViewportChanged);

    for (int event = 0; event < 4; ++event) {
        model.handleFrequencyWheel(false, 30, Qt::ControlModifier);
    }
    QElapsedTimer timer;
    timer.start();
    while (filterChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(filterChanges.count(), 1);
    QCOMPARE(model.filterWidth(), quint64{13'000});
    QCOMPARE(viewportChanges.count(), 0);

    model.handleFrequencyWheel(true, 10, Qt::ControlModifier);
    QTest::qWait(40);
    QCOMPARE(filterChanges.count(), 1);
    QCOMPARE(model.filterWidth(), quint64{13'000});
    model.handleFrequencyWheel(true, -120, Qt::ControlModifier);
    timer.restart();
    while (filterChanges.count() == 1 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(filterChanges.count(), 2);
    QCOMPARE(model.filterWidth(), quint64{12'500});

    model.setFilterWidth(model.maximumFilterWidth());
    filterChanges.clear();
    model.handleFrequencyWheel(false, 120, Qt::ControlModifier);
    QTest::qWait(40);
    QCOMPARE(filterChanges.count(), 0);
    QCOMPARE(model.filterWidth(), model.maximumFilterWidth());

    model.setDemodulationModeIndex(
        static_cast<int>(sdr::radio::DemodulationMode::Wfm));
    QCOMPARE(model.filterWidth(), quint64{180'000});
    filterChanges.clear();
    model.handleFrequencyWheel(true, 120, Qt::ControlModifier);
    timer.restart();
    while (filterChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(model.filterWidth(), quint64{190'000});
    model.setFilterWidth(model.minimumFilterWidth());
    filterChanges.clear();
    model.handleFrequencyWheel(false, -120, Qt::ControlModifier);
    QTest::qWait(40);
    QCOMPARE(filterChanges.count(), 0);
    QCOMPARE(model.filterWidth(), model.minimumFilterWidth());

    model.setDemodulationModeIndex(
        static_cast<int>(sdr::radio::DemodulationMode::Usb));
    const QStringList usbPresets = model.filterWidthOptions();
    filterChanges.clear();
    model.handleFrequencyWheel(true, 120, Qt::ControlModifier);
    timer.restart();
    while (filterChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(model.filterWidth(), quint64{2'500});
    QCOMPARE(model.filterLowerFrequency(), model.listeningFrequency());
    QCOMPARE(
        model.filterUpperFrequency(),
        model.listeningFrequency() + model.filterWidth());
    QCOMPARE(model.filterWidthOptions(), usbPresets);
    QVERIFY(!model.filterWidthOptions().contains(QStringLiteral("2.5 kHz")));

    model.setDemodulationModeIndex(
        static_cast<int>(sdr::radio::DemodulationMode::Lsb));
    model.setFilterWidth(2'400);
    QCOMPARE(model.filterWidth(), quint64{2'400});
    filterChanges.clear();
    model.handleFrequencyWheel(false, 120, Qt::ControlModifier);
    timer.restart();
    while (filterChanges.count() == 0 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(model.filterWidth(), quint64{2'500});
    QCOMPARE(model.filterUpperFrequency(), model.listeningFrequency());
    QCOMPARE(
        model.filterLowerFrequency(),
        model.listeningFrequency() - model.filterWidth());
}

void ApplicationModelTest::preservesDisplayFramesDuringOrdinaryRetunes()
{
    ApplicationModel model;
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);

    model.shiftCenterFromSpectrum(120);
    model.adjustCenterFrequencyDigit(9, 1);
    model.setCenterFrequencyText(QStringLiteral("101000000"));
    QCOMPARE(spectrumResets.count(), 0);
    QCOMPARE(waterfallResets.count(), 0);

    model.setSampleRate(1'000'000);
    QVERIFY(spectrumResets.count() >= 1);
    QCOMPARE(waterfallResets.count(), 0);
}

void ApplicationModelTest::keepsSpectrumLiveDuringContinuousTuning()
{
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    const quint64 initialCenter = model.centerFrequency();
    const QVector<float> populatedFrame(4'096, 0.25F);
    const auto publishFrame =
        [&runtime, &model, &populatedFrame](
            quint64 centerFrequency,
            quint64 sequence,
            quint64 timestampNanoseconds,
            quint64 tuningGeneration) {
            runtime.spectrumFrameReady(
                populatedFrame,
                centerFrequency,
                model.effectiveSampleRate(),
                4'096,
                sequence,
                timestampNanoseconds,
                tuningGeneration);
        };

    publishFrame(initialCenter, 1, 100, 0);
    QCOMPARE(spectrumFrames.count(), 1);

    for (quint64 request = 1; request <= 4; ++request) {
        model.shiftCenterFromSpectrum(120);
        QCOMPARE(
            model.centerFrequency(),
            initialCenter + request * 10'000);
        if (request > 1) {
            const quint64 appliedCenter =
                initialCenter + (request - 1) * 10'000;
            publishFrame(
                appliedCenter,
                request,
                request * 100,
                request - 1);
            QCOMPARE(spectrumFrames.count(), static_cast<qsizetype>(request));
            QCOMPARE(
                spectrumFrames.last().at(1).toULongLong(),
                static_cast<qulonglong>(appliedCenter));
            QVERIFY(
                spectrumFrames.last().at(1).toULongLong() !=
                model.centerFrequency());
        }
    }
    QCOMPARE(spectrumResets.count(), 0);

    const qsizetype acceptedFrames = spectrumFrames.count();
    publishFrame(initialCenter + 20'000, 3, 300, 2);
    QCOMPARE(spectrumFrames.count(), acceptedFrames);

    publishFrame(initialCenter - 10'000'000, 5, 500, 5);
    QCOMPARE(spectrumFrames.count(), acceptedFrames);

    runtime.spectrumFrameReady(
        {},
        model.centerFrequency(),
        model.effectiveSampleRate(),
        4'096,
        5,
        500,
        4);
    QCOMPARE(spectrumFrames.count(), acceptedFrames);

    publishFrame(model.centerFrequency(), 5, 500, 4);
    QCOMPARE(spectrumFrames.count(), acceptedFrames + 1);
    QCOMPARE(
        spectrumFrames.last().at(1).toULongLong(),
        static_cast<qulonglong>(model.centerFrequency()));
    for (const auto& arguments : spectrumFrames) {
        QVERIFY(arguments.front().value<QVector<float>>().size() >= 2);
    }
    runtime.shutdown();
}

void ApplicationModelTest::coalescesRapidRuntimeWheelTuningRequests()
{
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);
    QSignalSpy centerRequests(
        &runtime,
        &sdr::app::ReceiverRuntime::setCenterFrequencyRequested);
    QSignalSpy listeningRequests(
        &runtime,
        &sdr::app::ReceiverRuntime::setListeningFrequencyRequested);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    runtime.start();
    QElapsedTimer timer;
    timer.start();
    while (!model.backendReady() && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QVERIFY(model.backendReady());
    const quint64 initialCenter = model.centerFrequency();
    spectrumResets.clear();
    spectrumFrames.clear();

    for (int event = 0; event < 4; ++event) {
        model.shiftCenterFromSpectrum(30);
    }
    QCOMPARE(model.centerFrequency(), initialCenter + 10'000);
    QCOMPARE(model.listeningFrequency(), initialCenter + 10'000);
    const QVector<float> appliedFrame(4'096, 0.25F);
    runtime.spectrumFrameReady(
        appliedFrame,
        initialCenter,
        model.effectiveSampleRate(),
        4'096,
        1,
        1,
        0);
    QCOMPARE(spectrumFrames.count(), 1);
    QCOMPARE(
        spectrumFrames.last().at(1).toULongLong(),
        static_cast<qulonglong>(initialCenter));
    QCOMPARE(spectrumResets.count(), 0);
    for (int event = 0; event < 19; ++event) {
        model.shiftCenterFromSpectrum(120);
    }
    QCOMPARE(model.centerFrequency(), initialCenter + 200'000);
    QCOMPARE(model.listeningFrequency(), initialCenter + 200'000);
    QCOMPARE(spectrumResets.count(), 0);
    timer.start();
    while (centerRequests.count() < 1 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(centerRequests.count(), 1);
    QCOMPARE(
        centerRequests.front().front().toULongLong(),
        static_cast<qulonglong>(initialCenter + 200'000));
    timer.restart();
    while (model.runtimeBusy() && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(spectrumResets.count(), 0);

    for (int event = 0; event < 20; ++event) {
        model.handleFrequencyWheel(true, 120, Qt::ShiftModifier);
    }
    timer.restart();
    while (listeningRequests.count() < 1 && timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    QCOMPARE(listeningRequests.count(), 1);
    QCOMPARE(
        listeningRequests.front().front().toULongLong(),
        initialCenter + 400'000);
    for (const auto& arguments : spectrumFrames) {
        QVERIFY(arguments.front().value<QVector<float>>().size() >= 2);
    }
    runtime.shutdown();
}

void ApplicationModelTest::recentersListeningFrequencyAfterDirectCenterChange()
{
    ApplicationModel model;

    model.selectListeningFrequencyAt(75.0, 100.0);
    model.setCenterFrequencyText(QStringLiteral("200000000"));

    QCOMPARE(model.centerFrequency(), quint64{200'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{200'000'000});
    QCOMPARE(model.listeningPosition(), 0.5);
    QCOMPARE(
        model.statusText(),
        QStringLiteral(
            "Center and listening frequency changed"));
}

void ApplicationModelTest::updatesReceiverState()
{
    ApplicationModel model;
    QSignalSpy runningSpy(&model, &ApplicationModel::receiverRunningChanged);

    model.startReception();
    QVERIFY(model.receiverRunning());
    QCOMPARE(runningSpy.count(), 1);
    QCOMPARE(
        model.statusText(),
        QStringLiteral("Mock reception started - no hardware is active"));

    model.stopReception();
    QVERIFY(!model.receiverRunning());
    QCOMPARE(runningSpy.count(), 2);
    QCOMPARE(model.statusText(), QStringLiteral("Mock reception stopped"));
}

void ApplicationModelTest::tunesBookmarksExactlyWithoutStartingReception()
{
    ApplicationModel model;
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(model.bookmarkModel());
    QVERIFY(bookmarks);
    const auto add = [bookmarks](QString name, QString mode, qint64 low,
                                 qint64 high, quint64 frequency, double gain,
                                 double squelch, bool enabled) {
        sdr::app::BookmarkData data;
        data.name = std::move(name);
        data.listeningFrequency = frequency;
        data.requestedGainDb = gain;
        data.demodulatorId = std::move(mode);
        data.filterLowHz = low;
        data.filterHighHz = high;
        data.squelchThresholdDb = squelch;
        data.squelchEnabled = enabled;
        return bookmarks->addBookmark(-1, data);
    };
    const QString usb = add(QStringLiteral("USB"), QStringLiteral("usb"),
        0, 2'700, 14'200'000, 18.5, -67.0, true);
    model.tuneBookmark(bookmarks->visibleRowForUuid(usb));
    QVERIFY(!model.receiverRunning());
    QCOMPARE(model.centerFrequency(), quint64{14'200'000});
    QCOMPARE(model.listeningFrequency(), quint64{14'200'000});
    QCOMPARE(model.requestedGain(), 18.5);
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.filterWidth(), quint64{2'700});
    QCOMPARE(model.filterLowerFrequency(), quint64{14'200'000});
    QCOMPARE(model.filterUpperFrequency(), quint64{14'202'700});
    QCOMPARE(model.squelchLevel(), -67.0);
    QVERIFY(!model.squelchDisabled());

    const QString lsb = add(QStringLiteral("LSB"), QStringLiteral("lsb"),
        -2'400, 0, 7'100'000, 22.0, -54.0, false);
    model.startReception();
    model.tuneBookmark(bookmarks->visibleRowForUuid(lsb));
    QVERIFY(model.receiverRunning());
    QCOMPARE(model.listeningFrequency(), quint64{7'100'000});
    QCOMPARE(model.demodulationModeName(), QStringLiteral("LSB"));
    QCOMPARE(model.filterWidth(), quint64{2'400});
    QCOMPARE(model.filterLowerFrequency(), quint64{7'097'600});
    QCOMPARE(model.filterUpperFrequency(), quint64{7'100'000});
    QCOMPARE(model.squelchLevel(), -54.0);
    QVERIFY(model.squelchDisabled());
}

void ApplicationModelTest::forwardsControlsToMockBackend()
{
    ApplicationModel model;

    model.setSampleRate(1'000'000);
    QCOMPARE(model.sampleRate(), quint64{1'000'000});

    model.setDemodulationModeIndex(1);
    model.setFilterWidth(25'000);
    QCOMPARE(model.filterWidth(), quint64{25'000});

    model.setGain(24.0);
    QCOMPARE(model.gain(), 24.0);

    model.setPpmCorrection(-12.0);
    QCOMPARE(model.ppmCorrection(), -12.0);

    QCOMPARE(
        model.demodulationModes(),
        QStringList({
            QStringLiteral("AM"),
            QStringLiteral("NFM"),
            QStringLiteral("WFM"),
            QStringLiteral("USB"),
            QStringLiteral("LSB"),
            QStringLiteral("DMR/P25"),
        }));
    model.setDemodulationModeIndex(5);
    QCOMPARE(model.demodulationModeName(), QStringLiteral("DMR/P25"));
    QCOMPARE(
        model.demodulationModeIndex(),
        static_cast<int>(
            sdr::radio::DemodulationMode::DigitalDecoderOutput));

    model.setSquelchLevel(-62.0);
    QCOMPARE(model.squelchLevel(), -62.0);
    QVERIFY(!model.squelchDisabled());

    model.disableSquelch();
    QVERIFY(model.squelchDisabled());
}

void ApplicationModelTest::defersGainApplicationUntilSliderCommit()
{
    ApplicationModel model;

    model.previewGain(24.0);
    QCOMPARE(model.requestedGain(), 24.0);
    QCOMPARE(model.gain(), 0.0);

    model.commitGain(24.0);
    QCOMPARE(model.gain(), 24.0);
}

void ApplicationModelTest::exposesModeSpecificControls()
{
    ApplicationModel model;

    model.setDemodulationModeIndex(3);
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.minimumFilterWidth(), quint64{1'800});
    QCOMPARE(model.maximumFilterWidth(), quint64{4'000});
    QCOMPARE(model.filterWidth(), quint64{2'400});

    model.setDemodulationModeIndex(4);
    QCOMPARE(model.demodulationModeName(), QStringLiteral("LSB"));

    model.setSquelchLevel(-64.0);
    QCOMPARE(model.squelchLevel(), -64.0);
    QCOMPARE(model.squelchStateText(), QStringLiteral("Manual"));
    model.disableSquelch();
    QCOMPARE(model.squelchStateText(), QStringLiteral("Disabled (open)"));
}

void ApplicationModelTest::filtersPresetWidthsAndAcceptsCustomWidths()
{
    ApplicationModel model;
    QVERIFY(model.filterWidthOptions().contains(QStringLiteral("10 kHz")));
    QVERIFY(model.filterWidthOptions().contains(QStringLiteral("Custom…")));
    model.setFilterWidthText(QStringLiteral("9 kHz"));
    QCOMPARE(model.filterWidth(), quint64{9'000});

    model.setDemodulationModeIndex(static_cast<int>(sdr::radio::DemodulationMode::Wfm));
    QVERIFY(model.filterWidthOptions().contains(QStringLiteral("180 kHz")));
    QVERIFY(!model.filterWidthOptions().contains(QStringLiteral("9 kHz")));
    model.setFilterWidthText(QStringLiteral("90 kHz"));
    QCOMPARE(model.filterWidth(), quint64{180'000});
    QVERIFY(model.statusText().contains(QStringLiteral("outside")));
}

void ApplicationModelTest::reportsUnsupportedPpmWithoutChangingState()
{
    auto backend = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{.ppmCorrectionSupported = false});
    ApplicationModel model(std::move(backend));

    QVERIFY(!model.ppmCorrectionSupported());
    model.setPpmCorrection(10.0);
    QCOMPARE(model.ppmCorrection(), 0.0);
    QVERIFY(model.statusText().contains(QStringLiteral("unsupported")));
}

void ApplicationModelTest::synchronizesLifecycleOnlyAfterBackendConfirmation()
{
    auto startFailure = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{.startSucceeds = false});
    ApplicationModel stoppedModel(std::move(startFailure));
    stoppedModel.startReception();
    QVERIFY(!stoppedModel.receiverRunning());
    QVERIFY(stoppedModel.statusText().contains(QStringLiteral("failed to start")));

    auto stopFailure = std::make_unique<sdr::radio::MockReceiverBackend>(
        sdr::radio::MockReceiverConfiguration{.stopSucceeds = false});
    ApplicationModel runningModel(std::move(stopFailure));
    runningModel.startReception();
    QVERIFY(runningModel.receiverRunning());
    runningModel.stopReception();
    QVERIFY(runningModel.receiverRunning());
    QVERIFY(runningModel.statusText().contains(QStringLiteral("failed to stop")));
}

QTEST_GUILESS_MAIN(ApplicationModelTest)

#include "ApplicationModelTest.moc"
