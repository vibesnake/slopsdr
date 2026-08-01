// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QFileDialog;
class QWindow;

namespace sdr::gui {

class RecordingFileDialogController final : public QObject
{
    Q_OBJECT

public:
    using RecordingLoader = std::function<void(const QUrl&)>;
    using RecordingsDirectoryProvider = std::function<QString()>;

    RecordingFileDialogController(
        RecordingLoader recordingLoader,
        RecordingsDirectoryProvider recordingsDirectoryProvider,
        QObject* parent = nullptr);
    ~RecordingFileDialogController() override;

    RecordingFileDialogController(const RecordingFileDialogController&) = delete;
    RecordingFileDialogController& operator=(
        const RecordingFileDialogController&) = delete;

    Q_INVOKABLE void open();
    void setTransientParent(QWindow* parentWindow) noexcept;

    [[nodiscard]] bool dialogOpen() const noexcept;
    [[nodiscard]] QFileDialog* dialog() const noexcept;

signals:
    void recordingFileSelected(const QUrl& fileUrl);
    void dialogOpenChanged();

private:
    void ensureDialog();
    void configureSidebarLocations();
    void restorePersistentState();
    void persistAcceptedState(const QString& filePath);
    [[nodiscard]] QString initialDirectory() const;
    void handleAccepted();
    void handleRejected();

    RecordingLoader m_recordingLoader;
    RecordingsDirectoryProvider m_recordingsDirectoryProvider;
    std::unique_ptr<QFileDialog> m_dialog;
    QWindow* m_transientParent = nullptr;
    bool m_dialogOpen = false;
};

}  // namespace sdr::gui
