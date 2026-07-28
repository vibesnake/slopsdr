// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <QList>
#include <QString>

namespace sdr::platform {

enum class ConfigurationMigrationSeverity {
    Info,
    Warning,
};

struct ConfigurationMigrationMessage {
    ConfigurationMigrationSeverity severity = ConfigurationMigrationSeverity::Info;
    QString text;
};

[[nodiscard]] QString configurationRoot();

[[nodiscard]] QList<ConfigurationMigrationMessage> migrateLegacyConfiguration(
    const QString& root = {});

}  // namespace sdr::platform
