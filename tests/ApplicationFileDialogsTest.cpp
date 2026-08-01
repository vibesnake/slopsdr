// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationFileDialogs.hpp"

#if SDR_RECEIVER_TEST_GNURADIO
#include "ApplicationModel.hpp"
#include "GnuRadioReceiverBackend.hpp"
#include "RecordedAudioBackend.hpp"
#include "ReceiverRuntime.hpp"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace
{

constexpr auto lastDirectorySetting = "dialogs/recording/lastDirectory";
constexpr auto geometrySetting = "dialogs/sharedGeometry";
constexpr auto stateSetting = "dialogs/sharedState";
constexpr auto selectedFilterSetting = "dialogs/recording/selectedFilter";
const QString rawFilter = QStringLiteral("Raw IQ (*.raw *.RAW)");

QString cleanPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString fallbackDirectory()
{
    const QString music =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (QFileInfo(music).isDir() && QFileInfo(music).isReadable())
    {
        return cleanPath(music);
    }
    return cleanPath(QDir::homePath());
}

bool hasSidebarDirectory(const QList<QUrl>& urls, const QString& expected)
{
    return std::any_of(urls.cbegin(), urls.cend(),
                       [&expected](const QUrl& url)
                       {
                           return url.isLocalFile() &&
                                  cleanPath(url.toLocalFile()) == cleanPath(expected);
                       });
}

#if SDR_RECEIVER_TEST_GNURADIO
void appendLittleEndian16(QByteArray& bytes, std::uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendLittleEndian32(QByteArray& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
}

QString writePcmWav(const QTemporaryDir& directory)
{
    QByteArray samples(32, '\0');
    QByteArray wav{"RIFF", 4};
    appendLittleEndian32(wav, static_cast<std::uint32_t>(36 + samples.size()));
    wav.append("WAVEfmt ", 8);
    appendLittleEndian32(wav, 16);
    appendLittleEndian16(wav, 1);
    appendLittleEndian16(wav, 1);
    appendLittleEndian32(wav, 48'000);
    appendLittleEndian32(wav, 96'000);
    appendLittleEndian16(wav, 2);
    appendLittleEndian16(wav, 16);
    wav.append("data", 4);
    appendLittleEndian32(wav, static_cast<std::uint32_t>(samples.size()));
    wav.append(samples);
    const QString path = directory.filePath(QStringLiteral("content-detected.bin"));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(wav) != wav.size())
        return {};
    return path;
}

bool waitUntil(const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

QString recordingTransitionDiagnostic(const ApplicationModel& model,
    const QSignalSpy& snapshots, qsizetype snapshotCount)
{
    return QStringLiteral(
               "snapshots=%1 (expected > %2), status=%3, loaded=%4, "
               "source=%5, recording=%6, metadataRequired=%7")
        .arg(snapshots.count())
        .arg(snapshotCount)
        .arg(model.statusText())
        .arg(model.recordingLoaded())
        .arg(model.sourceDescription())
        .arg(model.recordingDisplayName())
        .arg(model.recordedIqMetadataRequired());
}

bool waitForRecordingTransition(const QSignalSpy& snapshots,
    qsizetype snapshotCount, const std::function<bool()>& predicate)
{
    return waitUntil([&snapshots, snapshotCount, &predicate] {
        return snapshots.count() > snapshotCount && predicate();
    });
}
#endif

} // namespace

class ApplicationFileDialogsTest final : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void configuresDetailDialogAndPreventsDuplicates();
    void acceptsNormalizedLocalFileAndPersistsState();
    void cancellationPreservesSelectionAndSettings();
    void restoresFilterAndFallsBackFromInvalidDirectories();
    void ignoresMalformedSavedDialogState();
    void configuresDirectoryAndExecutablePurposes();
    void keepsPurposeDirectoriesIndependentAndRestoresSharedGeometry();
    void rejectsInvalidDirectoryAndExecutableSelections();
#if SDR_RECEIVER_TEST_GNURADIO
    void loadsRawIqManualFallbackAndWavThroughSharedDialog();
#endif

  private:
    QTemporaryDir m_settingsDirectory;
};

void ApplicationFileDialogsTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("slopSDR"));
    QCoreApplication::setApplicationName(QStringLiteral("slopSDR"));
    qputenv("XDG_CONFIG_HOME", m_settingsDirectory.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       m_settingsDirectory.path());
}

void ApplicationFileDialogsTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
}

void ApplicationFileDialogsTest::configuresDetailDialogAndPreventsDuplicates()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    int loads = 0;
    sdr::gui::ApplicationFileDialogs controller([&loads](const QUrl&) { ++loads; }, {},
                                                {}, [&recordings]
                                                { return recordings.path(); }, {});

    controller.openRecordingFileDialog();
    QFileDialog* const firstDialog = controller.dialog();
    QVERIFY(firstDialog);
    QVERIFY(controller.dialogOpen());
    QVERIFY(firstDialog->testOption(QFileDialog::DontUseNativeDialog));
    QCOMPARE(firstDialog->fileMode(), QFileDialog::ExistingFile);
    QCOMPARE(firstDialog->acceptMode(), QFileDialog::AcceptOpen);
    QCOMPARE(firstDialog->viewMode(), QFileDialog::Detail);
    QCOMPARE(firstDialog->windowModality(), Qt::ApplicationModal);
    QCOMPARE(firstDialog->windowTitle(), QStringLiteral("Load recording"));
    QCOMPARE(firstDialog->nameFilters(),
             QStringList({
                 QStringLiteral("All supported recordings (*.wav *.WAV *.raw *.RAW)"),
                 QStringLiteral("WAV audio (*.wav *.WAV)"),
                 rawFilter,
                 QStringLiteral("All files (*)"),
             }));
    QVERIFY(firstDialog->width() >= 860);
    QVERIFY(firstDialog->height() >= 560);
    QCOMPARE(cleanPath(firstDialog->directory().absolutePath()),
             cleanPath(recordings.path()));
    QVERIFY(hasSidebarDirectory(firstDialog->sidebarUrls(), recordings.path()));
    QVERIFY(hasSidebarDirectory(firstDialog->sidebarUrls(), QDir::homePath()));
    const QString music =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (QFileInfo(music).isDir() && QFileInfo(music).isReadable())
    {
        QVERIFY(hasSidebarDirectory(firstDialog->sidebarUrls(), music));
    }

    controller.openRecordingFileDialog();
    QCOMPARE(controller.dialog(), firstDialog);
    QCOMPARE(loads, 0);
    firstDialog->reject();
    QVERIFY(!controller.dialogOpen());
}

void ApplicationFileDialogsTest::acceptsNormalizedLocalFileAndPersistsState()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    const QString rawPath = recordings.filePath(QStringLiteral("capture.raw"));
    QFile raw(rawPath);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    QCOMPARE(raw.write("raw", 3), qint64{3});
    raw.close();
    QVERIFY(QDir().mkpath(recordings.filePath(QStringLiteral("nested"))));

    QUrl loadedUrl;
    sdr::gui::ApplicationFileDialogs controller(
        [&loadedUrl](const QUrl& url) { loadedUrl = url; }, {}, {},
        [&recordings] { return recordings.path(); }, {});
    QSignalSpy selected(&controller,
                        &sdr::gui::ApplicationFileDialogs::recordingFileSelected);
    controller.openRecordingFileDialog();
    QFileDialog* const dialog = controller.dialog();
    QVERIFY(dialog);
    dialog->selectNameFilter(rawFilter);
    dialog->resize(970, 640);
    dialog->selectFile(recordings.filePath(QStringLiteral("nested/../capture.raw")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));

    QCOMPARE(selected.count(), 1);
    QCOMPARE(loadedUrl, QUrl::fromLocalFile(cleanPath(rawPath)));
    QVERIFY(!controller.dialogOpen());
    QSettings settings;
    QCOMPARE(settings.value(lastDirectorySetting).toString(),
             cleanPath(recordings.path()));
    QVERIFY(settings.contains(geometrySetting));
    QVERIFY(settings.contains(stateSetting));
    QCOMPARE(settings.value(selectedFilterSetting).toString(), rawFilter);
}

void ApplicationFileDialogsTest::cancellationPreservesSelectionAndSettings()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    QSettings settings;
    settings.setValue(lastDirectorySetting, recordings.path());
    settings.setValue(selectedFilterSetting, rawFilter);
    settings.sync();

    QString activeRecording = QStringLiteral("already-loaded.raw");
    QString recordingsFolder = QStringLiteral("/preserved/folder");
    QString dsdFmeExecutable = QStringLiteral("/preserved/dsd-fme");
    sdr::gui::ApplicationFileDialogs controller(
        [&activeRecording](const QUrl& url) { activeRecording = url.toLocalFile(); },
        [&recordingsFolder](const QUrl& url) { recordingsFolder = url.toLocalFile(); },
        [&dsdFmeExecutable](const QUrl& url) { dsdFmeExecutable = url.toLocalFile(); },
        [&recordings] { return recordings.path(); }, [] { return QString(); });
    controller.openRecordingFileDialog();
    QFileDialog* const dialog = controller.dialog();
    QVERIFY(dialog);
    dialog->resize(1'050, 690);
    dialog->selectNameFilter(QStringLiteral("All files (*)"));
    dialog->reject();

    QCOMPARE(activeRecording, QStringLiteral("already-loaded.raw"));
    controller.selectRecordingDirectory();
    controller.dialog()->reject();
    controller.selectDsdFmeExecutable();
    controller.dialog()->reject();
    QCOMPARE(recordingsFolder, QStringLiteral("/preserved/folder"));
    QCOMPARE(dsdFmeExecutable, QStringLiteral("/preserved/dsd-fme"));
    QCOMPARE(settings.value(lastDirectorySetting).toString(), recordings.path());
    QCOMPARE(settings.value(selectedFilterSetting).toString(), rawFilter);
    QVERIFY(!settings.contains(geometrySetting));
    QVERIFY(!settings.contains(stateSetting));
    QVERIFY(
        !settings.contains(QStringLiteral("dialogs/recordingDirectory/lastDirectory")));
    QVERIFY(!settings.contains(QStringLiteral("dialogs/dsdFme/lastDirectory")));
}

void ApplicationFileDialogsTest::restoresFilterAndFallsBackFromInvalidDirectories()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    QSettings settings;
    settings.setValue(lastDirectorySetting,
                      recordings.filePath(QStringLiteral("missing")));
    settings.setValue(selectedFilterSetting, rawFilter);
    settings.sync();

    sdr::gui::ApplicationFileDialogs controller(
        [](const QUrl&) {}, {}, {}, [&recordings] { return recordings.path(); }, {});
    controller.openRecordingFileDialog();
    QCOMPARE(cleanPath(controller.dialog()->directory().absolutePath()),
             cleanPath(recordings.path()));
    QCOMPARE(controller.dialog()->selectedNameFilter(), rawFilter);
    controller.dialog()->reject();

    settings.clear();
    settings.sync();
    sdr::gui::ApplicationFileDialogs fallbackController(
        [](const QUrl&) {}, {}, {},
        [] { return QStringLiteral("/not/a/recordings/folder"); }, {});
    fallbackController.openRecordingFileDialog();
    QCOMPARE(cleanPath(fallbackController.dialog()->directory().absolutePath()),
             fallbackDirectory());
    fallbackController.dialog()->reject();
}

void ApplicationFileDialogsTest::ignoresMalformedSavedDialogState()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    QSettings settings;
    settings.setValue(geometrySetting, QByteArrayLiteral("not-a-geometry"));
    settings.setValue(stateSetting, QByteArrayLiteral("not-a-dialog-state"));
    settings.setValue(selectedFilterSetting, QStringLiteral("unsupported"));
    settings.sync();

    sdr::gui::ApplicationFileDialogs controller(
        [](const QUrl&) {}, {}, {}, [&recordings] { return recordings.path(); }, {});
    controller.openRecordingFileDialog();
    QFileDialog* const dialog = controller.dialog();
    QVERIFY(dialog);
    QCOMPARE(dialog->viewMode(), QFileDialog::Detail);
    QCOMPARE(dialog->selectedNameFilter(), dialog->nameFilters().constFirst());
    QVERIFY(dialog->width() >= 860);
    QVERIFY(dialog->height() >= 560);
    dialog->reject();
}

void ApplicationFileDialogsTest::configuresDirectoryAndExecutablePurposes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = directory.filePath(QStringLiteral("dsd-fme"));
    QFile file(executable);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QUrl selectedDirectory;
    QUrl selectedExecutable;
    sdr::gui::ApplicationFileDialogs dialogs(
        [](const QUrl&) {}, [&selectedDirectory](const QUrl& url)
        { selectedDirectory = url; }, [&selectedExecutable](const QUrl& url)
        { selectedExecutable = url; }, [&directory] { return directory.path(); },
        [&executable] { return executable; });
    dialogs.selectRecordingDirectory();
    QCOMPARE(dialogs.dialog()->windowTitle(),
             QStringLiteral("Select recording folder"));
    QCOMPARE(dialogs.dialog()->fileMode(), QFileDialog::Directory);
    QVERIFY(dialogs.dialog()->testOption(QFileDialog::ShowDirsOnly));
    dialogs.dialog()->selectFile(directory.path());
    QVERIFY(
        QMetaObject::invokeMethod(dialogs.dialog(), "accept", Qt::DirectConnection));
    QCOMPARE(selectedDirectory, QUrl::fromLocalFile(cleanPath(directory.path())));
    dialogs.selectDsdFmeExecutable();
    QCOMPARE(dialogs.dialog()->windowTitle(),
             QStringLiteral("Select dsd-fme executable"));
    QCOMPARE(dialogs.dialog()->fileMode(), QFileDialog::ExistingFile);
    QVERIFY(dialogs.dialog()->nameFilters().contains(
        QStringLiteral("Executable files (*)")));
    dialogs.dialog()->selectFile(executable);
    QVERIFY(
        QMetaObject::invokeMethod(dialogs.dialog(), "accept", Qt::DirectConnection));
    QCOMPARE(selectedExecutable, QUrl::fromLocalFile(cleanPath(executable)));
    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("dialogs/recordingDirectory/lastDirectory"))
                 .toString(),
             cleanPath(directory.path()));
    QCOMPARE(settings.value(QStringLiteral("dialogs/dsdFme/lastDirectory")).toString(),
             cleanPath(directory.path()));
}

void ApplicationFileDialogsTest::
    keepsPurposeDirectoriesIndependentAndRestoresSharedGeometry()
{
    QTemporaryDir recordingDirectory;
    QTemporaryDir folderDirectory;
    QTemporaryDir executableDirectory;
    QVERIFY(recordingDirectory.isValid());
    QVERIFY(folderDirectory.isValid());
    QVERIFY(executableDirectory.isValid());
    QSettings settings;
    settings.setValue(QStringLiteral("dialogs/recording/lastDirectory"),
                      recordingDirectory.path());
    settings.setValue(QStringLiteral("dialogs/recordingDirectory/lastDirectory"),
                      folderDirectory.path());
    settings.setValue(QStringLiteral("dialogs/dsdFme/lastDirectory"),
                      executableDirectory.path());
    sdr::gui::ApplicationFileDialogs dialogs(
        [](const QUrl&) {}, {}, {}, [] { return QString(); }, [] { return QString(); });
    dialogs.openRecordingFileDialog();
    QCOMPARE(cleanPath(dialogs.dialog()->directory().absolutePath()),
             cleanPath(recordingDirectory.path()));
    dialogs.dialog()->reject();
    dialogs.selectRecordingDirectory();
    QCOMPARE(cleanPath(dialogs.dialog()->directory().absolutePath()),
             cleanPath(folderDirectory.path()));
    dialogs.dialog()->reject();
    dialogs.selectDsdFmeExecutable();
    QCOMPARE(cleanPath(dialogs.dialog()->directory().absolutePath()),
             cleanPath(executableDirectory.path()));
    dialogs.dialog()->resize(980, 650);
    dialogs.dialog()->selectFile(executableDirectory.path());
    QVERIFY(
        QMetaObject::invokeMethod(&dialogs, "handleAccepted", Qt::DirectConnection));
    settings.setValue(geometrySetting, dialogs.dialog()->saveGeometry());
    sdr::gui::ApplicationFileDialogs restored(
        [](const QUrl&) {}, {}, {}, [] { return QString(); }, [] { return QString(); });
    restored.selectRecordingDirectory();
    QVERIFY(restored.dialog()->width() >= 900);
    QVERIFY(restored.dialog()->height() >= 600);
    restored.dialog()->reject();
}

void ApplicationFileDialogsTest::rejectsInvalidDirectoryAndExecutableSelections()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString unwritable = root.filePath(QStringLiteral("read-only"));
    QVERIFY(QDir().mkdir(unwritable));
    QVERIFY(QFile::setPermissions(unwritable, QFile::ReadOwner | QFile::ExeOwner));
    const QString nonExecutable = root.filePath(QStringLiteral("dsd-fme"));
    QFile file(nonExecutable);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QUrl selectedDirectory = QUrl::fromLocalFile(QStringLiteral("/preserved/folder"));
    QUrl selectedExecutable = QUrl::fromLocalFile(QStringLiteral("/preserved/dsd-fme"));
    sdr::gui::ApplicationFileDialogs dialogs(
        [](const QUrl&) {},
        [&selectedDirectory](const QUrl& url) { selectedDirectory = url; },
        [&selectedExecutable](const QUrl& url) { selectedExecutable = url; },
        [&root] { return root.path(); }, [] { return QString(); });
    QSignalSpy errors(&dialogs, &sdr::gui::ApplicationFileDialogs::selectionError);
    dialogs.selectRecordingDirectory();
    dialogs.dialog()->selectFile(nonExecutable);
    QVERIFY(
        QMetaObject::invokeMethod(&dialogs, "handleAccepted", Qt::DirectConnection));
    QCOMPARE(selectedDirectory,
             QUrl::fromLocalFile(QStringLiteral("/preserved/folder")));
    QCOMPARE(errors.count(), 1);
    dialogs.selectRecordingDirectory();
    dialogs.dialog()->selectFile(unwritable);
    QVERIFY(
        QMetaObject::invokeMethod(&dialogs, "handleAccepted", Qt::DirectConnection));
    QCOMPARE(selectedDirectory,
             QUrl::fromLocalFile(QStringLiteral("/preserved/folder")));
    QCOMPARE(errors.count(), 2);
    dialogs.selectDsdFmeExecutable();
    dialogs.dialog()->selectFile(root.path());
    QVERIFY(
        QMetaObject::invokeMethod(&dialogs, "handleAccepted", Qt::DirectConnection));
    QCOMPARE(selectedExecutable,
             QUrl::fromLocalFile(QStringLiteral("/preserved/dsd-fme")));
    QCOMPARE(errors.count(), 3);
    dialogs.selectDsdFmeExecutable();
    dialogs.dialog()->selectFile(root.filePath(QStringLiteral("missing")));
    QVERIFY(
        QMetaObject::invokeMethod(&dialogs, "handleAccepted", Qt::DirectConnection));
    QCOMPARE(selectedExecutable,
             QUrl::fromLocalFile(QStringLiteral("/preserved/dsd-fme")));
    QCOMPARE(errors.count(), 4);
    dialogs.selectDsdFmeExecutable();
    dialogs.dialog()->selectFile(nonExecutable);
    QVERIFY(
        QMetaObject::invokeMethod(&dialogs, "handleAccepted", Qt::DirectConnection));
    QCOMPARE(selectedExecutable,
             QUrl::fromLocalFile(QStringLiteral("/preserved/dsd-fme")));
    QCOMPARE(errors.count(), 5);
    QVERIFY(!QSettings().contains(
        QStringLiteral("dialogs/recordingDirectory/lastDirectory")));
    QVERIFY(!QSettings().contains(QStringLiteral("dialogs/dsdFme/lastDirectory")));
}

#if SDR_RECEIVER_TEST_GNURADIO
void ApplicationFileDialogsTest::loadsRawIqManualFallbackAndWavThroughSharedDialog()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    const QString rawPath = recordings.filePath(QStringLiteral("sidecar capture.raw"));
    {
        QFile raw(rawPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray(32, '\0')), qint64{32});
    }
    {
        QFile sidecar(recordings.filePath(QStringLiteral("sidecar capture.json")));
        QVERIFY(sidecar.open(QIODevice::WriteOnly));
        const QByteArray json =
            "{\"hardware_center_frequency_hz\":101000000,"
            "\"sample_rate_hz\":200000,\"sample_format\":\"cf32_le\","
            "\"byte_order\":\"little-endian\",\"written_sample_count\":4}";
        QCOMPARE(sidecar.write(json), json.size());
    }
    const QString manualPath = recordings.filePath(QStringLiteral("manual.raw"));
    {
        QFile raw(manualPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray(32, '\0')), qint64{32});
    }
    const QString malformedPath =
        recordings.filePath(QStringLiteral("malformed.raw"));
    {
        QFile raw(malformedPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write("bad", 3), qint64{3});
    }
    const QString invalidSidecarPath =
        recordings.filePath(QStringLiteral("invalid-sidecar.raw"));
    {
        QFile raw(invalidSidecarPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray(32, '\0')), qint64{32});
        QFile sidecar(
            recordings.filePath(QStringLiteral("invalid-sidecar.json")));
        QVERIFY(sidecar.open(QIODevice::WriteOnly));
        QCOMPARE(sidecar.write("{}", 2), qint64{2});
    }
    const QString wavPath = writePcmWav(recordings);
    QVERIFY(!wavPath.isEmpty());

    sdr::app::ReceiverRuntime::Factories factories;
    factories.createRecordedBackend = [](
                                          sdr::radio::RecordedIqSourceConfiguration source) {
        return std::make_unique<sdr::dsp::GnuRadioReceiverBackend>(
            std::move(source));
    };
    factories.createRecordedAudioBackend = [](const std::string& path) {
        return std::make_unique<sdr::dsp::RecordedAudioBackend>(
            std::filesystem::path(path));
    };
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock, std::move(factories));
    ApplicationModel model(runtime);
    QSignalSpy snapshots(&runtime, &sdr::app::ReceiverRuntime::snapshotChanged);
    runtime.start();
    QVERIFY2(waitUntil([&model, &snapshots] {
        return snapshots.count() > 0 && model.mockMode() &&
               model.statusText().contains(QStringLiteral("Mock backend ready"));
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, 0)));

    int forwardedCount = 0;
    QStringList forwardedPaths;
    sdr::gui::ApplicationFileDialogs dialogs(
        [&model, &forwardedCount, &forwardedPaths](const QUrl& url) {
            ++forwardedCount;
            forwardedPaths.append(url.toLocalFile());
            model.loadRecording(url);
        },
        {}, {}, [&recordings] { return recordings.path(); }, {});
    const auto acceptRecording = [&dialogs](const QString& path) {
        dialogs.openRecordingFileDialog();
        dialogs.dialog()->selectFile(path);
        return QMetaObject::invokeMethod(
            dialogs.dialog(), "accept", Qt::DirectConnection);
    };

    QVERIFY(QDir().mkpath(recordings.filePath(QStringLiteral("nested"))));
    qsizetype snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(
        recordings.filePath(QStringLiteral("nested/../sidecar capture.raw"))));
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model] {
        return model.recordingLoaded() && model.recordedIqSource() &&
               model.recordingDisplayName() == QStringLiteral("sidecar capture.raw");
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));
    QCOMPARE(forwardedCount, 1);
    QCOMPARE(forwardedPaths.constFirst(), cleanPath(rawPath));
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("sidecar capture.raw"));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(manualPath));
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model] {
        return model.recordedIqMetadataRequired();
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordingLoaded());
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("sidecar capture.raw"));
    snapshotCount = snapshots.count();
    model.selectRecordedIqSource(
        QUrl::fromLocalFile(manualPath), 102'000'000, 250'000);
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model] {
        return model.recordingLoaded() && !model.recordedIqMetadataRequired() &&
               model.recordingDisplayName() == QStringLiteral("manual.raw");
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(invalidSidecarPath));
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model] {
        return model.recordedIqMetadataRequired();
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordingLoaded());
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("manual.raw"));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(malformedPath));
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model] {
        return model.statusText().contains(QStringLiteral("selection failed"));
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordingLoaded());
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("manual.raw"));
    QVERIFY(model.statusText().contains(QStringLiteral("truncated")));

    snapshotCount = snapshots.count();
    model.ejectRecording();
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model] {
        return !model.recordingLoaded();
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(wavPath));
    QVERIFY2(waitForRecordingTransition(snapshots, snapshotCount, [&model, &wavPath] {
        return model.recordingLoaded() &&
               model.recordingDisplayName() == QFileInfo(wavPath).fileName();
    }), qPrintable(recordingTransitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordedAudioSource());
    QCOMPARE(forwardedCount, 5);

    runtime.shutdown();
}
#endif

QTEST_MAIN(ApplicationFileDialogsTest)

#include "ApplicationFileDialogsTest.moc"
