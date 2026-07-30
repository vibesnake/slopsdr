// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "IqRecordingService.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <complex>
#include <cstring>

namespace {

QByteArray contents(const std::filesystem::path& path)
{
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

float floatAt(const QByteArray& bytes, int offset)
{
    float value = 0.0F;
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    return value;
}

}  // namespace

class IqRecordingServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesInterleavedLittleEndianCf32AndMetadata();
    void usesCollisionSafeFullBandwidthNames();
    void boundsQueueAndFinalizesOnFailureAndDestruction();
};

void IqRecordingServiceTest::writesInterleavedLittleEndianCf32AndMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::IqRecordingService recorder;
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 145'000'000,
        .sampleRate = 2'400'000,
        .deviceIdentifier = "rtlsdr:serial=test",
    }));
    recorder.enqueue(std::array<std::complex<float>, 2>{
        std::complex<float>{0.5F, -0.25F}, std::complex<float>{-1.0F, 1.0F}});
    recorder.stop();

    const auto state = recorder.state();
    QVERIFY(!state.active);
    QVERIFY(!state.failed);
    QCOMPARE(state.filePath.extension().string(), std::string(".cf32"));
    const QByteArray data = contents(state.filePath);
    QCOMPARE(data.size(), 16);
    QCOMPARE(floatAt(data, 0), 0.5F);
    QCOMPARE(floatAt(data, 4), -0.25F);
    QCOMPARE(floatAt(data, 8), -1.0F);
    QCOMPARE(floatAt(data, 12), 1.0F);
    const auto json = QJsonDocument::fromJson(contents(state.metadataPath)).object();
    QCOMPARE(json.value(QStringLiteral("hardware_center_frequency_hz")).toInteger(),
             qint64{145'000'000});
    QCOMPARE(json.value(QStringLiteral("sample_rate_hz")).toInteger(), qint64{2'400'000});
    QCOMPARE(json.value(QStringLiteral("sample_format")).toString(), QStringLiteral("cf32"));
    QCOMPARE(json.value(QStringLiteral("byte_order")).toString(), QStringLiteral("little-endian"));
    QCOMPARE(json.value(QStringLiteral("written_sample_count")).toInteger(), qint64{2});
    QCOMPARE(json.value(QStringLiteral("dropped_sample_count")).toInteger(), qint64{0});
    QCOMPARE(json.value(QStringLiteral("device_identifier")).toString(),
             QStringLiteral("rtlsdr:serial=test"));
    QVERIFY(json.contains(QStringLiteral("start_timestamp")));
    QVERIFY(json.contains(QStringLiteral("end_timestamp")));
}

void IqRecordingServiceTest::usesCollisionSafeFullBandwidthNames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const sdr::platform::IqRecordingRequest request{
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 145'000'000,
        .sampleRate = 2'400'000,
        .deviceIdentifier = {},
    };
    sdr::platform::IqRecordingService first;
    QVERIFY(first.start(request));
    first.stop();
    sdr::platform::IqRecordingService second;
    QVERIFY(second.start(request));
    second.stop();
    QVERIFY(QString::fromStdString(first.state().filePath.filename().string())
                 .contains(QStringLiteral("145000000Hz_2400000sps_full-iq")));
    QVERIFY(first.state().filePath != second.state().filePath);
}

void IqRecordingServiceTest::boundsQueueAndFinalizesOnFailureAndDestruction()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::IqRecordingService recorder(1);
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 1,
        .sampleRate = 1,
        .deviceIdentifier = {},
    }));
    recorder.enqueue(std::array<std::complex<float>, 2>{});
    recorder.addDroppedSamples(3);
    recorder.stop();
    QVERIFY(recorder.state().droppedSamples >= 5);
    QVERIFY(QFile::exists(QString::fromStdString(recorder.state().metadataPath.string())));

    std::filesystem::path finalized;
    {
        sdr::platform::IqRecordingService scoped;
        QVERIFY(scoped.start({
            .directory = std::filesystem::path(directory.path().toStdString()),
            .centerFrequencyHz = 2,
            .sampleRate = 1,
            .deviceIdentifier = {},
        }));
        scoped.enqueue(std::array<std::complex<float>, 1>{std::complex<float>{1.0F, 0.0F}});
        finalized = scoped.state().metadataPath;
    }
    QVERIFY(QFile::exists(QString::fromStdString(finalized.string())));
}

QTEST_MAIN(IqRecordingServiceTest)
#include "IqRecordingServiceTest.moc"
