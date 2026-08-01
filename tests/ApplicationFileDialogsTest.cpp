// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationFileDialogs.hpp"

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

QTEST_MAIN(ApplicationFileDialogsTest)

#include "ApplicationFileDialogsTest.moc"
