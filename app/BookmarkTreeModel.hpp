// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "BookmarkJsonStore.hpp"

#include <QAbstractListModel>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QUuid>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace sdr::app {

struct BookmarkData {
    QString name;
    quint64 listeningFrequency = 0;
    double requestedGainDb = 0.0;
    QString demodulatorId;
    qint64 filterLowHz = 0;
    qint64 filterHighHz = 0;
    double squelchThresholdDb = -80.0;
    bool squelchEnabled = true;
    bool hasSavedSquelch = true;
    QJsonObject modeSpecificSettings{{QStringLiteral("version"), 1}};
    bool scannerIncluded = false;
};

class BookmarkTreeModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString filePath READ filePath CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool persistencePending READ persistencePending NOTIFY persistencePendingChanged)

public:
    enum Role {
        UuidRole = Qt::UserRole + 1,
        ItemTypeRole,
        NameRole,
        DepthRole,
        GroupRole,
        ExpandedRole,
        HasChildrenRole,
        ScannerCheckStateRole,
        ScannerIncludedRole,
        ListeningFrequencyRole,
        RequestedGainRole,
        DemodulatorIdRole,
        DemodulatorNameRole,
        DemodulatorAvailableRole,
        FilterLowHzRole,
        FilterHighHzRole,
        SquelchThresholdRole,
        SquelchEnabledRole,
        ModeSpecificSettingsRole,
    };
    Q_ENUM(Role)

    explicit BookmarkTreeModel(
        QString filePath = {}, QObject* parent = nullptr);
    ~BookmarkTreeModel() override;

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] bool persistencePending() const noexcept;
    [[nodiscard]] static QString defaultFilePath();

    Q_INVOKABLE bool setExpanded(int visibleRow, bool expanded);
    Q_INVOKABLE bool toggleExpanded(int visibleRow);
    Q_INVOKABLE bool toggleScannerInclusion(int visibleRow);
    Q_INVOKABLE QString addGroup(int parentVisibleRow, const QString& name);
    [[nodiscard]] QString addBookmark(
        int parentVisibleRow, const BookmarkData& bookmark);
    Q_INVOKABLE QVariantMap itemDetails(int visibleRow) const;
    Q_INVOKABLE bool renameGroup(int visibleRow, const QString& name);
    Q_INVOKABLE bool updateBookmark(int visibleRow, const QVariantMap& fields);
    [[nodiscard]] std::optional<BookmarkData> bookmarkAt(int visibleRow) const;
    Q_INVOKABLE bool removeItem(int visibleRow);
    Q_INVOKABLE int visibleRowForUuid(const QString& uuid) const;
    Q_INVOKABLE bool expandGroupForDrop(int visibleRow);
    Q_INVOKABLE bool moveBookmark(
        const QString& sourceUuid,
        const QString& targetUuid,
        const QString& placement);

    [[nodiscard]] bool reload();
    [[nodiscard]] bool saveNow();

signals:
    void lastErrorChanged();
    void loadingChanged();
    void persistencePendingChanged();

private:
    struct Node;

    [[nodiscard]] Node* insertionParent(int visibleRow) const;
    [[nodiscard]] Node* findNode(const QUuid& uuid) const;
    [[nodiscard]] int visibleSubtreeEnd(const Node& node) const;
    [[nodiscard]] QJsonDocument serializedDocument() const;
    void restoreDocument(const QJsonDocument& document);
    [[nodiscard]] Qt::CheckState scannerCheckState(const Node& node) const;
    void setScannerIncludedRecursively(Node& node, bool included);
    void rebuildVisibleNodes();
    void appendVisibleChildren(Node& parent, int depth);
    void resetToEmpty();
    void setLastError(QString error);
    [[nodiscard]] QJsonObject serializeNode(const Node& node) const;
    [[nodiscard]] std::unique_ptr<Node> parseNode(
        const QJsonObject& object,
        Node* parent,
        std::vector<QUuid>& seenUuids,
        int depth,
        QString& error) const;
    void applyLoadResult(
        sdr::platform::BookmarkJsonStore::LoadResult result,
        quint64 requestedRevision);
    void applySaveResult(bool succeeded, QString error, quint64 generation);

    QString m_filePath;
    std::shared_ptr<sdr::platform::BookmarkJsonStore> m_store;
    QThread m_persistenceThread;
    QObject* m_persistenceContext = nullptr;
    QString m_lastError;
    std::unique_ptr<Node> m_root;
    QJsonObject m_documentExtensions;
    QVector<Node*> m_visibleNodes;
    QVector<int> m_visibleDepths;
    quint64 m_revision = 0;
    quint64 m_saveGeneration = 0;
    bool m_loading = false;
    bool m_persistencePending = false;
    QJsonDocument m_moveRollbackDocument;
    quint64 m_moveSaveGeneration = 0;
};

}  // namespace sdr::app
