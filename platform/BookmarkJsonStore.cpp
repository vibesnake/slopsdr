// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "BookmarkJsonStore.hpp"

#include "BookmarkLimits.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace sdr::platform {

BookmarkJsonStore::BookmarkJsonStore(QString filePath)
    : m_filePath(filePath.isEmpty() ? defaultFilePath() : std::move(filePath))
{
}

QString BookmarkJsonStore::filePath() const
{
    return m_filePath;
}

BookmarkJsonStore::LoadResult BookmarkJsonStore::load() const
{
    QFile file(m_filePath);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {
            LoadStatus::Error,
            {},
            QStringLiteral("Could not open bookmarks: %1")
                .arg(file.errorString()),
        };
    }
    if (file.size() > bookmarkLimits::maximumFileBytes) {
        return {
            LoadStatus::Error,
            {},
            QStringLiteral("Bookmarks file exceeds the %1 MiB size limit")
                .arg(bookmarkLimits::maximumFileBytes / (1024 * 1024)),
        };
    }
    const QByteArray bytes = file.read(bookmarkLimits::maximumFileBytes + 1);
    if (bytes.size() > bookmarkLimits::maximumFileBytes || !file.atEnd()) {
        return {
            LoadStatus::Error,
            {},
            QStringLiteral("Bookmarks file exceeds the %1 MiB size limit")
                .arg(bookmarkLimits::maximumFileBytes / (1024 * 1024)),
        };
    }
    if (file.error() != QFile::NoError) {
        return {
            LoadStatus::Error,
            {},
            QStringLiteral("Could not read bookmarks: %1")
                .arg(file.errorString()),
        };
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return {
            LoadStatus::Error,
            {},
            QStringLiteral("Bookmarks file is malformed: %1")
                .arg(parseError.errorString()),
        };
    }
    return {LoadStatus::Loaded, document, {}};
}

bool BookmarkJsonStore::save(
    const QJsonDocument& document, QString& error) const
{
    const QByteArray bytes = document.toJson(QJsonDocument::Indented);
    if (bytes.size() > bookmarkLimits::maximumFileBytes) {
        error = QStringLiteral("Bookmarks file exceeds the %1 MiB size limit")
                    .arg(bookmarkLimits::maximumFileBytes / (1024 * 1024));
        return false;
    }
    const QFileInfo target(m_filePath);
    QDir directory = target.dir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        error = QStringLiteral("Could not create bookmark directory %1")
                    .arg(directory.absolutePath());
        return false;
    }
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Could not save bookmarks: %1")
                    .arg(file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("Could not atomically save bookmarks: %1")
                    .arg(file.errorString());
        return false;
    }
    error.clear();
    return true;
}

QString BookmarkJsonStore::defaultFilePath()
{
    QString directory = QStandardPaths::writableLocation(
        QStandardPaths::GenericConfigLocation);
    directory = QDir(directory).filePath(QCoreApplication::applicationName());
    return QDir(directory).filePath(QStringLiteral("bookmarks.json"));
}

}  // namespace sdr::platform
