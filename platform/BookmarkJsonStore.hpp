// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <QJsonDocument>
#include <QString>

namespace sdr::platform {

class BookmarkJsonStore final
{
public:
    enum class LoadStatus {
        Missing,
        Loaded,
        Error,
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        QJsonDocument document;
        QString error;
    };

    explicit BookmarkJsonStore(QString filePath = {});

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] LoadResult load() const;
    [[nodiscard]] bool save(
        const QJsonDocument& document, QString& error) const;

    [[nodiscard]] static QString defaultFilePath();

private:
    QString m_filePath;
};

}  // namespace sdr::platform
