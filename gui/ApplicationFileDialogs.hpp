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

namespace sdr::gui
{

class ApplicationFileDialogs final : public QObject
{
    Q_OBJECT

  public:
    using SelectionHandler = std::function<void(const QUrl&)>;
    using RecordingsDirectoryProvider = std::function<QString()>;

    ApplicationFileDialogs(SelectionHandler recordingLoader,
                           SelectionHandler recordingDirectorySelected,
                           SelectionHandler dsdFmeExecutableSelected,
                           RecordingsDirectoryProvider recordingsDirectoryProvider,
                           std::function<QString()> dsdFmePathProvider,
                           QObject* parent = nullptr);
    ~ApplicationFileDialogs() override;

    ApplicationFileDialogs(const ApplicationFileDialogs&) = delete;
    ApplicationFileDialogs& operator=(const ApplicationFileDialogs&) = delete;

    Q_INVOKABLE void openRecordingFileDialog();
    Q_INVOKABLE void selectRecordingDirectory();
    Q_INVOKABLE void selectDsdFmeExecutable();
    void setTransientParent(QWindow* parentWindow) noexcept;

    [[nodiscard]] bool dialogOpen() const noexcept;
    [[nodiscard]] QFileDialog* dialog() const noexcept;

  signals:
    void recordingFileSelected(const QUrl& fileUrl);
    void selectionError(const QString& message);
    void dialogOpenChanged();

  private:
    enum class Purpose
    {
        Recording,
        RecordingDirectory,
        DsdFmeExecutable
    };
    void open(Purpose purpose);
    void ensureDialog();
    void configure(Purpose purpose);
    void configureSidebarLocations();
    void restorePersistentState();
    void persistAcceptedState(const QString& filePath, Purpose purpose);
    [[nodiscard]] QString initialDirectory(Purpose purpose) const;

  private slots:
    void handleAccepted();
    void handleRejected();

  private:
    SelectionHandler m_recordingLoader;
    SelectionHandler m_recordingDirectorySelected;
    SelectionHandler m_dsdFmeExecutableSelected;
    RecordingsDirectoryProvider m_recordingsDirectoryProvider;
    std::function<QString()> m_dsdFmePathProvider;
    std::unique_ptr<QFileDialog> m_dialog;
    QWindow* m_transientParent = nullptr;
    bool m_dialogOpen = false;
    Purpose m_purpose = Purpose::Recording;
};

} // namespace sdr::gui
