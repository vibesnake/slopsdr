// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "IqRecordingService.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

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

class WriteGate final
{
public:
    ~WriteGate() { release(); }

    void block()
    {
        std::unique_lock lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this] { return m_released; });
    }

    [[nodiscard]] bool waitUntilEntered()
    {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, std::chrono::seconds(5), [this] {
            return m_entered;
        });
    }

    void release()
    {
        std::lock_guard lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_entered = false;
    bool m_released = false;
};

}  // namespace

class IqRecordingServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesInterleavedLittleEndianCf32AndMetadata();
    void usesCollisionSafeFullBandwidthNames();
    void boundsQueueAndFinalizesOnFailureAndDestruction();
    void writesWithoutBlockingProducersAndReconcilesSidecarTotals();
    void reportsQueueContentionAndWriteFailureDrops();
    void drainsTwoPointFourMsEquivalentInputDeterministically();
    void racesShutdownWithActiveProducer();
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
    QCOMPARE(state.filePath.extension().string(), std::string(".raw"));
    const QByteArray data = contents(state.filePath);
    QCOMPARE(data.size(), 16);
    QCOMPARE(data, QByteArray::fromHex("0000003f000080be000080bf0000803f"));
    QCOMPARE(floatAt(data, 0), 0.5F);
    QCOMPARE(floatAt(data, 4), -0.25F);
    QCOMPARE(floatAt(data, 8), -1.0F);
    QCOMPARE(floatAt(data, 12), 1.0F);
    const auto json = QJsonDocument::fromJson(contents(state.metadataPath)).object();
    QCOMPARE(json.value(QStringLiteral("hardware_center_frequency_hz")).toInteger(),
             qint64{145'000'000});
    QCOMPARE(json.value(QStringLiteral("sample_rate_hz")).toInteger(), qint64{2'400'000});
    QCOMPARE(json.value(QStringLiteral("sample_format")).toString(), QStringLiteral("cf32_le"));
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
    QCOMPARE(recorder.state().droppedSamples, std::uint64_t{5});
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

void IqRecordingServiceTest::writesWithoutBlockingProducersAndReconcilesSidecarTotals()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WriteGate gate;
    sdr::platform::IqRecordingWriterHooks slowWriterHooks;
    slowWriterHooks.beforeWrite = [&gate] { gate.block(); };
    sdr::platform::IqRecordingService recorder(
        8, std::move(slowWriterHooks));
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 1,
        .sampleRate = 2'400'000,
        .deviceIdentifier = {},
    }));
    recorder.enqueue(std::array<std::complex<float>, 1>{
        std::complex<float>{0.1F, -0.1F}});
    if (!gate.waitUntilEntered()) {
        gate.release();
        QFAIL("writer did not reach the controlled write");
    }

    recorder.enqueue(std::array<std::complex<float>, 2>{
        std::complex<float>{0.2F, -0.2F}, std::complex<float>{0.3F, -0.3F}});
    const auto whileWriting = recorder.state();
    QCOMPARE(whileWriting.queuedSamples, std::uint64_t{2});
    QCOMPARE(whileWriting.droppedSamples, std::uint64_t{0});

    gate.release();
    recorder.stop();
    const auto state = recorder.state();
    QCOMPARE(state.writtenSamples, std::uint64_t{3});
    QCOMPARE(state.droppedSamples, std::uint64_t{0});
    const auto metadata = QJsonDocument::fromJson(contents(state.metadataPath)).object();
    QCOMPARE(metadata.value(QStringLiteral("written_sample_count")).toInteger(), qint64{3});
    QCOMPARE(metadata.value(QStringLiteral("dropped_sample_count")).toInteger(), qint64{0});
    QCOMPARE(state.writtenSamples + state.droppedSamples, std::uint64_t{3});
}

void IqRecordingServiceTest::reportsQueueContentionAndWriteFailureDrops()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WriteGate contentionGate;
    sdr::platform::IqRecordingWriterHooks contentionHooks;
    contentionHooks.afterDequeueLocked = [&contentionGate] { contentionGate.block(); };
    sdr::platform::IqRecordingService contention(
        2, std::move(contentionHooks));
    QVERIFY(contention.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 1,
        .sampleRate = 1,
        .deviceIdentifier = {},
    }));
    contention.enqueue(std::array<std::complex<float>, 1>{
        std::complex<float>{0.1F, -0.1F}});
    if (!contentionGate.waitUntilEntered()) {
        contentionGate.release();
        QFAIL("writer did not hold the controlled queue lock");
    }
    contention.enqueue(std::array<std::complex<float>, 1>{
        std::complex<float>{0.2F, -0.2F}});
    contentionGate.release();
    contention.stop();
    QCOMPARE(contention.state().writtenSamples, std::uint64_t{1});
    QCOMPARE(contention.state().droppedSamples, std::uint64_t{1});
    QCOMPARE(contention.state().writtenSamples + contention.state().droppedSamples,
             std::uint64_t{2});

    WriteGate queueGate;
    sdr::platform::IqRecordingWriterHooks overflowHooks;
    overflowHooks.beforeWrite = [&queueGate] { queueGate.block(); };
    sdr::platform::IqRecordingService overflow(
        2, std::move(overflowHooks));
    QVERIFY(overflow.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 2,
        .sampleRate = 1,
        .deviceIdentifier = {},
    }));
    overflow.enqueue(std::array<std::complex<float>, 1>{
        std::complex<float>{0.1F, -0.1F}});
    if (!queueGate.waitUntilEntered()) {
        queueGate.release();
        QFAIL("writer did not reach the controlled write");
    }
    overflow.enqueue(std::array<std::complex<float>, 2>{
        std::complex<float>{0.2F, -0.2F}, std::complex<float>{0.3F, -0.3F}});
    overflow.enqueue(std::array<std::complex<float>, 1>{
        std::complex<float>{0.4F, -0.4F}});
    QCOMPARE(overflow.state().droppedSamples, std::uint64_t{1});
    queueGate.release();
    overflow.stop();
    QCOMPARE(overflow.state().writtenSamples, std::uint64_t{3});
    QCOMPARE(overflow.state().droppedSamples, std::uint64_t{1});
    QCOMPARE(overflow.state().writtenSamples + overflow.state().droppedSamples,
             std::uint64_t{4});

    sdr::platform::IqRecordingWriterHooks failingWriterHooks;
    failingWriterHooks.failWrites = true;
    sdr::platform::IqRecordingService failed(2, std::move(failingWriterHooks));
    QVERIFY(failed.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 3,
        .sampleRate = 1,
        .deviceIdentifier = {},
    }));
    failed.enqueue(std::array<std::complex<float>, 1>{
        std::complex<float>{0.1F, -0.1F}});
    failed.stop();
    QVERIFY(failed.state().failed);
    QCOMPARE(failed.state().writtenSamples, std::uint64_t{0});
    QCOMPARE(failed.state().droppedSamples, std::uint64_t{1});
    QVERIFY(QFile::exists(QString::fromStdString(failed.state().metadataPath.string())));
    const auto failedMetadata =
        QJsonDocument::fromJson(contents(failed.state().metadataPath)).object();
    QCOMPARE(failedMetadata.value(QStringLiteral("written_sample_count")).toInteger(),
             qint64{0});
    QCOMPARE(failedMetadata.value(QStringLiteral("dropped_sample_count")).toInteger(),
             qint64{1});
}

void IqRecordingServiceTest::drainsTwoPointFourMsEquivalentInputDeterministically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WriteGate gate;
    constexpr std::size_t samplesPerChunk = 100'000;
    constexpr std::size_t chunks = 24;
    sdr::platform::IqRecordingWriterHooks sustainedWriterHooks;
    sustainedWriterHooks.beforeWrite = [&gate] { gate.block(); };
    sdr::platform::IqRecordingService recorder(
        samplesPerChunk * chunks, std::move(sustainedWriterHooks));
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .centerFrequencyHz = 1,
        .sampleRate = 2'400'000,
        .deviceIdentifier = {},
    }));
    const std::vector<std::complex<float>> chunk(
        samplesPerChunk, {0.25F, -0.5F});
    recorder.enqueue(chunk);
    if (!gate.waitUntilEntered()) {
        gate.release();
        QFAIL("writer did not reach the controlled write");
    }
    for (std::size_t index = 1; index < chunks; ++index) {
        recorder.enqueue(chunk);
    }
    QCOMPARE(recorder.state().queuedSamples,
             std::uint64_t{samplesPerChunk * (chunks - 1)});
    gate.release();
    recorder.stop();
    const auto state = recorder.state();
    QCOMPARE(state.writtenSamples, std::uint64_t{samplesPerChunk * chunks});
    QCOMPARE(state.droppedSamples, std::uint64_t{0});
    QCOMPARE(std::filesystem::file_size(state.filePath),
             std::uintmax_t{samplesPerChunk * chunks * 8});
}

void IqRecordingServiceTest::racesShutdownWithActiveProducer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        WriteGate writerGate;
        WriteGate producerGate;
        WriteGate stopGate;
        sdr::platform::IqRecordingWriterHooks hooks;
        hooks.beforeWriterLoop = [&writerGate] { writerGate.block(); };
        hooks.afterStopRequested = [&stopGate] { stopGate.block(); };
        hooks.afterEnqueue = [&producerGate] { producerGate.block(); };
        sdr::platform::IqRecordingService recorder(8, std::move(hooks));
        QVERIFY(recorder.start({
            .directory = std::filesystem::path(directory.path().toStdString()),
            .centerFrequencyHz = iteration,
            .sampleRate = 2'400'000,
            .deviceIdentifier = {},
        }));
        if (!writerGate.waitUntilEntered()) {
            writerGate.release();
            recorder.stop();
            QFAIL("writer did not reach the startup gate");
        }

        std::thread producer([&recorder] {
            recorder.enqueue(std::array<std::complex<float>, 1>{
                std::complex<float>{0.25F, -0.5F}});
        });
        if (!producerGate.waitUntilEntered()) {
            producerGate.release();
            producer.join();
            writerGate.release();
            recorder.stop();
            QFAIL("producer did not reach the shutdown gate");
        }
        std::atomic<bool> stopReturned = false;
        std::thread stopper([&recorder, &stopReturned] {
            recorder.stop();
            stopReturned.store(true, std::memory_order_release);
        });
        if (!stopGate.waitUntilEntered()) {
            writerGate.release();
            producerGate.release();
            stopGate.release();
            producer.join();
            stopper.join();
            QFAIL("stop did not reach the shutdown gate");
        }
        QVERIFY(!stopReturned.load(std::memory_order_acquire));
        writerGate.release();
        producerGate.release();
        stopGate.release();
        producer.join();
        stopper.join();
        QVERIFY(stopReturned.load(std::memory_order_acquire));
        const auto state = recorder.state();
        QCOMPARE(state.writtenSamples + state.droppedSamples,
                 std::uint64_t{1});
        QVERIFY(!state.active);
        QVERIFY(!state.failed);
        QVERIFY(std::filesystem::exists(state.filePath));
        QVERIFY(std::filesystem::exists(state.metadataPath));
        const auto metadata = QJsonDocument::fromJson(contents(state.metadataPath)).object();
        QCOMPARE(metadata.value(QStringLiteral("written_sample_count")).toInteger(),
                 qint64{1});
        QCOMPARE(metadata.value(QStringLiteral("dropped_sample_count")).toInteger(),
                 qint64{0});
    }
}

QTEST_MAIN(IqRecordingServiceTest)
#include "IqRecordingServiceTest.moc"
