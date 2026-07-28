// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ConfigurationMigration.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace sdr::platform {
namespace {

constexpr auto legacyApplicationName = "vibeSDR";

void appendMessage(
    QList<ConfigurationMigrationMessage>& messages,
    ConfigurationMigrationSeverity severity,
    const QString& text)
{
    messages.append({severity, text});
}

void migrateFile(
    const QString& sourcePath,
    const QString& destinationPath,
    QList<ConfigurationMigrationMessage>& messages)
{
    if (!QFileInfo::exists(sourcePath) || QFileInfo::exists(destinationPath)) {
        return;
    }

    const QDir destinationDirectory = QFileInfo(destinationPath).dir();
    if (!destinationDirectory.exists() &&
        !QDir().mkpath(destinationDirectory.absolutePath())) {
        appendMessage(
            messages,
            ConfigurationMigrationSeverity::Warning,
            QStringLiteral("Could not create configuration migration destination"));
        return;
    }

    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        appendMessage(
            messages,
            ConfigurationMigrationSeverity::Warning,
            QStringLiteral("Could not read legacy configuration file %1")
                .arg(QFileInfo(sourcePath).fileName()));
        return;
    }

    QTemporaryFile temporaryFile(
        destinationDirectory.filePath(QStringLiteral(".migration-XXXXXX")));
    if (!temporaryFile.open()) {
        appendMessage(
            messages,
            ConfigurationMigrationSeverity::Warning,
            QStringLiteral("Could not prepare configuration migration for %1")
                .arg(QFileInfo(sourcePath).fileName()));
        return;
    }
    const QByteArray contents = sourceFile.readAll();
    if (temporaryFile.write(contents) != contents.size() || !temporaryFile.flush()) {
        appendMessage(
            messages,
            ConfigurationMigrationSeverity::Warning,
            QStringLiteral("Could not copy legacy configuration file %1")
                .arg(QFileInfo(sourcePath).fileName()));
        return;
    }
    temporaryFile.close();

    if (!QFile::rename(temporaryFile.fileName(), destinationPath)) {
        appendMessage(
            messages,
            ConfigurationMigrationSeverity::Warning,
            QStringLiteral("Could not copy legacy configuration file %1")
                .arg(QFileInfo(sourcePath).fileName()));
        return;
    }

    appendMessage(
        messages,
        ConfigurationMigrationSeverity::Info,
        QStringLiteral("Copied legacy configuration file %1")
            .arg(QFileInfo(sourcePath).fileName()));
}

}  // namespace

QString configurationRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

QList<ConfigurationMigrationMessage> migrateLegacyConfiguration(const QString& root)
{
    QList<ConfigurationMigrationMessage> messages;
    const QString activeRoot = root.isEmpty() ? configurationRoot() : root;
    const QString applicationName = QCoreApplication::applicationName();
    if (activeRoot.isEmpty() || applicationName.isEmpty()) {
        return messages;
    }

    const QDir configRoot(activeRoot);
    const QString legacyDirectory = configRoot.filePath(
        QString::fromLatin1(legacyApplicationName));
    const QString activeDirectory = configRoot.filePath(applicationName);
    migrateFile(
        QDir(legacyDirectory).filePath(
            QString::fromLatin1(legacyApplicationName) + QStringLiteral(".conf")),
        QDir(activeDirectory).filePath(applicationName + QStringLiteral(".conf")),
        messages);
    migrateFile(
        QDir(legacyDirectory).filePath(QStringLiteral("bookmarks.json")),
        QDir(activeDirectory).filePath(QStringLiteral("bookmarks.json")),
        messages);
    return messages;
}

}  // namespace sdr::platform
