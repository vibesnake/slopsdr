// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordingFileDialogController.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace sdr::gui {
namespace {

constexpr auto lastDirectorySetting = "recording/dialogLastDirectory";
constexpr auto geometrySetting = "recording/dialogGeometry";
constexpr auto stateSetting = "recording/dialogState";
constexpr auto selectedFilterSetting = "recording/dialogSelectedFilter";
constexpr int initialDialogWidth = 1'100;
constexpr int initialDialogHeight = 760;
constexpr int minimumDialogWidth = 860;
constexpr int minimumDialogHeight = 560;

QString accessibleDirectory(const QString& candidate)
{
    const QFileInfo info(candidate);
    if (!info.isDir() || !info.isReadable()) {
        return {};
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

void appendSidebarUrl(QList<QUrl>& urls, const QString& path)
{
    const QString accessiblePath = accessibleDirectory(path);
    if (accessiblePath.isEmpty()) {
        return;
    }
    const auto duplicate = std::any_of(
        urls.cbegin(), urls.cend(), [&accessiblePath](const QUrl& url) {
            return url.isLocalFile() &&
                   QDir::cleanPath(url.toLocalFile()) == accessiblePath;
        });
    if (!duplicate) {
        urls.append(QUrl::fromLocalFile(accessiblePath));
    }
}

}  // namespace

RecordingFileDialogController::RecordingFileDialogController(
    RecordingLoader recordingLoader,
    RecordingsDirectoryProvider recordingsDirectoryProvider,
    QObject* parent)
    : QObject(parent)
    , m_recordingLoader(std::move(recordingLoader))
    , m_recordingsDirectoryProvider(std::move(recordingsDirectoryProvider))
{
}

RecordingFileDialogController::~RecordingFileDialogController() = default;

void RecordingFileDialogController::open()
{
    ensureDialog();
    if (m_dialogOpen) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }

    if (m_transientParent && m_dialog->windowHandle()) {
        m_dialog->windowHandle()->setTransientParent(m_transientParent);
    }
    m_dialog->setDirectory(initialDirectory());
    m_dialogOpen = true;
    emit dialogOpenChanged();
    m_dialog->show();
    if (m_transientParent && m_dialog->windowHandle()) {
        m_dialog->windowHandle()->setTransientParent(m_transientParent);
    }
}

void RecordingFileDialogController::setTransientParent(
    QWindow* parentWindow) noexcept
{
    m_transientParent = parentWindow;
    if (m_dialog && m_dialog->windowHandle()) {
        m_dialog->windowHandle()->setTransientParent(m_transientParent);
    }
}

bool RecordingFileDialogController::dialogOpen() const noexcept
{
    return m_dialogOpen;
}

QFileDialog* RecordingFileDialogController::dialog() const noexcept
{
    return m_dialog.get();
}

void RecordingFileDialogController::ensureDialog()
{
    if (m_dialog) {
        return;
    }

    m_dialog = std::make_unique<QFileDialog>();
    m_dialog->setWindowTitle(tr("Load recording"));
    m_dialog->setAccessibleName(tr("Load recording"));
    m_dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    m_dialog->setFileMode(QFileDialog::ExistingFile);
    m_dialog->setAcceptMode(QFileDialog::AcceptOpen);
    m_dialog->setViewMode(QFileDialog::Detail);
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

    connect(
        m_dialog.get(), &QFileDialog::accepted,
        this, &RecordingFileDialogController::handleAccepted);
    connect(
        m_dialog.get(), &QFileDialog::rejected,
        this, &RecordingFileDialogController::handleRejected);
}

void RecordingFileDialogController::configureSidebarLocations()
{
    QList<QUrl> sidebarUrls;
    appendSidebarUrl(
        sidebarUrls,
        m_recordingsDirectoryProvider ? m_recordingsDirectoryProvider() : QString());
    appendSidebarUrl(
        sidebarUrls,
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    appendSidebarUrl(sidebarUrls, QDir::homePath());
    for (const QUrl& url : m_dialog->sidebarUrls()) {
        if (url.isLocalFile()) {
            appendSidebarUrl(sidebarUrls, url.toLocalFile());
        } else if (!sidebarUrls.contains(url)) {
            sidebarUrls.append(url);
        }
    }
    m_dialog->setSidebarUrls(sidebarUrls);
}

void RecordingFileDialogController::restorePersistentState()
{
    const QSettings settings;
    const QByteArray geometry = settings.value(geometrySetting).toByteArray();
    if (!geometry.isEmpty()) {
        static_cast<void>(m_dialog->restoreGeometry(geometry));
    }
    const QByteArray state = settings.value(stateSetting).toByteArray();
    if (!state.isEmpty()) {
        static_cast<void>(m_dialog->restoreState(state));
    }
    const QString selectedFilter = settings.value(selectedFilterSetting).toString();
    if (m_dialog->nameFilters().contains(selectedFilter)) {
        m_dialog->selectNameFilter(selectedFilter);
    }
}

void RecordingFileDialogController::persistAcceptedState(const QString& filePath)
{
    QSettings settings;
    settings.setValue(lastDirectorySetting, QFileInfo(filePath).absolutePath());
    settings.setValue(geometrySetting, m_dialog->saveGeometry());
    settings.setValue(stateSetting, m_dialog->saveState());
    settings.setValue(selectedFilterSetting, m_dialog->selectedNameFilter());
}

QString RecordingFileDialogController::initialDirectory() const
{
    const QSettings settings;
    const QString remembered = accessibleDirectory(
        settings.value(lastDirectorySetting).toString());
    if (!remembered.isEmpty()) {
        return remembered;
    }
    const QString recordingsDirectory = accessibleDirectory(
        m_recordingsDirectoryProvider ? m_recordingsDirectoryProvider() : QString());
    if (!recordingsDirectory.isEmpty()) {
        return recordingsDirectory;
    }
    const QString musicDirectory = accessibleDirectory(
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    if (!musicDirectory.isEmpty()) {
        return musicDirectory;
    }
    return QDir::homePath();
}

void RecordingFileDialogController::handleAccepted()
{
    const QStringList selectedFiles = m_dialog->selectedFiles();
    const QString selectedFile = selectedFiles.isEmpty()
                                     ? QString()
                                     : QDir::cleanPath(selectedFiles.constFirst());
    m_dialogOpen = false;
    emit dialogOpenChanged();
    const QFileInfo info(selectedFile);
    if (!info.isFile()) {
        return;
    }

    const QUrl fileUrl = QUrl::fromLocalFile(info.absoluteFilePath());
    persistAcceptedState(info.absoluteFilePath());
    emit recordingFileSelected(fileUrl);
    if (m_recordingLoader) {
        m_recordingLoader(fileUrl);
    }
}

void RecordingFileDialogController::handleRejected()
{
    if (!m_dialogOpen) {
        return;
    }
    m_dialogOpen = false;
    emit dialogOpenChanged();
}

}  // namespace sdr::gui
