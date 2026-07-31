// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "BookmarkTreeModel.hpp"
#include "BookmarkLimits.hpp"
#include "DemodulatorRegistry.hpp"

#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <array>
#include <functional>
#include <utility>

namespace {

using sdr::app::BookmarkData;
using sdr::app::BookmarkTreeModel;

BookmarkData bookmark(
    QString name,
    QString demodulatorId,
    bool scannerIncluded = false)
{
    BookmarkData data;
    data.name = std::move(name);
    data.listeningFrequency = 145'500'000;
    data.requestedGainDb = 21.5;
    data.demodulatorId = std::move(demodulatorId);
    data.filterLowHz = -6'250;
    data.filterHighHz = 6'250;
    data.squelchThresholdDb = -72.0;
    data.squelchEnabled = true;
    data.modeSpecificSettings = {
        {QStringLiteral("version"), 3},
        {QStringLiteral("futureOption"), QStringLiteral("preserve-me")},
        {QStringLiteral("nested"), QJsonObject{{QStringLiteral("value"), 17}}},
    };
    data.scannerIncluded = scannerIncluded;
    return data;
}

QJsonObject findSerializedItem(const QJsonObject& node, const QString& uuid)
{
    if (node.value(QStringLiteral("uuid")).toString() == uuid) {
        return node;
    }
    for (const QJsonValue& child :
         node.value(QStringLiteral("children")).toArray()) {
        const QJsonObject found = findSerializedItem(child.toObject(), uuid);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

int checkState(const BookmarkTreeModel& model, int row)
{
    return model.data(
                    model.index(row, 0),
                    BookmarkTreeModel::ScannerCheckStateRole)
        .toInt();
}

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds = 1'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

QJsonObject serializedGroup(const QJsonArray& children = {})
{
    return {
        {QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("type"), QStringLiteral("group")},
        {QStringLiteral("name"), QStringLiteral("Group")},
        {QStringLiteral("expanded"), true},
        {QStringLiteral("children"), children},
    };
}

QJsonObject serializedBookmark(const QUuid& uuid = QUuid::createUuid())
{
    return {
        {QStringLiteral("uuid"), uuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("type"), QStringLiteral("bookmark")},
        {QStringLiteral("name"), QStringLiteral("Bookmark")},
        {QStringLiteral("listeningFrequency"), 145'500'000},
        {QStringLiteral("requestedGainDb"), 21.5},
        {QStringLiteral("demodulatorId"), QStringLiteral("am")},
        {QStringLiteral("filterLowHz"), -6'250},
        {QStringLiteral("filterHighHz"), 6'250},
        {QStringLiteral("squelchThresholdDb"), -72.0},
        {QStringLiteral("squelchEnabled"), true},
        {QStringLiteral("modeSpecificSettings"), QJsonObject{{QStringLiteral("version"), 1}}},
        {QStringLiteral("scannerIncluded"), false},
    };
}

QJsonDocument bookmarkDocument(const QJsonObject& root)
{
    return QJsonDocument({
        {QStringLiteral("version"), 1},
        {QStringLiteral("root"), root},
    });
}

void writeDocument(const QString& path, const QJsonDocument& document)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(document.toJson(QJsonDocument::Compact)),
             document.toJson(QJsonDocument::Compact).size());
}

QJsonObject payloadAtLimit(const QString& key, QJsonObject base = {})
{
    qsizetype low = 0;
    qsizetype high = sdr::platform::bookmarkLimits::maximumJsonPayloadBytes;
    QJsonObject result = base;
    while (low <= high) {
        const qsizetype middle = low + (high - low) / 2;
        QJsonObject candidate = base;
        candidate.insert(key, QString(middle, QLatin1Char('x')));
        if (QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() <=
            sdr::platform::bookmarkLimits::maximumJsonPayloadBytes) {
            result = std::move(candidate);
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    return result;
}

}  // namespace

class BookmarkTreeModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void resolvesStableDemodulatorIdsWithoutOrdinals();
    void persistsNestedHierarchyExpansionAndUnknownModes();
    void derivesAndUpdatesTriStateScannerInclusion();
    void snapshotsCheckedBookmarksInSavedOrder();
    void editsItemsPreservesUuidsAndRemovesDescendants();
    void preservesUnknownJsonFieldsWhenEditing();
    void loadsLegacyBookmarksWithoutSquelchFields();
    void recoversFromMissingMalformedAndUnsupportedFiles();
    void reordersAndMovesBookmarksWithoutChangingIdentityOrData();
    void rejectsInvalidBookmarkMovesWithoutSaving();
    void rollsBackBookmarkMoveWhenAtomicSaveFails();
    void boundsFileAndRejectedLoadsPreserveModel();
    void boundsParsedTreeAndDetectsLateDuplicate();
    void boundsStringsAndPayloadMutations();
    void preservesBoundedExtensionsAcrossRoundTrip();
};

void BookmarkTreeModelTest::editsItemsPreservesUuidsAndRemovesDescendants()
{
    QTemporaryDir directory;
    BookmarkTreeModel model(directory.filePath(QStringLiteral("bookmarks.json")));
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    const QString groupUuid = model.addGroup(-1, QStringLiteral("Old"));
    const int groupRow = model.visibleRowForUuid(groupUuid);
    const QString childUuid = model.addBookmark(groupRow,
        bookmark(QStringLiteral("Child"), QStringLiteral("usb"), true));
    QVERIFY(model.renameGroup(groupRow, QStringLiteral("Renamed")));
    QCOMPARE(model.itemDetails(groupRow).value(QStringLiteral("uuid")).toString(), groupUuid);
    const int childRow = model.visibleRowForUuid(childUuid);
    QVariantMap fields = model.itemDetails(childRow);
    fields.insert(QStringLiteral("name"), QStringLiteral("Edited"));
    fields.insert(QStringLiteral("listeningFrequency"), qulonglong{7'100'000});
    fields.insert(QStringLiteral("requestedGain"), 33.0);
    fields.insert(QStringLiteral("filterLowHz"), qlonglong{0});
    fields.insert(QStringLiteral("filterHighHz"), qlonglong{2'700});
    fields.insert(QStringLiteral("squelchThreshold"), -61.0);
    fields.insert(QStringLiteral("squelchEnabled"), false);
    QVERIFY(model.updateBookmark(childRow, fields));
    QCOMPARE(model.itemDetails(childRow).value(QStringLiteral("uuid")).toString(), childUuid);
    QCOMPARE(model.bookmarkAt(childRow)->squelchThresholdDb, -61.0);
    const QString rootBookmark = model.addBookmark(childRow,
        bookmark(QStringLiteral("Root sibling"), QStringLiteral("am")));
    QCOMPARE(model.data(model.index(model.visibleRowForUuid(rootBookmark), 0),
                 BookmarkTreeModel::DepthRole).toInt(), 0);
    QVERIFY(model.removeItem(groupRow));
    QCOMPARE(model.visibleRowForUuid(childUuid), -1);
    QVERIFY(model.visibleRowForUuid(rootBookmark) >= 0);
}

void BookmarkTreeModelTest::reordersAndMovesBookmarksWithoutChangingIdentityOrData()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    QString firstUuid;
    QString secondUuid;
    QString groupUuid;
    QString nestedUuid;
    QString nestedBookmarkUuid;
    BookmarkData preserved = bookmark(
        QStringLiteral("Preserve all fields"), QStringLiteral("future-mode"), true);

    {
        BookmarkTreeModel model(path);
        QVERIFY(waitUntil([&model] { return !model.loading(); }));
        firstUuid = model.addBookmark(-1, preserved);
        secondUuid = model.addBookmark(
            -1, bookmark(QStringLiteral("Second"), QStringLiteral("am")));
        groupUuid = model.addGroup(-1, QStringLiteral("Top"));
        nestedUuid = model.addGroup(
            model.visibleRowForUuid(groupUuid), QStringLiteral("Nested"));
        nestedBookmarkUuid = model.addBookmark(
            model.visibleRowForUuid(nestedUuid),
            bookmark(QStringLiteral("Nested bookmark"), QStringLiteral("nfm")));
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));

        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QVERIFY(model.moveBookmark(secondUuid, firstUuid, QStringLiteral("before")));
        QCOMPARE(model.visibleRowForUuid(secondUuid), 0);
        QCOMPARE(model.visibleRowForUuid(firstUuid), 1);
        QCOMPARE(moved.count(), 1);
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));

        QVERIFY(model.moveBookmark(secondUuid, firstUuid, QStringLiteral("after")));
        QCOMPARE(model.visibleRowForUuid(firstUuid), 0);
        QCOMPARE(model.visibleRowForUuid(secondUuid), 1);
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));

        QVERIFY(model.moveBookmark(firstUuid, groupUuid, QStringLiteral("into")));
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
        QCOMPARE(
            model.data(model.index(model.visibleRowForUuid(firstUuid), 0),
                       BookmarkTreeModel::DepthRole).toInt(),
            1);

        QVERIFY(model.moveBookmark(firstUuid, nestedUuid, QStringLiteral("into")));
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
        const int movedRow = model.visibleRowForUuid(firstUuid);
        QCOMPARE(model.data(model.index(movedRow, 0),
                            BookmarkTreeModel::DepthRole).toInt(), 2);
        const auto afterMove = model.bookmarkAt(movedRow);
        QVERIFY(afterMove.has_value());
        QCOMPARE(afterMove->name, preserved.name);
        QCOMPARE(afterMove->listeningFrequency, preserved.listeningFrequency);
        QCOMPARE(afterMove->requestedGainDb, preserved.requestedGainDb);
        QCOMPARE(afterMove->demodulatorId, preserved.demodulatorId);
        QCOMPARE(afterMove->filterLowHz, preserved.filterLowHz);
        QCOMPARE(afterMove->filterHighHz, preserved.filterHighHz);
        QCOMPARE(afterMove->squelchThresholdDb, preserved.squelchThresholdDb);
        QCOMPARE(afterMove->squelchEnabled, preserved.squelchEnabled);
        QCOMPARE(afterMove->scannerIncluded, preserved.scannerIncluded);
        QCOMPARE(afterMove->modeSpecificSettings, preserved.modeSpecificSettings);

        QVERIFY(model.moveBookmark(nestedBookmarkUuid, {}, QStringLiteral("into")));
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
        QCOMPARE(model.data(
                     model.index(model.visibleRowForUuid(nestedBookmarkUuid), 0),
                     BookmarkTreeModel::DepthRole).toInt(), 0);
    }

    BookmarkTreeModel restored(path);
    QVERIFY(waitUntil([&restored] { return !restored.loading(); }));
    QCOMPARE(restored.visibleRowForUuid(secondUuid), 0);
    const int restoredRootBookmarkRow =
        restored.visibleRowForUuid(nestedBookmarkUuid);
    QVERIFY(restoredRootBookmarkRow > 0);
    QCOMPARE(restored.data(restored.index(restoredRootBookmarkRow, 0),
                           BookmarkTreeModel::DepthRole).toInt(), 0);
    const int restoredMovedRow = restored.visibleRowForUuid(firstUuid);
    QVERIFY(restoredMovedRow >= 0);
    QCOMPARE(restored.data(restored.index(restoredMovedRow, 0),
                           BookmarkTreeModel::DepthRole).toInt(), 2);
    QCOMPARE(restored.itemDetails(restoredMovedRow)
                 .value(QStringLiteral("uuid")).toString(), firstUuid);
}

void BookmarkTreeModelTest::rejectsInvalidBookmarkMovesWithoutSaving()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    BookmarkTreeModel model(path);
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    const QString groupUuid = model.addGroup(-1, QStringLiteral("Group"));
    const QString bookmarkUuid = model.addBookmark(
        -1, bookmark(QStringLiteral("Bookmark"), QStringLiteral("am")));
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();

    QVERIFY(!model.moveBookmark(groupUuid, bookmarkUuid, QStringLiteral("before")));
    QVERIFY(!model.moveBookmark(bookmarkUuid, groupUuid, QStringLiteral("before")));
    QVERIFY(!model.moveBookmark(bookmarkUuid, QStringLiteral("missing"), QStringLiteral("into")));
    QVERIFY(!model.moveBookmark(bookmarkUuid, bookmarkUuid, QStringLiteral("after")));
    QVERIFY(!model.persistencePending());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
}

void BookmarkTreeModelTest::rollsBackBookmarkMoveWhenAtomicSaveFails()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    BookmarkTreeModel model(path);
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    const QString firstUuid = model.addBookmark(
        -1, bookmark(QStringLiteral("First"), QStringLiteral("am")));
    const QString secondUuid = model.addBookmark(
        -1, bookmark(QStringLiteral("Second"), QStringLiteral("nfm")));
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    QCOMPARE(model.visibleRowForUuid(firstUuid), 0);
    QCOMPARE(model.visibleRowForUuid(secondUuid), 1);

    const QFileDevice::Permissions originalPermissions =
        QFileInfo(directory.path()).permissions();
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QVERIFY(model.moveBookmark(secondUuid, firstUuid, QStringLiteral("before")));
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    QVERIFY(!model.lastError().isEmpty());
    QCOMPARE(model.visibleRowForUuid(firstUuid), 0);
    QCOMPARE(model.visibleRowForUuid(secondUuid), 1);
    QVERIFY(QFile::setPermissions(directory.path(), originalPermissions));
}

void BookmarkTreeModelTest::preservesUnknownJsonFieldsWhenEditing()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    QString uuid;
    {
        BookmarkTreeModel seed(path);
        QVERIFY(waitUntil([&seed] { return !seed.loading(); }));
        uuid = seed.addBookmark(-1,
            bookmark(QStringLiteral("Known"), QStringLiteral("am")));
        QVERIFY(waitUntil([&seed] { return !seed.persistencePending(); }));
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    document.insert(QStringLiteral("futureDocumentField"), QJsonArray{1, 2, 3});
    QJsonObject root = document.value(QStringLiteral("root")).toObject();
    QJsonArray children = root.value(QStringLiteral("children")).toArray();
    QJsonObject item = children.at(0).toObject();
    item.insert(QStringLiteral("futureItemField"),
        QJsonObject{{QStringLiteral("x"), true}});
    children.replace(0, item);
    root.insert(QStringLiteral("children"), children);
    document.insert(QStringLiteral("root"), root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(document).toJson());
    file.close();
    BookmarkTreeModel restored(path);
    QVERIFY(waitUntil([&restored] { return !restored.loading(); }));
    QVariantMap fields = restored.itemDetails(restored.visibleRowForUuid(uuid));
    fields.insert(QStringLiteral("name"), QStringLiteral("Changed"));
    QVERIFY(restored.updateBookmark(restored.visibleRowForUuid(uuid), fields));
    QVERIFY(waitUntil([&restored] { return !restored.persistencePending(); }));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(saved.value(QStringLiteral("futureDocumentField")),
        QJsonValue(QJsonArray{1, 2, 3}));
    const QJsonObject savedItem = findSerializedItem(
        saved.value(QStringLiteral("root")).toObject(), uuid);
    QCOMPARE(savedItem.value(QStringLiteral("futureItemField")),
        QJsonValue(QJsonObject{{QStringLiteral("x"), true}}));
}

void BookmarkTreeModelTest::resolvesStableDemodulatorIdsWithoutOrdinals()
{
    using sdr::radio::DemodulationMode;
    using sdr::radio::DemodulatorRegistry;
    const std::array expected{
        std::pair{"am", DemodulationMode::Am},
        std::pair{"nfm", DemodulationMode::Nfm},
        std::pair{"wfm", DemodulationMode::Wfm},
        std::pair{"usb", DemodulationMode::Usb},
        std::pair{"lsb", DemodulationMode::Lsb},
        std::pair{"digital-auto", DemodulationMode::DigitalDecoderOutput},
    };
    for (const auto& [id, mode] : expected) {
        const auto resolved = DemodulatorRegistry::resolve(id);
        QVERIFY(resolved.has_value());
        QCOMPARE(*resolved, mode);
        const auto* descriptor = DemodulatorRegistry::findByMode(mode);
        QVERIFY(descriptor);
        QCOMPARE(descriptor->id, std::string_view{id});
    }
    QVERIFY(!DemodulatorRegistry::resolve("future-digital").has_value());
    QVERIFY(!DemodulatorRegistry::findById("AM"));
}

void BookmarkTreeModelTest::persistsNestedHierarchyExpansionAndUnknownModes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    QString groupUuid;
    QString subgroupUuid;
    QString bookmarkUuid;
    const QJsonObject unknownSettings = bookmark(
        QStringLiteral("Future mode"),
        QStringLiteral("future-digital")).modeSpecificSettings;

    {
        BookmarkTreeModel model(path);
        QVERIFY(waitUntil([&model] { return !model.loading(); }));
        QCOMPARE(model.rowCount(), 0);
        groupUuid = model.addGroup(-1, QStringLiteral("Local"));
        QVERIFY(!groupUuid.isEmpty());
        const int groupRow = model.visibleRowForUuid(groupUuid);
        QCOMPARE(groupRow, 0);
        subgroupUuid = model.addGroup(groupRow, QStringLiteral("Airband"));
        QVERIFY(!subgroupUuid.isEmpty());
        const int subgroupRow = model.visibleRowForUuid(subgroupUuid);
        QCOMPARE(subgroupRow, 1);
        bookmarkUuid = model.addBookmark(
            subgroupRow,
            bookmark(
                QStringLiteral("Future mode"),
                QStringLiteral("future-digital")));
        QVERIFY(!bookmarkUuid.isEmpty());
        QCOMPARE(model.rowCount(), 3);
        const int bookmarkRow = model.visibleRowForUuid(bookmarkUuid);
        QCOMPARE(
            model.data(
                    model.index(bookmarkRow, 0),
                    BookmarkTreeModel::DemodulatorIdRole)
                .toString(),
            QStringLiteral("future-digital"));
        QVERIFY(!model.data(
                          model.index(bookmarkRow, 0),
                          BookmarkTreeModel::DemodulatorAvailableRole)
                     .toBool());
        QVERIFY(model.setExpanded(subgroupRow, false));
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    }

    {
        BookmarkTreeModel restored(path);
        QVERIFY(waitUntil([&restored] { return !restored.loading(); }));
        QCOMPARE(restored.rowCount(), 2);
        QCOMPARE(restored.visibleRowForUuid(groupUuid), 0);
        QCOMPARE(restored.visibleRowForUuid(subgroupUuid), 1);
        QCOMPARE(restored.visibleRowForUuid(bookmarkUuid), -1);
        QVERIFY(restored.setExpanded(1, true));
        QCOMPARE(restored.rowCount(), 3);
        const int bookmarkRow = restored.visibleRowForUuid(bookmarkUuid);
        QVERIFY(bookmarkRow >= 0);
        QCOMPARE(
            restored.data(
                        restored.index(bookmarkRow, 0),
                        BookmarkTreeModel::ModeSpecificSettingsRole)
                .toMap()
                .value(QStringLiteral("futureOption"))
                .toString(),
            QStringLiteral("preserve-me"));
        QVERIFY(waitUntil(
            [&restored] { return !restored.persistencePending(); }));
    }

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(document.value(QStringLiteral("version")).toInt(), 1);
    const QJsonObject savedBookmark = findSerializedItem(
        document.value(QStringLiteral("root")).toObject(), bookmarkUuid);
    QCOMPARE(
        savedBookmark.value(QStringLiteral("demodulatorId")).toString(),
        QStringLiteral("future-digital"));
    QCOMPARE(
        savedBookmark.value(QStringLiteral("modeSpecificSettings")).toObject(),
        unknownSettings);
    QCOMPARE(
        savedBookmark.value(QStringLiteral("requestedGainDb")).toDouble(),
        21.5);
    QCOMPARE(
        savedBookmark.value(QStringLiteral("filterLowHz")).toInt(),
        -6'250);
    QCOMPARE(
        savedBookmark.value(QStringLiteral("filterHighHz")).toInt(),
        6'250);
    QCOMPARE(
        savedBookmark.value(QStringLiteral("squelchThresholdDb")).toDouble(),
        -72.0);
    QVERIFY(savedBookmark.value(QStringLiteral("squelchEnabled")).toBool());
}

void BookmarkTreeModelTest::loadsLegacyBookmarksWithoutSquelchFields()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    QString uuid;
    {
        BookmarkTreeModel seed(path);
        QVERIFY(waitUntil([&seed] { return !seed.loading(); }));
        uuid = seed.addBookmark(
            -1, bookmark(QStringLiteral("Legacy"), QStringLiteral("am")));
        QVERIFY(waitUntil([&seed] { return !seed.persistencePending(); }));
    }

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject root = document.value(QStringLiteral("root")).toObject();
    QJsonArray children = root.value(QStringLiteral("children")).toArray();
    QJsonObject item = children.at(0).toObject();
    item.remove(QStringLiteral("squelchThresholdDb"));
    item.remove(QStringLiteral("squelchEnabled"));
    children[0] = item;
    root.insert(QStringLiteral("children"), children);
    document.insert(QStringLiteral("root"), root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(document).toJson());
    file.close();

    BookmarkTreeModel restored(path);
    QVERIFY(waitUntil([&restored] { return !restored.loading(); }));
    const int row = restored.visibleRowForUuid(uuid);
    QVERIFY(row >= 0);
    const auto loaded = restored.bookmarkAt(row);
    QVERIFY(loaded.has_value());
    QVERIFY(!loaded->hasSavedSquelch);
    QCOMPARE(loaded->squelchThresholdDb, -80.0);
    QVERIFY(loaded->squelchEnabled);
    QVERIFY(restored.lastError().isEmpty());
}

void BookmarkTreeModelTest::derivesAndUpdatesTriStateScannerInclusion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    BookmarkTreeModel model(path);
    const QString groupUuid = model.addGroup(-1, QStringLiteral("Scan set"));
    const int groupRow = model.visibleRowForUuid(groupUuid);
    const QString firstUuid = model.addBookmark(
        groupRow,
        bookmark(QStringLiteral("First"), QStringLiteral("am")));
    const QString subgroupUuid = model.addGroup(groupRow, QStringLiteral("Nested"));
    const int subgroupRow = model.visibleRowForUuid(subgroupUuid);
    const QString secondUuid = model.addBookmark(
        subgroupRow,
        bookmark(QStringLiteral("Second"), QStringLiteral("nfm")));
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(checkState(model, groupRow), static_cast<int>(Qt::Unchecked));

    const int firstRow = model.visibleRowForUuid(firstUuid);
    QVERIFY(model.toggleScannerInclusion(firstRow));
    QCOMPARE(checkState(model, groupRow), static_cast<int>(Qt::PartiallyChecked));
    QCOMPARE(
        checkState(model, model.visibleRowForUuid(subgroupUuid)),
        static_cast<int>(Qt::Unchecked));

    QVERIFY(model.toggleScannerInclusion(groupRow));
    QCOMPARE(checkState(model, groupRow), static_cast<int>(Qt::Checked));
    QCOMPARE(
        checkState(model, model.visibleRowForUuid(firstUuid)),
        static_cast<int>(Qt::Checked));
    QCOMPARE(
        checkState(model, model.visibleRowForUuid(secondUuid)),
        static_cast<int>(Qt::Checked));

    QVERIFY(model.toggleScannerInclusion(groupRow));
    QCOMPARE(checkState(model, groupRow), static_cast<int>(Qt::Unchecked));
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    BookmarkTreeModel restored(path);
    QVERIFY(waitUntil([&restored] { return !restored.loading(); }));
    QCOMPARE(
        checkState(restored, restored.visibleRowForUuid(groupUuid)),
        static_cast<int>(Qt::Unchecked));
}

void BookmarkTreeModelTest::snapshotsCheckedBookmarksInSavedOrder()
{
    QTemporaryDir directory;
    BookmarkTreeModel model(directory.filePath(QStringLiteral("bookmarks.json")));
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    const QString first = model.addBookmark(
        -1, bookmark(QStringLiteral("First"), QStringLiteral("am"), true));
    const QString skipped = model.addBookmark(
        -1, bookmark(QStringLiteral("Skipped"), QStringLiteral("am")));
    const QString last = model.addBookmark(
        -1, bookmark(QStringLiteral("Last"), QStringLiteral("usb"), true));
    const QString group = model.addGroup(-1, QStringLiteral("Checked group"));
    const QString groupedFirst = model.addBookmark(
        model.visibleRowForUuid(group),
        bookmark(QStringLiteral("Grouped first"), QStringLiteral("am")));
    const QString groupedSecond = model.addBookmark(
        model.visibleRowForUuid(group),
        bookmark(QStringLiteral("Grouped second"), QStringLiteral("nfm")));
    QVERIFY(model.toggleScannerInclusion(model.visibleRowForUuid(group)));
    QVERIFY(model.setExpanded(model.visibleRowForUuid(group), false));
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    QVERIFY(model.moveBookmark(last, first, QStringLiteral("before")));

    const auto snapshot = model.scannerBookmarks();
    QCOMPARE(snapshot.size(), std::size_t{4});
    QCOMPARE(snapshot.at(0).uuid, last);
    QCOMPARE(snapshot.at(0).bookmark.name, QStringLiteral("Last"));
    QCOMPARE(snapshot.at(1).uuid, first);
    QCOMPARE(snapshot.at(1).bookmark.name, QStringLiteral("First"));
    QCOMPARE(snapshot.at(2).uuid, groupedFirst);
    QCOMPARE(snapshot.at(3).uuid, groupedSecond);
    QVERIFY(std::none_of(snapshot.cbegin(), snapshot.cend(),
        [&skipped](const sdr::app::BookmarkSnapshot& entry) {
            return entry.uuid == skipped;
        }));
}

void BookmarkTreeModelTest::recoversFromMissingMalformedAndUnsupportedFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));

    BookmarkTreeModel missing(path);
    QVERIFY(waitUntil([&missing] { return !missing.loading(); }));
    QCOMPARE(missing.rowCount(), 0);
    QVERIFY(missing.lastError().isEmpty());
    QVERIFY(!QFile::exists(path));

    {
        QFile malformed(path);
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        QCOMPARE(malformed.write("{ definitely-not-json"), 21);
    }
    BookmarkTreeModel recovered(path);
    QVERIFY(waitUntil([&recovered] { return !recovered.loading(); }));
    QCOMPARE(recovered.rowCount(), 0);
    QVERIFY(recovered.lastError().contains(QStringLiteral("malformed")));
    QFile unchanged(path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("{ definitely-not-json"));
    unchanged.close();

    QVERIFY(!recovered.addGroup(-1, QStringLiteral("Recovered")).isEmpty());
    QVERIFY(waitUntil(
        [&recovered] { return !recovered.persistencePending(); }));
    BookmarkTreeModel afterAtomicSave(path);
    QVERIFY(waitUntil(
        [&afterAtomicSave] { return !afterAtomicSave.loading(); }));
    QCOMPARE(afterAtomicSave.rowCount(), 1);
    QVERIFY(afterAtomicSave.lastError().isEmpty());

    QJsonObject unsupported;
    unsupported.insert(QStringLiteral("version"), 99);
    unsupported.insert(QStringLiteral("root"), QJsonObject{});
    QFile unsupportedFile(path);
    QVERIFY(unsupportedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    unsupportedFile.write(QJsonDocument(unsupported).toJson());
    unsupportedFile.close();
    BookmarkTreeModel unsupportedModel(path);
    QVERIFY(waitUntil(
        [&unsupportedModel] { return !unsupportedModel.loading(); }));
    QCOMPARE(unsupportedModel.rowCount(), 0);
    QVERIFY(unsupportedModel.lastError().contains(QStringLiteral("version")));
}

void BookmarkTreeModelTest::boundsFileAndRejectedLoadsPreserveModel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    BookmarkTreeModel model(path);
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    const QString uuid = model.addBookmark(-1, bookmark(QStringLiteral("Retained"), QStringLiteral("am")));
    QVERIFY(!uuid.isEmpty());
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));

    const QByteArray oversized(
        sdr::platform::bookmarkLimits::maximumFileBytes + 1, 'x');
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(oversized), oversized.size());
    file.close();
    QVERIFY(model.reload());
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    QCOMPARE(model.visibleRowForUuid(uuid) >= 0, true);
    QVERIFY(model.lastError().contains(QStringLiteral("size limit")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), oversized);
}

void BookmarkTreeModelTest::boundsParsedTreeAndDetectsLateDuplicate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));

    QJsonArray tooWide;
    for (qsizetype index = 0;
         index < sdr::platform::bookmarkLimits::maximumNodeCount;
         ++index) {
        tooWide.append(serializedBookmark());
    }
    writeDocument(path, bookmarkDocument(serializedGroup(tooWide)));
    BookmarkTreeModel wide(path);
    QVERIFY(waitUntil([&wide] { return !wide.loading(); }, 5'000));
    QCOMPARE(wide.rowCount(), 0);
    QVERIFY(wide.lastError().contains(QStringLiteral("node limit")));

    tooWide.removeLast();
    writeDocument(path, bookmarkDocument(serializedGroup(tooWide)));
    BookmarkTreeModel full(path);
    QVERIFY(waitUntil([&full] { return !full.loading(); }, 5'000));
    QVERIFY(full.addGroup(-1, QStringLiteral("Too many")).isEmpty());
    QVERIFY(full.lastError().contains(QStringLiteral("node limit")));

    QJsonArray duplicateNearEnd;
    const QUuid duplicateUuid = QUuid::createUuid();
    for (qsizetype index = 0;
         index < sdr::platform::bookmarkLimits::maximumNodeCount - 1;
         ++index) {
        duplicateNearEnd.append(serializedBookmark(
            index + 1 == sdr::platform::bookmarkLimits::maximumNodeCount - 1
                ? duplicateUuid
                : (index == 0 ? duplicateUuid : QUuid::createUuid())));
    }
    writeDocument(path, bookmarkDocument(serializedGroup(duplicateNearEnd)));
    BookmarkTreeModel duplicate(path);
    QVERIFY(waitUntil([&duplicate] { return !duplicate.loading(); }, 5'000));
    QCOMPARE(duplicate.rowCount(), 0);
    QVERIFY(duplicate.lastError().contains(QStringLiteral("duplicate UUID")));

    QJsonObject nested = serializedBookmark();
    for (int depth = 0;
         depth <= sdr::platform::bookmarkLimits::maximumTreeDepth;
         ++depth) {
        nested = serializedGroup(QJsonArray{nested});
    }
    writeDocument(path, bookmarkDocument(serializedGroup(QJsonArray{nested})));
    BookmarkTreeModel deep(path);
    QVERIFY(waitUntil([&deep] { return !deep.loading(); }));
    QCOMPARE(deep.rowCount(), 0);
    QVERIFY(deep.lastError().contains(QStringLiteral("nesting depth")));

    nested = serializedGroup();
    for (int depth = 1;
         depth < sdr::platform::bookmarkLimits::maximumTreeDepth;
         ++depth) {
        nested = serializedGroup(QJsonArray{nested});
    }
    writeDocument(path, bookmarkDocument(serializedGroup(QJsonArray{nested})));
    BookmarkTreeModel deepest(path);
    QVERIFY(waitUntil([&deepest] { return !deepest.loading(); }));
    QCOMPARE(deepest.rowCount(), sdr::platform::bookmarkLimits::maximumTreeDepth);
    QVERIFY(deepest.addBookmark(
                deepest.rowCount() - 1,
                bookmark(QStringLiteral("Too deep"), QStringLiteral("am")))
                .isEmpty());
    QVERIFY(deepest.lastError().contains(QStringLiteral("nesting depth")));

    QJsonObject longName = serializedBookmark();
    longName.insert(
        QStringLiteral("name"),
        QString(sdr::platform::bookmarkLimits::maximumNameUtf8Bytes + 1,
                QLatin1Char('n')));
    writeDocument(path, bookmarkDocument(serializedGroup(QJsonArray{longName})));
    BookmarkTreeModel nameTooLong(path);
    QVERIFY(waitUntil([&nameTooLong] { return !nameTooLong.loading(); }));
    QVERIFY(nameTooLong.lastError().contains(QStringLiteral("name exceeds")));
}

void BookmarkTreeModelTest::boundsStringsAndPayloadMutations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BookmarkTreeModel model(directory.filePath(QStringLiteral("bookmarks.json")));
    QVERIFY(waitUntil([&model] { return !model.loading(); }));

    BookmarkData valid = bookmark(
        QString(sdr::platform::bookmarkLimits::maximumNameUtf8Bytes, QLatin1Char('a')),
        QString(sdr::platform::bookmarkLimits::maximumDemodulatorIdUtf8Bytes, QLatin1Char('d')));
    valid.modeSpecificSettings = payloadAtLimit(
        QStringLiteral("future"), QJsonObject{{QStringLiteral("version"), 1}});
    const QString uuid = model.addBookmark(-1, valid);
    QVERIFY(!uuid.isEmpty());
    const int row = model.visibleRowForUuid(uuid);
    QVERIFY(row >= 0);

    BookmarkData longName = valid;
    longName.name.append(QLatin1Char('a'));
    QVERIFY(model.addBookmark(-1, longName).isEmpty());
    QVERIFY(model.lastError().contains(QStringLiteral("name exceeds")));
    BookmarkData longId = valid;
    longId.demodulatorId.append(QLatin1Char('d'));
    QVERIFY(model.addBookmark(-1, longId).isEmpty());
    QVERIFY(model.lastError().contains(QStringLiteral("ID exceeds")));
    BookmarkData largeSettings = valid;
    largeSettings.modeSpecificSettings.insert(
        QStringLiteral("future"),
        largeSettings.modeSpecificSettings.value(QStringLiteral("future")).toString() +
            QLatin1Char('x'));
    QVERIFY(model.addBookmark(-1, largeSettings).isEmpty());
    QVERIFY(model.lastError().contains(QStringLiteral("mode settings")));

    QVariantMap fields = model.itemDetails(row);
    fields.insert(QStringLiteral("modeSpecificSettings"),
                  largeSettings.modeSpecificSettings.toVariantMap());
    QVERIFY(!model.updateBookmark(row, fields));
    QCOMPARE(model.bookmarkAt(row)->modeSpecificSettings, valid.modeSpecificSettings);
}

void BookmarkTreeModelTest::preservesBoundedExtensionsAcrossRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bookmarks.json"));
    QJsonObject item = serializedBookmark();
    const QJsonObject extension = payloadAtLimit(QStringLiteral("futureItem"));
    for (auto it = extension.begin(); it != extension.end(); ++it) {
        item.insert(it.key(), it.value());
    }
    const QString uuid = item.value(QStringLiteral("uuid")).toString();
    writeDocument(path, bookmarkDocument(serializedGroup(QJsonArray{item})));

    BookmarkTreeModel model(path);
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    const int row = model.visibleRowForUuid(uuid);
    QVERIFY(row >= 0);
    QVariantMap fields = model.itemDetails(row);
    fields.insert(QStringLiteral("name"), QStringLiteral("Changed"));
    QVERIFY(model.updateBookmark(row, fields));
    QVERIFY(waitUntil([&model] { return !model.persistencePending(); }));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(findSerializedItem(saved.value(QStringLiteral("root")).toObject(), uuid)
                 .value(QStringLiteral("futureItem")),
             extension.value(QStringLiteral("futureItem")));

    QJsonObject oversizedItem = item;
    oversizedItem.insert(
        QStringLiteral("futureItem"),
        extension.value(QStringLiteral("futureItem")).toString() + QLatin1Char('x'));
    writeDocument(path, bookmarkDocument(serializedGroup(QJsonArray{oversizedItem})));
    QVERIFY(model.reload());
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    QVERIFY(model.visibleRowForUuid(uuid) >= 0);
    QVERIFY(model.lastError().contains(QStringLiteral("extensions exceed")));

    QJsonObject oversizedModeItem = serializedBookmark();
    QJsonObject oversizedMode = payloadAtLimit(
        QStringLiteral("future"), QJsonObject{{QStringLiteral("version"), 1}});
    oversizedMode.insert(
        QStringLiteral("future"),
        oversizedMode.value(QStringLiteral("future")).toString() + QLatin1Char('x'));
    oversizedModeItem.insert(QStringLiteral("modeSpecificSettings"), oversizedMode);
    writeDocument(path, bookmarkDocument(serializedGroup(QJsonArray{oversizedModeItem})));
    QVERIFY(model.reload());
    QVERIFY(waitUntil([&model] { return !model.loading(); }));
    QVERIFY(model.visibleRowForUuid(uuid) >= 0);
    QVERIFY(model.lastError().contains(QStringLiteral("mode settings")));
}

QTEST_GUILESS_MAIN(BookmarkTreeModelTest)

#include "BookmarkTreeModelTest.moc"
