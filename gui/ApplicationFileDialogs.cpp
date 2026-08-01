// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationFileDialogs.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace sdr::gui
{
namespace
{

constexpr auto lastDirectorySetting = "dialogs/%1/lastDirectory";
constexpr auto geometrySetting = "dialogs/sharedGeometry";
constexpr auto stateSetting = "dialogs/sharedState";
constexpr auto selectedFilterSetting = "dialogs/%1/selectedFilter";
constexpr auto legacyLastDirectorySetting = "recording/dialogLastDirectory";
constexpr auto legacyGeometrySetting = "recording/dialogGeometry";
constexpr auto legacyStateSetting = "recording/dialogState";
constexpr auto legacySelectedFilterSetting = "recording/dialogSelectedFilter";
constexpr int initialDialogWidth = 1'100;
constexpr int initialDialogHeight = 760;
constexpr int minimumDialogWidth = 860;
constexpr int minimumDialogHeight = 560;

QString accessibleDirectory(const QString& candidate)
{
    const QFileInfo info(candidate);
    if (!info.isDir() || !info.isReadable())
    {
        return {};
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

void appendSidebarUrl(QList<QUrl>& urls, const QString& path)
{
    const QString accessiblePath = accessibleDirectory(path);
    if (accessiblePath.isEmpty())
    {
        return;
    }
    const auto duplicate =
        std::any_of(urls.cbegin(), urls.cend(),
                    [&accessiblePath](const QUrl& url)
                    {
                        return url.isLocalFile() &&
                               QDir::cleanPath(url.toLocalFile()) == accessiblePath;
                    });
    if (!duplicate)
    {
        urls.append(QUrl::fromLocalFile(accessiblePath));
    }
}

} // namespace

ApplicationFileDialogs::ApplicationFileDialogs(
    SelectionHandler recordingLoader, SelectionHandler recordingDirectorySelected,
    SelectionHandler dsdFmeExecutableSelected,
    RecordingsDirectoryProvider recordingsDirectoryProvider,
    std::function<QString()> dsdFmePathProvider, QObject* parent)
    : QObject(parent), m_recordingLoader(std::move(recordingLoader)),
      m_recordingDirectorySelected(std::move(recordingDirectorySelected)),
      m_dsdFmeExecutableSelected(std::move(dsdFmeExecutableSelected)),
      m_recordingsDirectoryProvider(std::move(recordingsDirectoryProvider)),
      m_dsdFmePathProvider(std::move(dsdFmePathProvider))
{
}

ApplicationFileDialogs::~ApplicationFileDialogs() = default;

void ApplicationFileDialogs::openRecordingFileDialog()
{
    open(Purpose::Recording);
}
void ApplicationFileDialogs::selectRecordingDirectory()
{
    open(Purpose::RecordingDirectory);
}
void ApplicationFileDialogs::selectDsdFmeExecutable()
{
    open(Purpose::DsdFmeExecutable);
}
void ApplicationFileDialogs::open(Purpose purpose)
{
    ensureDialog();
    if (m_dialogOpen)
    {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }

    if (m_transientParent && m_dialog->windowHandle())
    {
        m_dialog->windowHandle()->setTransientParent(m_transientParent);
    }
    m_purpose = purpose;
    configure(purpose);
    m_dialog->setDirectory(initialDirectory(purpose));
    m_dialogOpen = true;
    emit dialogOpenChanged();
    m_dialog->show();
    if (m_transientParent && m_dialog->windowHandle())
    {
        m_dialog->windowHandle()->setTransientParent(m_transientParent);
    }
}

void ApplicationFileDialogs::setTransientParent(QWindow* parentWindow) noexcept
{
    m_transientParent = parentWindow;
    if (m_dialog && m_dialog->windowHandle())
    {
        m_dialog->windowHandle()->setTransientParent(m_transientParent);
    }
}

bool ApplicationFileDialogs::dialogOpen() const noexcept
{
    return m_dialogOpen;
}

QFileDialog* ApplicationFileDialogs::dialog() const noexcept
{
    return m_dialog.get();
}

void ApplicationFileDialogs::ensureDialog()
{
    if (m_dialog)
    {
        return;
    }

    m_dialog = std::make_unique<QFileDialog>();
    m_dialog->setWindowTitle(tr("Load recording"));
    m_dialog->setAccessibleName(tr("Load recording"));
    m_dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    m_dialog->setWindowModality(Qt::ApplicationModal);
    m_dialog->setNameFilters({
        tr("All supported recordings (*.wav *.WAV *.raw *.RAW)"),
        tr("WAV audio (*.wav *.WAV)"),
        tr("Raw IQ (*.raw *.RAW)"),
        tr("All files (*)"),
    });
    m_dialog->resize(initialDialogWidth, initialDialogHeight);
    m_dialog->setMinimumSize(minimumDialogWidth, minimumDialogHeight);
    restorePersistentState();
    configureSidebarLocations();

    connect(m_dialog.get(), &QFileDialog::accepted, this,
            &ApplicationFileDialogs::handleAccepted);
    connect(m_dialog.get(), &QFileDialog::rejected, this,
            &ApplicationFileDialogs::handleRejected);
}

void ApplicationFileDialogs::configure(Purpose purpose)
{
    m_dialog->setViewMode(QFileDialog::Detail);
    m_dialog->setAcceptMode(QFileDialog::AcceptOpen);
    m_dialog->setOption(QFileDialog::ShowDirsOnly,
                        purpose == Purpose::RecordingDirectory);
    if (purpose == Purpose::RecordingDirectory)
    {
        m_dialog->setWindowTitle(tr("Select recording folder"));
        m_dialog->setFileMode(QFileDialog::Directory);
        m_dialog->setNameFilters({});
    }
    else if (purpose == Purpose::DsdFmeExecutable)
    {
        m_dialog->setWindowTitle(tr("Select dsd-fme executable"));
        m_dialog->setFileMode(QFileDialog::ExistingFile);
        m_dialog->setNameFilters({tr("Executable files (*)"), tr("All files (*)")});
    }
    else
    {
        m_dialog->setWindowTitle(tr("Load recording"));
        m_dialog->setFileMode(QFileDialog::ExistingFile);
        m_dialog->setNameFilters(
            {tr("All supported recordings (*.wav *.WAV *.raw *.RAW)"),
             tr("WAV audio (*.wav *.WAV)"), tr("Raw IQ (*.raw *.RAW)"),
             tr("All files (*)")});
    }
    const QString name = purpose == Purpose::Recording            ? "recording"
                         : purpose == Purpose::RecordingDirectory ? "recordingDirectory"
                                                                  : "dsdFme";
    const QSettings settings;
    QString filter =
        settings.value(QString::fromLatin1(selectedFilterSetting).arg(name)).toString();
    if (filter.isEmpty() && purpose == Purpose::Recording)
    {
        filter = settings.value(legacySelectedFilterSetting).toString();
    }
    if (m_dialog->nameFilters().contains(filter))
        m_dialog->selectNameFilter(filter);
}

void ApplicationFileDialogs::configureSidebarLocations()
{
    QList<QUrl> sidebarUrls;
    appendSidebarUrl(sidebarUrls, m_recordingsDirectoryProvider
                                      ? m_recordingsDirectoryProvider()
                                      : QString());
    appendSidebarUrl(sidebarUrls,
                     QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    appendSidebarUrl(sidebarUrls, QDir::homePath());
    for (const QUrl& url : m_dialog->sidebarUrls())
    {
        if (url.isLocalFile())
        {
            appendSidebarUrl(sidebarUrls, url.toLocalFile());
        }
        else if (!sidebarUrls.contains(url))
        {
            sidebarUrls.append(url);
        }
    }
    m_dialog->setSidebarUrls(sidebarUrls);
}

void ApplicationFileDialogs::restorePersistentState()
{
    const QSettings settings;
    QByteArray geometry = settings.value(geometrySetting).toByteArray();
    if (geometry.isEmpty())
        geometry = settings.value(legacyGeometrySetting).toByteArray();
    if (!geometry.isEmpty())
    {
        static_cast<void>(m_dialog->restoreGeometry(geometry));
    }
    QByteArray state = settings.value(stateSetting).toByteArray();
    if (state.isEmpty())
        state = settings.value(legacyStateSetting).toByteArray();
    if (!state.isEmpty())
    {
        static_cast<void>(m_dialog->restoreState(state));
    }
}

void ApplicationFileDialogs::persistAcceptedState(const QString& filePath,
                                                  Purpose purpose)
{
    QSettings settings;
    const QString name = purpose == Purpose::Recording            ? "recording"
                         : purpose == Purpose::RecordingDirectory ? "recordingDirectory"
                                                                  : "dsdFme";
    const QString lastDirectory = purpose == Purpose::RecordingDirectory
                                      ? filePath
                                      : QFileInfo(filePath).absolutePath();
    settings.setValue(QString::fromLatin1(lastDirectorySetting).arg(name),
                      lastDirectory);
    settings.setValue(geometrySetting, m_dialog->saveGeometry());
    settings.setValue(stateSetting, m_dialog->saveState());
    settings.setValue(QString::fromLatin1(selectedFilterSetting).arg(name),
                      m_dialog->selectedNameFilter());
}

QString ApplicationFileDialogs::initialDirectory(Purpose purpose) const
{
    const QSettings settings;
    const QString name = purpose == Purpose::Recording            ? "recording"
                         : purpose == Purpose::RecordingDirectory ? "recordingDirectory"
                                                                  : "dsdFme";
    QString configured;
    if (purpose == Purpose::DsdFmeExecutable && m_dsdFmePathProvider)
    {
        const QString executablePath = m_dsdFmePathProvider().trimmed();
        if (!executablePath.isEmpty())
            configured = QFileInfo(executablePath).absolutePath();
    }
    else if (m_recordingsDirectoryProvider)
    {
        configured = m_recordingsDirectoryProvider();
    }
    const QString configuredDirectory = accessibleDirectory(configured);
    QString rememberedPath =
        settings.value(QString::fromLatin1(lastDirectorySetting).arg(name)).toString();
    if (rememberedPath.isEmpty() && purpose == Purpose::Recording)
    {
        rememberedPath = settings.value(legacyLastDirectorySetting).toString();
    }
    const QString remembered = accessibleDirectory(rememberedPath);
    if (purpose != Purpose::Recording && !configuredDirectory.isEmpty())
        return configuredDirectory;
    if (!remembered.isEmpty())
    {
        return remembered;
    }
    if (!configuredDirectory.isEmpty())
        return configuredDirectory;
    const QString recordingsDirectory = accessibleDirectory(
        m_recordingsDirectoryProvider ? m_recordingsDirectoryProvider() : QString());
    if (!recordingsDirectory.isEmpty())
    {
        return recordingsDirectory;
    }
    const QString musicDirectory = accessibleDirectory(
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    if (!musicDirectory.isEmpty())
    {
        return musicDirectory;
    }
    return QDir::homePath();
}

void ApplicationFileDialogs::handleAccepted()
{
    const QStringList selectedFiles = m_dialog->selectedFiles();
    const QString selectedFile = selectedFiles.isEmpty()
                                     ? QString()
                                     : QDir::cleanPath(selectedFiles.constFirst());
    m_dialogOpen = false;
    emit dialogOpenChanged();
    const QFileInfo info(selectedFile);
    if (m_purpose == Purpose::RecordingDirectory && !info.isDir())
    {
        emit selectionError(tr("The selected recording folder does not exist."));
        return;
    }
    if (m_purpose == Purpose::RecordingDirectory && !info.isWritable())
    {
        emit selectionError(tr("The selected recording folder is not writable."));
        return;
    }
    if (m_purpose != Purpose::RecordingDirectory && !info.isFile())
    {
        emit selectionError(tr("The selected path is not a regular file."));
        return;
    }
    if (m_purpose == Purpose::DsdFmeExecutable && !info.isExecutable())
    {
        emit selectionError(tr("The selected DSD-FME file is not executable."));
        return;
    }

    const QUrl fileUrl = QUrl::fromLocalFile(info.absoluteFilePath());
    persistAcceptedState(info.absoluteFilePath(), m_purpose);
    if (m_purpose == Purpose::Recording)
    {
        emit recordingFileSelected(fileUrl);
        if (m_recordingLoader)
            m_recordingLoader(fileUrl);
    }
    else if (m_purpose == Purpose::RecordingDirectory)
    {
        if (m_recordingDirectorySelected)
            m_recordingDirectorySelected(fileUrl);
    }
    else if (m_dsdFmeExecutableSelected)
        m_dsdFmeExecutableSelected(fileUrl);
}

void ApplicationFileDialogs::handleRejected()
{
    if (!m_dialogOpen)
    {
        return;
    }
    m_dialogOpen = false;
    emit dialogOpenChanged();
}

} // namespace sdr::gui
