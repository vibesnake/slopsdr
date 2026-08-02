// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationFileDialogs.hpp"
#include "ApplicationModel.hpp"
#include "GnuRadioReceiverBackend.hpp"
#include "RecordedAudioBackend.hpp"
#include "ReceiverRuntime.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace
{

QString cleanPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void appendLittleEndian16(QByteArray& bytes, std::uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendLittleEndian32(QByteArray& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
}

QString writePcmWav(const QTemporaryDir& directory)
{
    QByteArray samples(32, '\0');
    QByteArray wav{"RIFF", 4};
    appendLittleEndian32(wav, static_cast<std::uint32_t>(36 + samples.size()));
    wav.append("WAVEfmt ", 8);
    appendLittleEndian32(wav, 16);
    appendLittleEndian16(wav, 1);
    appendLittleEndian16(wav, 1);
    appendLittleEndian32(wav, 48'000);
    appendLittleEndian32(wav, 96'000);
    appendLittleEndian16(wav, 2);
    appendLittleEndian16(wav, 16);
    wav.append("data", 4);
    appendLittleEndian32(wav, static_cast<std::uint32_t>(samples.size()));
    wav.append(samples);
    const QString path = directory.filePath(QStringLiteral("content-detected.bin"));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(wav) != wav.size())
        return {};
    return path;
}

bool waitUntil(const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

QString transitionDiagnostic(const ApplicationModel& model,
    const QSignalSpy& snapshots, qsizetype snapshotCount)
{
    return QStringLiteral(
               "snapshots=%1 (expected > %2), status=%3, loaded=%4, "
               "source=%5, recording=%6, metadataRequired=%7")
        .arg(snapshots.count())
        .arg(snapshotCount)
        .arg(model.statusText())
        .arg(model.recordingLoaded())
        .arg(model.sourceDescription())
        .arg(model.recordingDisplayName())
        .arg(model.recordedIqMetadataRequired());
}

bool waitForTransition(const QSignalSpy& snapshots, qsizetype snapshotCount,
    const std::function<bool()>& predicate)
{
    return waitUntil([&snapshots, snapshotCount, &predicate] {
        return snapshots.count() > snapshotCount && predicate();
    });
}

bool acceptSelectedFile(QFileDialog* dialog)
{
    // Do not use QFileDialog::accept(): its validation needs an asynchronously
    // populated filesystem view. This test covers the application's accepted
    // signal, controller, model, and runtime path instead.
    const bool accepted = QMetaObject::invokeMethod(
        dialog, "accepted", Qt::DirectConnection);
    dialog->hide();
    return accepted;
}

} // namespace

class RecordingDialogPlaybackIntegrationTest final : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void loadsRawIqManualFallbackAndWavThroughSharedDialog();

  private:
    QTemporaryDir m_settingsDirectory;
};

void RecordingDialogPlaybackIntegrationTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("slopSDR"));
    QCoreApplication::setApplicationName(QStringLiteral("slopSDR"));
    qputenv("XDG_CONFIG_HOME", m_settingsDirectory.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       m_settingsDirectory.path());
}

void RecordingDialogPlaybackIntegrationTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
}

void RecordingDialogPlaybackIntegrationTest::loadsRawIqManualFallbackAndWavThroughSharedDialog()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    const QString rawPath = recordings.filePath(QStringLiteral("sidecar capture.raw"));
    {
        QFile raw(rawPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray(32, '\0')), qint64{32});
    }
    {
        QFile sidecar(recordings.filePath(QStringLiteral("sidecar capture.json")));
        QVERIFY(sidecar.open(QIODevice::WriteOnly));
        const QByteArray json =
            "{\"hardware_center_frequency_hz\":101000000,"
            "\"sample_rate_hz\":200000,\"sample_format\":\"cf32_le\","
            "\"byte_order\":\"little-endian\",\"written_sample_count\":4}";
        QCOMPARE(sidecar.write(json), json.size());
    }
    const QString manualPath = recordings.filePath(QStringLiteral("manual.raw"));
    {
        QFile raw(manualPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray(32, '\0')), qint64{32});
    }
    const QString malformedPath = recordings.filePath(QStringLiteral("malformed.raw"));
    {
        QFile raw(malformedPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write("bad", 3), qint64{3});
    }
    const QString invalidSidecarPath =
        recordings.filePath(QStringLiteral("invalid-sidecar.raw"));
    {
        QFile raw(invalidSidecarPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray(32, '\0')), qint64{32});
        QFile sidecar(recordings.filePath(QStringLiteral("invalid-sidecar.json")));
        QVERIFY(sidecar.open(QIODevice::WriteOnly));
        QCOMPARE(sidecar.write("{}", 2), qint64{2});
    }
    const QString wavPath = writePcmWav(recordings);
    QVERIFY(!wavPath.isEmpty());

    sdr::app::ReceiverRuntime::Factories factories;
    factories.createRecordedBackend = [](sdr::radio::RecordedIqSourceConfiguration source) {
        return std::make_unique<sdr::dsp::GnuRadioReceiverBackend>(std::move(source));
    };
    factories.createRecordedAudioBackend = [](const std::string& path) {
        return std::make_unique<sdr::dsp::RecordedAudioBackend>(
            std::filesystem::path(path));
    };
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock, std::move(factories));
    ApplicationModel model(runtime);
    QSignalSpy snapshots(&runtime, &sdr::app::ReceiverRuntime::snapshotChanged);
    QSignalSpy loadRequests(&runtime,
                            &sdr::app::ReceiverRuntime::loadRecordingRequested);
    runtime.start();
    QVERIFY2(waitUntil([&model, &snapshots] {
        return snapshots.count() > 0 && model.mockMode() &&
               model.statusText().contains(QStringLiteral("Mock backend ready"));
    }), qPrintable(transitionDiagnostic(model, snapshots, 0)));

    int forwardedCount = 0;
    QStringList forwardedPaths;
    sdr::gui::ApplicationFileDialogs dialogs(
        [&model, &forwardedCount, &forwardedPaths](const QUrl& url) {
            ++forwardedCount;
            forwardedPaths.append(url.toLocalFile());
            model.loadRecording(url);
        },
        {}, {}, [&recordings] { return recordings.path(); }, {});
    QSignalSpy selections(
        &dialogs, &sdr::gui::ApplicationFileDialogs::recordingFileSelected);
    const auto acceptRecording = [&dialogs](const QString& path) {
        dialogs.openRecordingFileDialog();
        dialogs.dialog()->selectFile(path);
        return acceptSelectedFile(dialogs.dialog());
    };
    const auto verifyRequest = [&loadRequests, &selections](int count,
                                   const QString& expectedPath) {
        QCOMPARE(selections.count(), count);
        QCOMPARE(loadRequests.count(), count);
        QCOMPARE(loadRequests.at(count - 1).constFirst().toString(), expectedPath);
    };

    QVERIFY(QDir().mkpath(recordings.filePath(QStringLiteral("nested"))));
    qsizetype snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(
        recordings.filePath(QStringLiteral("nested/../sidecar capture.raw"))));
    verifyRequest(1, cleanPath(rawPath));
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model] {
        return model.recordingLoaded() && model.recordedIqSource() &&
               model.recordingDisplayName() == QStringLiteral("sidecar capture.raw");
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(manualPath));
    verifyRequest(2, cleanPath(manualPath));
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model] {
        return model.recordedIqMetadataRequired();
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordingLoaded());
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("sidecar capture.raw"));

    snapshotCount = snapshots.count();
    model.selectRecordedIqSource(
        QUrl::fromLocalFile(manualPath), 102'000'000, 250'000);
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model] {
        return model.recordingLoaded() && !model.recordedIqMetadataRequired() &&
               model.recordingDisplayName() == QStringLiteral("manual.raw");
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(invalidSidecarPath));
    verifyRequest(3, cleanPath(invalidSidecarPath));
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model] {
        return model.recordedIqMetadataRequired();
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordingLoaded());
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("manual.raw"));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(malformedPath));
    verifyRequest(4, cleanPath(malformedPath));
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model] {
        return model.statusText().contains(QStringLiteral("selection failed"));
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordingLoaded());
    QCOMPARE(model.recordingDisplayName(), QStringLiteral("manual.raw"));
    QVERIFY(model.statusText().contains(QStringLiteral("truncated")));

    snapshotCount = snapshots.count();
    model.ejectRecording();
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model] {
        return !model.recordingLoaded();
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));

    snapshotCount = snapshots.count();
    QVERIFY(acceptRecording(wavPath));
    verifyRequest(5, cleanPath(wavPath));
    QVERIFY2(waitForTransition(snapshots, snapshotCount, [&model, &wavPath] {
        return model.recordingLoaded() &&
               model.recordingDisplayName() == QFileInfo(wavPath).fileName();
    }), qPrintable(transitionDiagnostic(model, snapshots, snapshotCount)));
    QVERIFY(model.recordedAudioSource());
    QCOMPARE(forwardedCount, 5);
    QCOMPARE(forwardedPaths,
             QStringList({cleanPath(rawPath), cleanPath(manualPath),
                          cleanPath(invalidSidecarPath), cleanPath(malformedPath),
                          cleanPath(wavPath)}));

    runtime.shutdown();
}

QTEST_MAIN(RecordingDialogPlaybackIntegrationTest)

#include "RecordingDialogPlaybackIntegrationTest.moc"
