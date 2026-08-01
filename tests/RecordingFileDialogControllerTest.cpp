// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordingFileDialogController.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace {

constexpr auto lastDirectorySetting = "recording/dialogLastDirectory";
constexpr auto geometrySetting = "recording/dialogGeometry";
constexpr auto stateSetting = "recording/dialogState";
constexpr auto selectedFilterSetting = "recording/dialogSelectedFilter";
const QString rawFilter = QStringLiteral("Raw IQ (*.raw *.RAW)");

QString cleanPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString fallbackDirectory()
{
    const QString music = QStandardPaths::writableLocation(
        QStandardPaths::MusicLocation);
    if (QFileInfo(music).isDir() && QFileInfo(music).isReadable()) {
        return cleanPath(music);
    }
    return cleanPath(QDir::homePath());
}

bool hasSidebarDirectory(const QList<QUrl>& urls, const QString& expected)
{
    return std::any_of(urls.cbegin(), urls.cend(), [&expected](const QUrl& url) {
        return url.isLocalFile() && cleanPath(url.toLocalFile()) == cleanPath(expected);
    });
}

}  // namespace

class RecordingFileDialogControllerTest final : public QObject
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

private:
    QTemporaryDir m_settingsDirectory;
};

void RecordingFileDialogControllerTest::initTestCase()
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
}

void RecordingFileDialogControllerTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
}

void RecordingFileDialogControllerTest::configuresDetailDialogAndPreventsDuplicates()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    int loads = 0;
    sdr::gui::RecordingFileDialogController controller(
        [&loads](const QUrl&) { ++loads; },
        [&recordings] { return recordings.path(); });

    controller.open();
    QFileDialog* const firstDialog = controller.dialog();
    QVERIFY(firstDialog);
    QVERIFY(controller.dialogOpen());
    QVERIFY(firstDialog->testOption(QFileDialog::DontUseNativeDialog));
    QCOMPARE(firstDialog->fileMode(), QFileDialog::ExistingFile);
    QCOMPARE(firstDialog->acceptMode(), QFileDialog::AcceptOpen);
    QCOMPARE(firstDialog->viewMode(), QFileDialog::Detail);
    QCOMPARE(firstDialog->windowModality(), Qt::ApplicationModal);
    QCOMPARE(firstDialog->windowTitle(), QStringLiteral("Load recording"));
    QCOMPARE(firstDialog->nameFilters(), QStringList({
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
    const QString music = QStandardPaths::writableLocation(
        QStandardPaths::MusicLocation);
    if (QFileInfo(music).isDir() && QFileInfo(music).isReadable()) {
        QVERIFY(hasSidebarDirectory(firstDialog->sidebarUrls(), music));
    }

    controller.open();
    QCOMPARE(controller.dialog(), firstDialog);
    QCOMPARE(loads, 0);
    firstDialog->reject();
    QVERIFY(!controller.dialogOpen());
}

void RecordingFileDialogControllerTest::acceptsNormalizedLocalFileAndPersistsState()
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
    sdr::gui::RecordingFileDialogController controller(
        [&loadedUrl](const QUrl& url) { loadedUrl = url; },
        [&recordings] { return recordings.path(); });
    QSignalSpy selected(&controller,
                        &sdr::gui::RecordingFileDialogController::recordingFileSelected);
    controller.open();
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
    QCOMPARE(settings.value(lastDirectorySetting).toString(), cleanPath(recordings.path()));
    QVERIFY(settings.contains(geometrySetting));
    QVERIFY(settings.contains(stateSetting));
    QCOMPARE(settings.value(selectedFilterSetting).toString(), rawFilter);
}

void RecordingFileDialogControllerTest::cancellationPreservesSelectionAndSettings()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    QSettings settings;
    settings.setValue(lastDirectorySetting, recordings.path());
    settings.setValue(selectedFilterSetting, rawFilter);
    settings.sync();

    QString activeRecording = QStringLiteral("already-loaded.raw");
    sdr::gui::RecordingFileDialogController controller(
        [&activeRecording](const QUrl& url) { activeRecording = url.toLocalFile(); },
        [&recordings] { return recordings.path(); });
    controller.open();
    QFileDialog* const dialog = controller.dialog();
    QVERIFY(dialog);
    dialog->resize(1'050, 690);
    dialog->selectNameFilter(QStringLiteral("All files (*)"));
    dialog->reject();

    QCOMPARE(activeRecording, QStringLiteral("already-loaded.raw"));
    QCOMPARE(settings.value(lastDirectorySetting).toString(), recordings.path());
    QCOMPARE(settings.value(selectedFilterSetting).toString(), rawFilter);
    QVERIFY(!settings.contains(geometrySetting));
    QVERIFY(!settings.contains(stateSetting));
}

void RecordingFileDialogControllerTest::restoresFilterAndFallsBackFromInvalidDirectories()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    QSettings settings;
    settings.setValue(lastDirectorySetting, recordings.filePath(QStringLiteral("missing")));
    settings.setValue(selectedFilterSetting, rawFilter);
    settings.sync();

    sdr::gui::RecordingFileDialogController controller(
        [](const QUrl&) {}, [&recordings] { return recordings.path(); });
    controller.open();
    QCOMPARE(cleanPath(controller.dialog()->directory().absolutePath()),
             cleanPath(recordings.path()));
    QCOMPARE(controller.dialog()->selectedNameFilter(), rawFilter);
    controller.dialog()->reject();

    settings.clear();
    settings.sync();
    sdr::gui::RecordingFileDialogController fallbackController(
        [](const QUrl&) {}, [] { return QStringLiteral("/not/a/recordings/folder"); });
    fallbackController.open();
    QCOMPARE(cleanPath(fallbackController.dialog()->directory().absolutePath()),
             fallbackDirectory());
    fallbackController.dialog()->reject();
}

void RecordingFileDialogControllerTest::ignoresMalformedSavedDialogState()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    QSettings settings;
    settings.setValue(geometrySetting, QByteArrayLiteral("not-a-geometry"));
    settings.setValue(stateSetting, QByteArrayLiteral("not-a-dialog-state"));
    settings.setValue(selectedFilterSetting, QStringLiteral("unsupported"));
    settings.sync();

    sdr::gui::RecordingFileDialogController controller(
        [](const QUrl&) {}, [&recordings] { return recordings.path(); });
    controller.open();
    QFileDialog* const dialog = controller.dialog();
    QVERIFY(dialog);
    QCOMPARE(dialog->viewMode(), QFileDialog::Detail);
    QCOMPARE(dialog->selectedNameFilter(), dialog->nameFilters().constFirst());
    QVERIFY(dialog->width() >= 860);
    QVERIFY(dialog->height() >= 560);
    dialog->reject();
}

QTEST_MAIN(RecordingFileDialogControllerTest)

#include "RecordingFileDialogControllerTest.moc"
