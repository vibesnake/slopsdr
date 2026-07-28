// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ConfigurationMigration.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

class ConfigurationMigrationTest final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigurationMigrationTest(QString configRoot)
        : m_configRoot(std::move(configRoot))
    {
    }

private slots:
    void init();
    void respectsCustomXdgConfigHome();
    void migratesBothFilesWithoutRemovingLegacyCopies();
    void migratesSettingsOnlyAndBookmarksOnly();
    void neverOverwritesExistingNewFiles();
    void ignoresMissingLegacyFiles();
    void preservesLegacyFileWhenCopyFails();
    void isIdempotentAfterMigration();

private:
    [[nodiscard]] QString legacyDirectory() const;
    [[nodiscard]] QString activeDirectory() const;
    [[nodiscard]] QString legacySettingsPath() const;
    [[nodiscard]] QString activeSettingsPath() const;
    [[nodiscard]] QString legacyBookmarksPath() const;
    [[nodiscard]] QString activeBookmarksPath() const;
    static void writeFile(const QString& path, const QByteArray& contents);
    [[nodiscard]] static QByteArray readFile(const QString& path);

    QString m_configRoot;
};

void ConfigurationMigrationTest::init()
{
    QDir root(m_configRoot);
    if (root.exists()) {
        QVERIFY(root.removeRecursively());
    }
    QVERIFY(QDir().mkpath(m_configRoot));
}

void ConfigurationMigrationTest::respectsCustomXdgConfigHome()
{
    QCOMPARE(
        QDir::cleanPath(sdr::platform::configurationRoot()),
        QDir::cleanPath(m_configRoot));
}

void ConfigurationMigrationTest::migratesBothFilesWithoutRemovingLegacyCopies()
{
    writeFile(legacySettingsPath(), "settings");
    writeFile(legacyBookmarksPath(), "bookmarks");

    const auto messages = sdr::platform::migrateLegacyConfiguration();

    QCOMPARE(readFile(activeSettingsPath()), QByteArray("settings"));
    QCOMPARE(readFile(activeBookmarksPath()), QByteArray("bookmarks"));
    QCOMPARE(readFile(legacySettingsPath()), QByteArray("settings"));
    QCOMPARE(readFile(legacyBookmarksPath()), QByteArray("bookmarks"));
    QCOMPARE(messages.size(), 2);
}

void ConfigurationMigrationTest::migratesSettingsOnlyAndBookmarksOnly()
{
    writeFile(legacySettingsPath(), "settings-only");
    auto messages = sdr::platform::migrateLegacyConfiguration();
    QCOMPARE(readFile(activeSettingsPath()), QByteArray("settings-only"));
    QVERIFY(!QFile::exists(activeBookmarksPath()));
    QCOMPARE(messages.size(), 1);

    init();
    writeFile(legacyBookmarksPath(), "bookmarks-only");
    messages = sdr::platform::migrateLegacyConfiguration();
    QVERIFY(!QFile::exists(activeSettingsPath()));
    QCOMPARE(readFile(activeBookmarksPath()), QByteArray("bookmarks-only"));
    QCOMPARE(messages.size(), 1);
}

void ConfigurationMigrationTest::neverOverwritesExistingNewFiles()
{
    writeFile(legacySettingsPath(), "legacy-settings");
    writeFile(legacyBookmarksPath(), "legacy-bookmarks");
    writeFile(activeSettingsPath(), "active-settings");
    writeFile(activeBookmarksPath(), "active-bookmarks");

    const auto messages = sdr::platform::migrateLegacyConfiguration();

    QCOMPARE(readFile(activeSettingsPath()), QByteArray("active-settings"));
    QCOMPARE(readFile(activeBookmarksPath()), QByteArray("active-bookmarks"));
    QCOMPARE(readFile(legacySettingsPath()), QByteArray("legacy-settings"));
    QCOMPARE(readFile(legacyBookmarksPath()), QByteArray("legacy-bookmarks"));
    QVERIFY(messages.isEmpty());
}

void ConfigurationMigrationTest::ignoresMissingLegacyFiles()
{
    QVERIFY(sdr::platform::migrateLegacyConfiguration().isEmpty());
    QVERIFY(!QFile::exists(activeSettingsPath()));
    QVERIFY(!QFile::exists(activeBookmarksPath()));
}

void ConfigurationMigrationTest::preservesLegacyFileWhenCopyFails()
{
    writeFile(legacySettingsPath(), "legacy-settings");
    writeFile(activeDirectory(), "not a directory");

    const auto messages = sdr::platform::migrateLegacyConfiguration();

    QCOMPARE(readFile(legacySettingsPath()), QByteArray("legacy-settings"));
    QVERIFY(!QFile::exists(activeSettingsPath()));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(
        messages.constFirst().severity,
        sdr::platform::ConfigurationMigrationSeverity::Warning);
}

void ConfigurationMigrationTest::isIdempotentAfterMigration()
{
    writeFile(legacySettingsPath(), "settings");

    QCOMPARE(sdr::platform::migrateLegacyConfiguration().size(), 1);
    QVERIFY(sdr::platform::migrateLegacyConfiguration().isEmpty());
    QCOMPARE(readFile(activeSettingsPath()), QByteArray("settings"));
}

QString ConfigurationMigrationTest::legacyDirectory() const
{
    return QDir(m_configRoot).filePath(QStringLiteral("vibeSDR"));
}

QString ConfigurationMigrationTest::activeDirectory() const
{
    return QDir(m_configRoot).filePath(QStringLiteral("slopSDR"));
}

QString ConfigurationMigrationTest::legacySettingsPath() const
{
    return QDir(legacyDirectory()).filePath(QStringLiteral("vibeSDR.conf"));
}

QString ConfigurationMigrationTest::activeSettingsPath() const
{
    return QDir(activeDirectory()).filePath(QStringLiteral("slopSDR.conf"));
}

QString ConfigurationMigrationTest::legacyBookmarksPath() const
{
    return QDir(legacyDirectory()).filePath(QStringLiteral("bookmarks.json"));
}

QString ConfigurationMigrationTest::activeBookmarksPath() const
{
    return QDir(activeDirectory()).filePath(QStringLiteral("bookmarks.json"));
}

void ConfigurationMigrationTest::writeFile(
    const QString& path, const QByteArray& contents)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).dir().absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
}

QByteArray ConfigurationMigrationTest::readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

int main(int argc, char* argv[])
{
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        return 1;
    }
    const QString configRoot = QDir(temporaryDirectory.path()).filePath(
        QStringLiteral("custom-xdg-config"));
    qputenv("XDG_CONFIG_HOME", configRoot.toUtf8());

    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("slopSDR"));
    QCoreApplication::setApplicationName(QStringLiteral("slopSDR"));
    ConfigurationMigrationTest test(configRoot);
    return QTest::qExec(&test, argc, argv);
}

#include "ConfigurationMigrationTest.moc"
