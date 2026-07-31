// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "BookmarkTreeModel.hpp"

#include "DemodulatorRegistry.hpp"
#include "BookmarkLimits.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sdr::app {

namespace {

constexpr int bookmarkFileVersion = 1;
constexpr int modeSettingsMinimumVersion = 1;
constexpr double maximumExactJsonInteger = 9'007'199'254'740'991.0;

bool strictInteger(const QJsonValue& value, qint64& result) noexcept
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < -maximumExactJsonInteger ||
        number > maximumExactJsonInteger) {
        return false;
    }
    result = static_cast<qint64>(number);
    return true;
}

bool strictUnsignedInteger(const QJsonValue& value, quint64& result) noexcept
{
    qint64 signedValue = 0;
    if (!strictInteger(value, signedValue) || signedValue < 0) {
        return false;
    }
    result = static_cast<quint64>(signedValue);
    return true;
}

bool validModeSpecificSettings(const QJsonObject& settings) noexcept
{
    qint64 version = 0;
    return strictInteger(settings.value(QStringLiteral("version")), version) &&
           version >= modeSettingsMinimumVersion;
}

bool fitsUtf8Bytes(const QString& value, qsizetype maximumBytes)
{
    return value.toUtf8().size() <= maximumBytes;
}

bool fitsCompactJsonPayload(const QJsonObject& value)
{
    return QJsonDocument(value).toJson(QJsonDocument::Compact).size() <=
           platform::bookmarkLimits::maximumJsonPayloadBytes;
}

bool validBookmarkPayload(
    const QString& name,
    const QString& demodulatorId,
    const QJsonObject& modeSpecificSettings,
    QString& error)
{
    if (!fitsUtf8Bytes(name, platform::bookmarkLimits::maximumNameUtf8Bytes)) {
        error = QStringLiteral("Bookmark name exceeds the 256-byte limit");
        return false;
    }
    if (!fitsUtf8Bytes(
            demodulatorId,
            platform::bookmarkLimits::maximumDemodulatorIdUtf8Bytes)) {
        error = QStringLiteral("Bookmark demodulator ID exceeds the 128-byte limit");
        return false;
    }
    if (!validModeSpecificSettings(modeSpecificSettings) ||
        !fitsCompactJsonPayload(modeSpecificSettings)) {
        error = QStringLiteral("Bookmark mode settings are invalid or exceed the 64 KiB limit");
        return false;
    }
    return true;
}

QString normalizedName(const QString& name, const QString& fallback)
{
    const QString trimmed = name.trimmed();
    return trimmed.isEmpty() ? fallback : trimmed;
}

}  // namespace

struct BookmarkTreeModel::Node {
    QUuid uuid = QUuid::createUuid();
    bool group = true;
    QString groupName;
    bool expanded = false;
    BookmarkData bookmark;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    QJsonObject extensions;
};

BookmarkTreeModel::BookmarkTreeModel(QString filePath, QObject* parent)
    : QAbstractListModel(parent)
    , m_filePath(filePath.isEmpty() ? defaultFilePath() : std::move(filePath))
    , m_store(std::make_shared<sdr::platform::BookmarkJsonStore>(m_filePath))
{
    resetToEmpty();
    m_persistenceContext = new QObject();
    m_persistenceContext->moveToThread(&m_persistenceThread);
    connect(
        &m_persistenceThread,
        &QThread::finished,
        m_persistenceContext,
        &QObject::deleteLater);
    m_persistenceThread.start();
    static_cast<void>(reload());
}

BookmarkTreeModel::~BookmarkTreeModel()
{
    if (m_persistenceThread.isRunning() && m_persistenceContext) {
        static_cast<void>(QMetaObject::invokeMethod(
            m_persistenceContext,
            [] {},
            Qt::BlockingQueuedConnection));
    }
    m_persistenceThread.quit();
    m_persistenceThread.wait();
}

int BookmarkTreeModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid()
               ? 0
               : static_cast<int>(std::min<qsizetype>(
                     m_visibleNodes.size(),
                     std::numeric_limits<int>::max()));
}

QVariant BookmarkTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= m_visibleNodes.size()) {
        return {};
    }
    const Node& node = *m_visibleNodes.at(index.row());
    const BookmarkData& bookmark = node.bookmark;
    switch (role) {
    case UuidRole:
        return node.uuid.toString(QUuid::WithoutBraces);
    case ItemTypeRole:
        return node.group ? QStringLiteral("group") : QStringLiteral("bookmark");
    case NameRole:
        return node.group ? node.groupName : bookmark.name;
    case DepthRole:
        return m_visibleDepths.at(index.row());
    case GroupRole:
        return node.group;
    case ExpandedRole:
        return node.group && node.expanded;
    case HasChildrenRole:
        return node.group && !node.children.empty();
    case ScannerCheckStateRole:
        return static_cast<int>(scannerCheckState(node));
    case ScannerIncludedRole:
        return !node.group && bookmark.scannerIncluded;
    case ListeningFrequencyRole:
        return QVariant::fromValue<qulonglong>(bookmark.listeningFrequency);
    case RequestedGainRole:
        return bookmark.requestedGainDb;
    case DemodulatorIdRole:
        return bookmark.demodulatorId;
    case DemodulatorNameRole: {
        if (node.group) {
            return {};
        }
        const auto id = bookmark.demodulatorId.toStdString();
        if (const auto* descriptor =
                sdr::radio::DemodulatorRegistry::findById(id)) {
            return QString::fromLatin1(
                descriptor->displayName.data(),
                static_cast<qsizetype>(descriptor->displayName.size()));
        }
        return QStringLiteral("Unavailable · %1").arg(bookmark.demodulatorId);
    }
    case DemodulatorAvailableRole:
        return node.group ||
               sdr::radio::DemodulatorRegistry::findById(
                   bookmark.demodulatorId.toStdString()) != nullptr;
    case FilterLowHzRole:
        return QVariant::fromValue<qlonglong>(bookmark.filterLowHz);
    case FilterHighHzRole:
        return QVariant::fromValue<qlonglong>(bookmark.filterHighHz);
    case SquelchThresholdRole:
        return bookmark.squelchThresholdDb;
    case SquelchEnabledRole:
        return bookmark.squelchEnabled;
    case ModeSpecificSettingsRole:
        return bookmark.modeSpecificSettings.toVariantMap();
    default:
        return {};
    }
}

QHash<int, QByteArray> BookmarkTreeModel::roleNames() const
{
    return {
        {UuidRole, "uuid"},
        {ItemTypeRole, "itemType"},
        {NameRole, "name"},
        {DepthRole, "depth"},
        {GroupRole, "isGroup"},
        {ExpandedRole, "expanded"},
        {HasChildrenRole, "hasChildren"},
        {ScannerCheckStateRole, "scannerCheckState"},
        {ScannerIncludedRole, "scannerIncluded"},
        {ListeningFrequencyRole, "listeningFrequency"},
        {RequestedGainRole, "requestedGain"},
        {DemodulatorIdRole, "demodulatorId"},
        {DemodulatorNameRole, "demodulatorName"},
        {DemodulatorAvailableRole, "demodulatorAvailable"},
        {FilterLowHzRole, "filterLowHz"},
        {FilterHighHzRole, "filterHighHz"},
        {SquelchThresholdRole, "squelchThreshold"},
        {SquelchEnabledRole, "squelchEnabled"},
        {ModeSpecificSettingsRole, "modeSpecificSettings"},
    };
}

QString BookmarkTreeModel::filePath() const
{
    return m_filePath;
}

QString BookmarkTreeModel::lastError() const
{
    return m_lastError;
}

bool BookmarkTreeModel::loading() const noexcept
{
    return m_loading;
}

bool BookmarkTreeModel::persistencePending() const noexcept
{
    return m_persistencePending;
}

QString BookmarkTreeModel::defaultFilePath()
{
    return sdr::platform::BookmarkJsonStore::defaultFilePath();
}

bool BookmarkTreeModel::setExpanded(int visibleRow, bool expanded)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size()) {
        return false;
    }
    Node& node = *m_visibleNodes.at(visibleRow);
    if (!node.group || node.expanded == expanded) {
        return node.group;
    }
    node.expanded = expanded;
    beginResetModel();
    rebuildVisibleNodes();
    endResetModel();
    return saveNow();
}

bool BookmarkTreeModel::toggleExpanded(int visibleRow)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size() ||
        !m_visibleNodes.at(visibleRow)->group) {
        return false;
    }
    return setExpanded(visibleRow, !m_visibleNodes.at(visibleRow)->expanded);
}

bool BookmarkTreeModel::toggleScannerInclusion(int visibleRow)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size()) {
        return false;
    }
    Node& node = *m_visibleNodes.at(visibleRow);
    const bool include = scannerCheckState(node) != Qt::Checked;
    setScannerIncludedRecursively(node, include);
    beginResetModel();
    rebuildVisibleNodes();
    endResetModel();
    return saveNow();
}

QString BookmarkTreeModel::addGroup(int parentVisibleRow, const QString& name)
{
    Node* parent = insertionParent(parentVisibleRow);
    const QString groupName = normalizedName(name, QStringLiteral("New group"));
    if (!parent) {
        return {};
    }
    if (nodeCount() >= platform::bookmarkLimits::maximumNodeCount) {
        setLastError(QStringLiteral("Bookmarks tree exceeds the 4,096-node limit"));
        return {};
    }
    if (nodeDepth(*parent) + 1 > platform::bookmarkLimits::maximumTreeDepth) {
        setLastError(QStringLiteral("Bookmarks tree exceeds the maximum nesting depth"));
        return {};
    }
    if (!fitsUtf8Bytes(name, platform::bookmarkLimits::maximumNameUtf8Bytes) ||
        !fitsUtf8Bytes(groupName, platform::bookmarkLimits::maximumNameUtf8Bytes)) {
        setLastError(QStringLiteral("Bookmark group name exceeds the 256-byte limit"));
        return {};
    }
    auto group = std::make_unique<Node>();
    group->groupName = groupName;
    group->expanded = true;
    group->parent = parent;
    const QString uuid = group->uuid.toString(QUuid::WithoutBraces);
    parent->children.push_back(std::move(group));
    parent->expanded = true;
    beginResetModel();
    rebuildVisibleNodes();
    endResetModel();
    static_cast<void>(saveNow());
    return uuid;
}

QString BookmarkTreeModel::addBookmark(
    int parentVisibleRow, const BookmarkData& bookmark)
{
    Node* parent = insertionParent(parentVisibleRow);
    const QString name = normalizedName(bookmark.name, QStringLiteral("Unnamed bookmark"));
    const QString demodulatorId = bookmark.demodulatorId.trimmed();
    if (!fitsUtf8Bytes(bookmark.name, platform::bookmarkLimits::maximumNameUtf8Bytes)) {
        setLastError(QStringLiteral("Bookmark name exceeds the 256-byte limit"));
        return {};
    }
    if (!fitsUtf8Bytes(
            bookmark.demodulatorId,
            platform::bookmarkLimits::maximumDemodulatorIdUtf8Bytes)) {
        setLastError(QStringLiteral("Bookmark demodulator ID exceeds the 128-byte limit"));
        return {};
    }
    QString error;
    if (!parent || demodulatorId.isEmpty() ||
        !std::isfinite(bookmark.requestedGainDb) ||
        !std::isfinite(bookmark.squelchThresholdDb) ||
        bookmark.filterLowHz > bookmark.filterHighHz ||
        !validBookmarkPayload(name, demodulatorId, bookmark.modeSpecificSettings, error)) {
        if (!error.isEmpty()) setLastError(std::move(error));
        return {};
    }
    if (nodeCount() >= platform::bookmarkLimits::maximumNodeCount) {
        setLastError(QStringLiteral("Bookmarks tree exceeds the 4,096-node limit"));
        return {};
    }
    if (nodeDepth(*parent) + 1 > platform::bookmarkLimits::maximumTreeDepth) {
        setLastError(QStringLiteral("Bookmarks tree exceeds the maximum nesting depth"));
        return {};
    }
    auto node = std::make_unique<Node>();
    node->group = false;
    node->bookmark = bookmark;
    node->bookmark.name = name;
    node->bookmark.demodulatorId = demodulatorId;
    node->parent = parent;
    const QString uuid = node->uuid.toString(QUuid::WithoutBraces);
    parent->children.push_back(std::move(node));
    parent->expanded = true;
    beginResetModel();
    rebuildVisibleNodes();
    endResetModel();
    static_cast<void>(saveNow());
    return uuid;
}

QVariantMap BookmarkTreeModel::itemDetails(int visibleRow) const
{
    QVariantMap details;
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size()) return details;
    const Node& node = *m_visibleNodes.at(visibleRow);
    details.insert(QStringLiteral("uuid"), node.uuid.toString(QUuid::WithoutBraces));
    details.insert(QStringLiteral("isGroup"), node.group);
    details.insert(QStringLiteral("name"), node.group ? node.groupName : node.bookmark.name);
    details.insert(QStringLiteral("hasChildren"), node.group && !node.children.empty());
    if (node.group) return details;
    const auto& bookmark = node.bookmark;
    details.insert(QStringLiteral("listeningFrequency"), QVariant::fromValue<qulonglong>(bookmark.listeningFrequency));
    details.insert(QStringLiteral("requestedGain"), bookmark.requestedGainDb);
    details.insert(QStringLiteral("demodulatorId"), bookmark.demodulatorId);
    details.insert(QStringLiteral("filterLowHz"), QVariant::fromValue<qlonglong>(bookmark.filterLowHz));
    details.insert(QStringLiteral("filterHighHz"), QVariant::fromValue<qlonglong>(bookmark.filterHighHz));
    details.insert(QStringLiteral("squelchThreshold"), bookmark.squelchThresholdDb);
    details.insert(QStringLiteral("squelchEnabled"), bookmark.squelchEnabled);
    details.insert(QStringLiteral("modeSpecificSettings"), bookmark.modeSpecificSettings.toVariantMap());
    details.insert(QStringLiteral("scannerIncluded"), bookmark.scannerIncluded);
    return details;
}

bool BookmarkTreeModel::renameGroup(int visibleRow, const QString& name)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size() ||
        !m_visibleNodes.at(visibleRow)->group || name.trimmed().isEmpty()) return false;
    const QString trimmed = name.trimmed();
    if (!fitsUtf8Bytes(trimmed, platform::bookmarkLimits::maximumNameUtf8Bytes)) {
        setLastError(QStringLiteral("Bookmark group name exceeds the 256-byte limit"));
        return false;
    }
    m_visibleNodes.at(visibleRow)->groupName = trimmed;
    emit dataChanged(index(visibleRow, 0), index(visibleRow, 0), {NameRole});
    return saveNow();
}

bool BookmarkTreeModel::updateBookmark(int visibleRow, const QVariantMap& fields)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size() ||
        m_visibleNodes.at(visibleRow)->group) return false;
    const QString inputName = fields.value(QStringLiteral("name")).toString();
    const QString inputDemodulatorId =
        fields.value(QStringLiteral("demodulatorId")).toString();
    BookmarkData updated;
    updated.name = inputName.trimmed();
    updated.listeningFrequency = fields.value(QStringLiteral("listeningFrequency")).toULongLong();
    updated.requestedGainDb = fields.value(QStringLiteral("requestedGain")).toDouble();
    updated.demodulatorId = inputDemodulatorId.trimmed();
    updated.filterLowHz = fields.value(QStringLiteral("filterLowHz")).toLongLong();
    updated.filterHighHz = fields.value(QStringLiteral("filterHighHz")).toLongLong();
    updated.squelchThresholdDb = fields.value(QStringLiteral("squelchThreshold")).toDouble();
    updated.squelchEnabled = fields.value(QStringLiteral("squelchEnabled")).toBool();
    updated.hasSavedSquelch = true;
    updated.modeSpecificSettings = QJsonObject::fromVariantMap(fields.value(QStringLiteral("modeSpecificSettings")).toMap());
    updated.scannerIncluded = fields.value(QStringLiteral("scannerIncluded")).toBool();
    QString error;
    if (!fitsUtf8Bytes(inputName, platform::bookmarkLimits::maximumNameUtf8Bytes)) {
        setLastError(QStringLiteral("Bookmark name exceeds the 256-byte limit"));
        return false;
    }
    if (!fitsUtf8Bytes(
            inputDemodulatorId,
            platform::bookmarkLimits::maximumDemodulatorIdUtf8Bytes)) {
        setLastError(QStringLiteral("Bookmark demodulator ID exceeds the 128-byte limit"));
        return false;
    }
    if (updated.name.isEmpty() || updated.demodulatorId.isEmpty() ||
        !std::isfinite(updated.requestedGainDb) || !std::isfinite(updated.squelchThresholdDb) ||
        updated.filterLowHz > updated.filterHighHz ||
        !validBookmarkPayload(
            updated.name, updated.demodulatorId, updated.modeSpecificSettings, error)) {
        if (!error.isEmpty()) setLastError(std::move(error));
        return false;
    }
    m_visibleNodes.at(visibleRow)->bookmark = std::move(updated);
    emit dataChanged(index(visibleRow, 0), index(visibleRow, 0));
    return saveNow();
}

std::optional<BookmarkData> BookmarkTreeModel::bookmarkAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size() || m_visibleNodes.at(visibleRow)->group) return std::nullopt;
    return m_visibleNodes.at(visibleRow)->bookmark;
}

std::vector<BookmarkSnapshot> BookmarkTreeModel::scannerBookmarks() const
{
    std::vector<BookmarkSnapshot> bookmarks;
    appendScannerBookmarks(*m_root, bookmarks);
    return bookmarks;
}

bool BookmarkTreeModel::removeItem(int visibleRow)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size()) {
        return false;
    }
    Node* node = m_visibleNodes.at(visibleRow);
    Node* parent = node->parent;
    if (!parent) {
        return false;
    }
    const auto match = std::find_if(
        parent->children.begin(),
        parent->children.end(),
        [node](const std::unique_ptr<Node>& child) {
            return child.get() == node;
        });
    if (match == parent->children.end()) {
        return false;
    }
    beginResetModel();
    parent->children.erase(match);
    rebuildVisibleNodes();
    endResetModel();
    return saveNow();
}

bool BookmarkTreeModel::expandGroupForDrop(int visibleRow)
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size()) {
        return false;
    }
    Node& node = *m_visibleNodes.at(visibleRow);
    if (!node.group) {
        return false;
    }
    if (!node.expanded) {
        node.expanded = true;
        beginResetModel();
        rebuildVisibleNodes();
        endResetModel();
    }
    return true;
}

bool BookmarkTreeModel::moveBookmark(
    const QString& sourceUuid,
    const QString& targetUuid,
    const QString& placement)
{
    if (m_loading || m_persistencePending || m_moveSaveGeneration != 0) {
        return false;
    }
    Node* source = findNode(QUuid(sourceUuid));
    Node* target = targetUuid.isEmpty() ? m_root.get()
                                        : findNode(QUuid(targetUuid));
    if (!source || source->group || !source->parent || !target) {
        return false;
    }

    Node* destinationParent = nullptr;
    std::size_t destinationIndex = 0;
    if (placement == QLatin1String("into")) {
        if (!target->group) {
            return false;
        }
        destinationParent = target;
        destinationIndex = target->children.size();
    } else if (placement == QLatin1String("before") ||
               placement == QLatin1String("after")) {
        if (target->group || !target->parent || target == source) {
            return false;
        }
        destinationParent = target->parent;
        const auto targetIt = std::find_if(
            destinationParent->children.begin(),
            destinationParent->children.end(),
            [target](const std::unique_ptr<Node>& child) {
                return child.get() == target;
            });
        if (targetIt == destinationParent->children.end()) {
            return false;
        }
        destinationIndex = static_cast<std::size_t>(
            std::distance(destinationParent->children.begin(), targetIt));
        if (placement == QLatin1String("after")) {
            ++destinationIndex;
        }
    } else {
        return false;
    }
    if (nodeDepth(*destinationParent) + 1 >
        platform::bookmarkLimits::maximumTreeDepth) {
        setLastError(QStringLiteral("Bookmarks tree exceeds the maximum nesting depth"));
        return false;
    }

    const auto sourceIt = std::find_if(
        source->parent->children.begin(),
        source->parent->children.end(),
        [source](const std::unique_ptr<Node>& child) {
            return child.get() == source;
        });
    if (sourceIt == source->parent->children.end()) {
        return false;
    }
    const std::size_t sourceSiblingIndex = static_cast<std::size_t>(
        std::distance(source->parent->children.begin(), sourceIt));
    if (source->parent == destinationParent &&
        (destinationIndex == sourceSiblingIndex ||
         destinationIndex == sourceSiblingIndex + 1)) {
        return false;
    }

    if (destinationParent != m_root.get() && !destinationParent->expanded) {
        destinationParent->expanded = true;
        beginResetModel();
        rebuildVisibleNodes();
        endResetModel();
    }
    const int sourceRow = visibleRowForUuid(sourceUuid);
    int destinationRow = rowCount();
    if (destinationParent != m_root.get()) {
        destinationRow = visibleSubtreeEnd(*destinationParent);
    }
    if (destinationIndex < destinationParent->children.size()) {
        destinationRow = visibleRowForUuid(
            destinationParent->children[destinationIndex]
                ->uuid.toString(QUuid::WithoutBraces));
    }
    if (sourceRow < 0 || destinationRow < 0) {
        return false;
    }

    m_moveRollbackDocument = serializedDocument();
    const bool changesVisibleOrder =
        destinationRow != sourceRow && destinationRow != sourceRow + 1;
    if (changesVisibleOrder) {
        beginMoveRows({}, sourceRow, sourceRow, {}, destinationRow);
    }
    std::unique_ptr<Node> moved = std::move(*sourceIt);
    source->parent->children.erase(sourceIt);
    if (source->parent == destinationParent &&
        sourceSiblingIndex < destinationIndex) {
        --destinationIndex;
    }
    moved->parent = destinationParent;
    destinationParent->children.insert(
        destinationParent->children.begin() +
            static_cast<std::ptrdiff_t>(destinationIndex),
        std::move(moved));
    rebuildVisibleNodes();
    if (changesVisibleOrder) {
        endMoveRows();
    } else {
        emit dataChanged(
            index(sourceRow, 0), index(sourceRow, 0), {DepthRole});
    }
    if (!saveNow()) {
        restoreDocument(m_moveRollbackDocument);
        m_moveRollbackDocument = {};
        return false;
    }
    m_moveSaveGeneration = m_saveGeneration;
    return true;
}

int BookmarkTreeModel::visibleRowForUuid(const QString& uuid) const
{
    const QUuid requested(uuid);
    if (requested.isNull()) {
        return -1;
    }
    for (int row = 0; row < m_visibleNodes.size(); ++row) {
        if (m_visibleNodes.at(row)->uuid == requested) {
            return row;
        }
    }
    return -1;
}

bool BookmarkTreeModel::reload()
{
    if (!m_persistenceContext) {
        return false;
    }
    if (!m_loading) {
        m_loading = true;
        emit loadingChanged();
    }
    const quint64 requestedRevision = m_revision;
    const auto store = m_store;
    QPointer<BookmarkTreeModel> model(this);
    QMetaObject::invokeMethod(
        m_persistenceContext,
        [store, model, requestedRevision] {
            auto result = store->load();
            if (!model) {
                return;
            }
            QMetaObject::invokeMethod(
                model,
                [model, result = std::move(result), requestedRevision]() mutable {
                    if (model) {
                        model->applyLoadResult(
                            std::move(result), requestedRevision);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}

void BookmarkTreeModel::applyLoadResult(
    sdr::platform::BookmarkJsonStore::LoadResult fileResult,
    quint64 requestedRevision)
{
    if (m_loading) {
        m_loading = false;
        emit loadingChanged();
    }
    if (requestedRevision != m_revision) {
        return;
    }
    if (fileResult.status ==
        sdr::platform::BookmarkJsonStore::LoadStatus::Missing) {
        beginResetModel();
        resetToEmpty();
        endResetModel();
        setLastError({});
        return;
    }
    if (fileResult.status ==
        sdr::platform::BookmarkJsonStore::LoadStatus::Error) {
        setLastError(fileResult.error);
        return;
    }
    if (!fileResult.document.isObject()) {
        setLastError(QStringLiteral("Bookmarks file is malformed"));
        return;
    }
    const QJsonObject documentObject = fileResult.document.object();
    qint64 version = 0;
    if (!strictInteger(documentObject.value(QStringLiteral("version")), version) ||
        version != bookmarkFileVersion ||
        !documentObject.value(QStringLiteral("root")).isObject()) {
        setLastError(QStringLiteral("Bookmarks file has an unsupported or invalid version"));
        return;
    }
    QJsonObject documentExtensions = documentObject;
    documentExtensions.remove(QStringLiteral("version"));
    documentExtensions.remove(QStringLiteral("root"));
    if (!fitsCompactJsonPayload(documentExtensions)) {
        setLastError(QStringLiteral("Bookmark document extensions exceed the 64 KiB limit"));
        return;
    }
    QSet<QUuid> seenUuids;
    std::size_t nodeCount = 0;
    QString error;
    std::unique_ptr<Node> loadedRoot = parseNode(
        documentObject.value(QStringLiteral("root")).toObject(),
        nullptr,
        seenUuids,
        nodeCount,
        0,
        error);
    if (!loadedRoot || !loadedRoot->group) {
        setLastError(error.isEmpty()
                         ? QStringLiteral("Bookmarks root must be a group")
                         : error);
        return;
    }
    beginResetModel();
    m_documentExtensions = std::move(documentExtensions);
    m_root = std::move(loadedRoot);
    rebuildVisibleNodes();
    endResetModel();
    setLastError({});
}

bool BookmarkTreeModel::saveNow()
{
    if (!m_persistenceContext) {
        return false;
    }
    QJsonObject documentObject = m_documentExtensions;
    documentObject.insert(QStringLiteral("version"), bookmarkFileVersion);
    documentObject.insert(QStringLiteral("root"), serializeNode(*m_root));
    const QJsonDocument document(documentObject);
    if (document.toJson(QJsonDocument::Indented).size() >
        platform::bookmarkLimits::maximumFileBytes) {
        setLastError(QStringLiteral("Bookmarks file exceeds the 4 MiB size limit"));
        return false;
    }
    QSet<QUuid> seenUuids;
    std::size_t parsedNodeCount = 0;
    QString validationError;
    const auto validatedRoot = parseNode(
        documentObject.value(QStringLiteral("root")).toObject(),
        nullptr,
        seenUuids,
        parsedNodeCount,
        0,
        validationError);
    if (!validatedRoot || !validatedRoot->group ||
        !fitsCompactJsonPayload(m_documentExtensions)) {
        setLastError(validationError.isEmpty()
                         ? QStringLiteral("Bookmark document extensions exceed the 64 KiB limit")
                         : std::move(validationError));
        return false;
    }
    ++m_revision;
    const quint64 generation = ++m_saveGeneration;
    if (!m_persistencePending) {
        m_persistencePending = true;
        emit persistencePendingChanged();
    }
    const auto store = m_store;
    QPointer<BookmarkTreeModel> model(this);
    QMetaObject::invokeMethod(
        m_persistenceContext,
        [store, model, document, generation] {
            QString error;
            const bool succeeded = store->save(document, error);
            if (!model) {
                return;
            }
            QMetaObject::invokeMethod(
                model,
                [model,
                 succeeded,
                 error = std::move(error),
                 generation]() mutable {
                    if (model) {
                        model->applySaveResult(
                            succeeded, std::move(error), generation);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}

void BookmarkTreeModel::applySaveResult(
    bool succeeded, QString error, quint64 generation)
{
    if (generation != m_saveGeneration) {
        return;
    }
    if (m_persistencePending) {
        m_persistencePending = false;
        emit persistencePendingChanged();
    }
    if (generation == m_moveSaveGeneration) {
        if (!succeeded) {
            restoreDocument(m_moveRollbackDocument);
        }
        m_moveRollbackDocument = {};
        m_moveSaveGeneration = 0;
    }
    setLastError(succeeded ? QString{} : std::move(error));
}

BookmarkTreeModel::Node* BookmarkTreeModel::findNode(const QUuid& uuid) const
{
    if (uuid.isNull()) {
        return nullptr;
    }
    const auto find = [&uuid](const auto& self, Node& node) -> Node* {
        if (node.uuid == uuid) {
            return &node;
        }
        for (const auto& child : node.children) {
            if (Node* match = self(self, *child)) {
                return match;
            }
        }
        return nullptr;
    };
    return find(find, *m_root);
}

std::size_t BookmarkTreeModel::nodeCount() const
{
    const auto count = [](const auto& self, const Node& node) -> std::size_t {
        std::size_t result = 1;
        for (const auto& child : node.children) {
            result += self(self, *child);
        }
        return result;
    };
    return count(count, *m_root);
}

int BookmarkTreeModel::nodeDepth(const Node& node)
{
    int depth = 0;
    for (const Node* parent = node.parent; parent; parent = parent->parent) {
        ++depth;
    }
    return depth;
}

int BookmarkTreeModel::visibleSubtreeEnd(const Node& node) const
{
    const int row = visibleRowForUuid(node.uuid.toString(QUuid::WithoutBraces));
    if (row < 0) {
        return -1;
    }
    const int depth = m_visibleDepths.at(row);
    int end = row + 1;
    while (end < m_visibleDepths.size() && m_visibleDepths.at(end) > depth) {
        ++end;
    }
    return end;
}

QJsonDocument BookmarkTreeModel::serializedDocument() const
{
    QJsonObject object = m_documentExtensions;
    object.insert(QStringLiteral("version"), bookmarkFileVersion);
    object.insert(QStringLiteral("root"), serializeNode(*m_root));
    return QJsonDocument(object);
}

void BookmarkTreeModel::restoreDocument(const QJsonDocument& document)
{
    if (!document.isObject()) {
        return;
    }
    const QJsonObject object = document.object();
    QSet<QUuid> uuids;
    std::size_t nodeCount = 0;
    QString error;
    auto root = parseNode(
        object.value(QStringLiteral("root")).toObject(),
        nullptr,
        uuids,
        nodeCount,
        0,
        error);
    if (!root) {
        return;
    }
    beginResetModel();
    m_documentExtensions = object;
    m_documentExtensions.remove(QStringLiteral("version"));
    m_documentExtensions.remove(QStringLiteral("root"));
    m_root = std::move(root);
    rebuildVisibleNodes();
    endResetModel();
}

BookmarkTreeModel::Node* BookmarkTreeModel::insertionParent(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= m_visibleNodes.size()) {
        return m_root.get();
    }
    Node* selected = m_visibleNodes.at(visibleRow);
    return selected->group ? selected : m_root.get();
}

Qt::CheckState BookmarkTreeModel::scannerCheckState(const Node& node) const
{
    if (!node.group) {
        return node.bookmark.scannerIncluded ? Qt::Checked : Qt::Unchecked;
    }
    std::size_t bookmarkCount = 0;
    std::size_t includedCount = 0;
    const auto count = [&bookmarkCount, &includedCount](
                           const auto& self, const Node& current) -> void {
        if (!current.group) {
            ++bookmarkCount;
            includedCount += current.bookmark.scannerIncluded ? 1U : 0U;
            return;
        }
        for (const auto& child : current.children) {
            self(self, *child);
        }
    };
    count(count, node);
    if (bookmarkCount == 0 || includedCount == 0) {
        return Qt::Unchecked;
    }
    return includedCount == bookmarkCount ? Qt::Checked
                                          : Qt::PartiallyChecked;
}

void BookmarkTreeModel::setScannerIncludedRecursively(
    Node& node, bool included)
{
    if (!node.group) {
        node.bookmark.scannerIncluded = included;
        return;
    }
    for (const auto& child : node.children) {
        setScannerIncludedRecursively(*child, included);
    }
}

void BookmarkTreeModel::rebuildVisibleNodes()
{
    m_visibleNodes.clear();
    m_visibleDepths.clear();
    appendVisibleChildren(*m_root, 0);
}

void BookmarkTreeModel::appendVisibleChildren(Node& parent, int depth)
{
    for (const auto& child : parent.children) {
        m_visibleNodes.push_back(child.get());
        m_visibleDepths.push_back(depth);
        if (child->group && child->expanded) {
            appendVisibleChildren(*child, depth + 1);
        }
    }
}

void BookmarkTreeModel::appendScannerBookmarks(
    const Node& parent,
    std::vector<BookmarkSnapshot>& bookmarks) const
{
    for (const auto& child : parent.children) {
        if (child->group) {
            appendScannerBookmarks(*child, bookmarks);
        } else if (child->bookmark.scannerIncluded) {
            bookmarks.push_back({
                child->uuid.toString(QUuid::WithoutBraces),
                child->bookmark,
            });
        }
    }
}

void BookmarkTreeModel::resetToEmpty()
{
    m_documentExtensions = {};
    m_root = std::make_unique<Node>();
    m_root->groupName = QStringLiteral("Bookmarks");
    m_root->expanded = true;
    rebuildVisibleNodes();
}

void BookmarkTreeModel::setLastError(QString error)
{
    if (m_lastError == error) {
        return;
    }
    m_lastError = std::move(error);
    emit lastErrorChanged();
}

QJsonObject BookmarkTreeModel::serializeNode(const Node& node) const
{
    QJsonObject object = node.extensions;
    object.insert(
        QStringLiteral("uuid"), node.uuid.toString(QUuid::WithoutBraces));
    object.insert(
        QStringLiteral("type"),
        node.group ? QStringLiteral("group") : QStringLiteral("bookmark"));
    if (node.group) {
        object.insert(QStringLiteral("name"), node.groupName);
        object.insert(QStringLiteral("expanded"), node.expanded);
        QJsonArray children;
        for (const auto& child : node.children) {
            children.append(serializeNode(*child));
        }
        object.insert(QStringLiteral("children"), children);
        return object;
    }
    const BookmarkData& bookmark = node.bookmark;
    object.insert(QStringLiteral("name"), bookmark.name);
    object.insert(
        QStringLiteral("listeningFrequency"),
        static_cast<double>(bookmark.listeningFrequency));
    object.insert(QStringLiteral("requestedGainDb"), bookmark.requestedGainDb);
    object.insert(QStringLiteral("demodulatorId"), bookmark.demodulatorId);
    object.insert(
        QStringLiteral("filterLowHz"),
        static_cast<double>(bookmark.filterLowHz));
    object.insert(
        QStringLiteral("filterHighHz"),
        static_cast<double>(bookmark.filterHighHz));
    object.insert(
        QStringLiteral("squelchThresholdDb"), bookmark.squelchThresholdDb);
    object.insert(QStringLiteral("squelchEnabled"), bookmark.squelchEnabled);
    object.insert(
        QStringLiteral("modeSpecificSettings"),
        bookmark.modeSpecificSettings);
    object.insert(QStringLiteral("scannerIncluded"), bookmark.scannerIncluded);
    return object;
}

std::unique_ptr<BookmarkTreeModel::Node> BookmarkTreeModel::parseNode(
    const QJsonObject& object,
    Node* parent,
    QSet<QUuid>& seenUuids,
    std::size_t& nodeCount,
    int depth,
    QString& error) const
{
    if (depth > platform::bookmarkLimits::maximumTreeDepth) {
        error = QStringLiteral("Bookmarks tree exceeds the maximum nesting depth");
        return {};
    }
    if (nodeCount >= platform::bookmarkLimits::maximumNodeCount) {
        error = QStringLiteral("Bookmarks tree exceeds the 4,096-node limit");
        return {};
    }
    const QUuid uuid(object.value(QStringLiteral("uuid")).toString());
    if (uuid.isNull() || seenUuids.contains(uuid)) {
        error = QStringLiteral("Bookmark item has a missing or duplicate UUID");
        return {};
    }
    seenUuids.insert(uuid);
    ++nodeCount;
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("group") &&
        type != QLatin1String("bookmark")) {
        error = QStringLiteral("Bookmark item has an invalid type");
        return {};
    }
    auto node = std::make_unique<Node>();
    node->extensions = object;
    for (const auto& key : {"uuid", "type", "name", "expanded", "children",
             "listeningFrequency", "requestedGainDb", "demodulatorId", "filterLowHz",
             "filterHighHz", "squelchThresholdDb", "squelchEnabled",
             "modeSpecificSettings", "scannerIncluded"}) {
        node->extensions.remove(QString::fromLatin1(key));
    }
    if (!fitsCompactJsonPayload(node->extensions)) {
        error = QStringLiteral("Bookmark item extensions exceed the 64 KiB limit");
        return {};
    }
    node->uuid = uuid;
    node->group = type == QLatin1String("group");
    node->parent = parent;
    if (node->group) {
        if (!object.value(QStringLiteral("name")).isString() ||
            !object.value(QStringLiteral("expanded")).isBool() ||
            !object.value(QStringLiteral("children")).isArray()) {
            error = QStringLiteral("Bookmark group is missing required fields");
            return {};
        }
        const QString inputName = object.value(QStringLiteral("name")).toString();
        node->groupName = normalizedName(
            inputName,
            QStringLiteral("Unnamed group"));
        if (!fitsUtf8Bytes(inputName, platform::bookmarkLimits::maximumNameUtf8Bytes) ||
            !fitsUtf8Bytes(
                node->groupName,
                platform::bookmarkLimits::maximumNameUtf8Bytes)) {
            error = QStringLiteral("Bookmark group name exceeds the 256-byte limit");
            return {};
        }
        node->expanded = object.value(QStringLiteral("expanded")).toBool();
        for (const QJsonValue& childValue :
             object.value(QStringLiteral("children")).toArray()) {
            if (!childValue.isObject()) {
                error = QStringLiteral("Bookmark group contains an invalid child");
                return {};
            }
            auto child = parseNode(
                childValue.toObject(),
                node.get(),
                seenUuids,
                nodeCount,
                depth + 1,
                error);
            if (!child) {
                return {};
            }
            node->children.push_back(std::move(child));
        }
        return node;
    }

    BookmarkData bookmark;
    qint64 filterLowHz = 0;
    qint64 filterHighHz = 0;
    quint64 listeningFrequency = 0;
    const QJsonValue requestedGain =
        object.value(QStringLiteral("requestedGainDb"));
    const QJsonValue squelchThreshold =
        object.value(QStringLiteral("squelchThresholdDb"));
    const QJsonValue squelchEnabled =
        object.value(QStringLiteral("squelchEnabled"));
    const bool hasSquelchThreshold = object.contains(
        QStringLiteral("squelchThresholdDb"));
    const bool hasSquelchEnabled = object.contains(
        QStringLiteral("squelchEnabled"));
    if (!object.value(QStringLiteral("name")).isString() ||
        !strictUnsignedInteger(
            object.value(QStringLiteral("listeningFrequency")),
            listeningFrequency) ||
        !requestedGain.isDouble() ||
        !std::isfinite(requestedGain.toDouble()) ||
        !object.value(QStringLiteral("demodulatorId")).isString() ||
        object.value(QStringLiteral("demodulatorId")).toString().trimmed().isEmpty() ||
        !strictInteger(
            object.value(QStringLiteral("filterLowHz")), filterLowHz) ||
        !strictInteger(
            object.value(QStringLiteral("filterHighHz")), filterHighHz) ||
        filterLowHz > filterHighHz ||
        hasSquelchThreshold != hasSquelchEnabled ||
        (hasSquelchThreshold &&
         (!squelchThreshold.isDouble() ||
          !std::isfinite(squelchThreshold.toDouble()) ||
          !squelchEnabled.isBool())) ||
        !object.value(QStringLiteral("modeSpecificSettings")).isObject() ||
        !object.value(QStringLiteral("scannerIncluded")).isBool()) {
        error = QStringLiteral("Bookmark is missing required or valid fields");
        return {};
    }
    const QString inputName = object.value(QStringLiteral("name")).toString();
    const QString inputDemodulatorId =
        object.value(QStringLiteral("demodulatorId")).toString();
    bookmark.name = normalizedName(
        inputName,
        QStringLiteral("Unnamed bookmark"));
    bookmark.listeningFrequency = listeningFrequency;
    bookmark.requestedGainDb = requestedGain.toDouble();
    bookmark.demodulatorId =
        inputDemodulatorId.trimmed();
    if (!fitsUtf8Bytes(inputName, platform::bookmarkLimits::maximumNameUtf8Bytes)) {
        error = QStringLiteral("Bookmark name exceeds the 256-byte limit");
        return {};
    }
    if (!fitsUtf8Bytes(
            inputDemodulatorId,
            platform::bookmarkLimits::maximumDemodulatorIdUtf8Bytes)) {
        error = QStringLiteral("Bookmark demodulator ID exceeds the 128-byte limit");
        return {};
    }
    if (!validBookmarkPayload(
            bookmark.name,
            bookmark.demodulatorId,
            object.value(QStringLiteral("modeSpecificSettings")).toObject(),
            error)) {
        return {};
    }
    bookmark.filterLowHz = filterLowHz;
    bookmark.filterHighHz = filterHighHz;
    bookmark.squelchThresholdDb = hasSquelchThreshold
        ? squelchThreshold.toDouble()
        : bookmark.squelchThresholdDb;
    bookmark.squelchEnabled = hasSquelchEnabled
        ? squelchEnabled.toBool()
        : bookmark.squelchEnabled;
    bookmark.hasSavedSquelch = hasSquelchThreshold;
    bookmark.modeSpecificSettings =
        object.value(QStringLiteral("modeSpecificSettings")).toObject();
    bookmark.scannerIncluded =
        object.value(QStringLiteral("scannerIncluded")).toBool();
    node->bookmark = std::move(bookmark);
    return node;
}

}  // namespace sdr::app
