// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WavRecordingService.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::uint32_t littleEndian32(const QByteArray& bytes, int offset)
{
    return static_cast<std::uint32_t>(
               static_cast<unsigned char>(bytes.at(offset))) |
           (static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes.at(offset + 1))) << 8U) |
           (static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes.at(offset + 2))) << 16U) |
           (static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes.at(offset + 3))) << 24U);
}

std::int16_t sampleAt(const QByteArray& bytes, int offset)
{
    std::int16_t sample = 0;
    std::memcpy(&sample, bytes.constData() + offset, sizeof(sample));
    return sample;
}

QByteArray fileContents(const std::filesystem::path& path)
{
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
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

class WavRecordingServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesFinalizedStereoPcmWav();
    void duplicatesAnalogMonoAndPreservesDecodedStereo();
    void boundsQueuedFramesAndRejectsInvalidFolders();
    void skipsQuietAudioWithPreRollAndTail();
    void reopensDuringTailWithoutAQuietGap();
    void usesUniqueSanitizedNames();
    void finalizesWhenDestroyed();
    void writesWithoutBlockingProducersAndReconcilesTotals();
    void reportsContentionAndWriteFailureDrops();
    void racesShutdownWithActiveProducer();
};

void WavRecordingServiceTest::writesFinalizedStereoPcmWav()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::WavRecordingService recorder;
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 145'500'000,
        .modeName = "NFM",
    }));
    recorder.enqueueMono(std::array<float, 2>{0.5F, -0.5F});
    recorder.stop();

    const auto state = recorder.state();
    QVERIFY(!state.active);
    QVERIFY(!state.failed);
    const QByteArray contents = fileContents(state.filePath);
    QVERIFY(contents.size() >= 52);
    QCOMPARE(contents.left(4), QByteArray("RIFF"));
    QCOMPARE(contents.mid(8, 4), QByteArray("WAVE"));
    QCOMPARE(littleEndian32(contents, 24), std::uint32_t{48'000});
    QCOMPARE(contents.mid(36, 4), QByteArray("data"));
    QCOMPARE(littleEndian32(contents, 40), std::uint32_t{8});
    QCOMPARE(littleEndian32(contents, 4), std::uint32_t{44});
    QCOMPARE(sampleAt(contents, 44), std::int16_t{16384});
    QCOMPARE(sampleAt(contents, 46), std::int16_t{16384});
    QCOMPARE(sampleAt(contents, 48), std::int16_t{-16384});
    QCOMPARE(sampleAt(contents, 50), std::int16_t{-16384});
}

void WavRecordingServiceTest::duplicatesAnalogMonoAndPreservesDecodedStereo()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::WavRecordingService recorder;
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 100,
        .modeName = "DMR/P25",
    }));
    recorder.enqueueMono(std::array<float, 1>{0.25F});
    recorder.enqueueStereo(std::array<float, 2>{-0.25F, 0.75F});
    recorder.stop();

    const QByteArray contents = fileContents(recorder.state().filePath);
    QVERIFY(contents.size() >= 52);
    QCOMPARE(sampleAt(contents, 44), std::int16_t{8192});
    QCOMPARE(sampleAt(contents, 46), std::int16_t{8192});
    QCOMPARE(sampleAt(contents, 48), std::int16_t{-8192});
    QCOMPARE(sampleAt(contents, 50), std::int16_t{24575});
}

void WavRecordingServiceTest::boundsQueuedFramesAndRejectsInvalidFolders()
{
    sdr::platform::WavRecordingService invalid;
    QVERIFY(!invalid.start({
        .directory = std::filesystem::path("/definitely/missing/slopsdr"),
        .frequencyHz = 1,
        .modeName = "AM",
    }));
    QVERIFY(invalid.state().failed);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::WavRecordingService recorder(1);
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 1,
        .modeName = "AM",
    }));
    recorder.enqueueStereo(std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F});
    QCOMPARE(recorder.state().droppedFrames, std::uint64_t{2});
    recorder.stop();
    QVERIFY(!recorder.state().failed);

    sdr::platform::WavRecordingService writeFailure(48'000, 4);
    QVERIFY(writeFailure.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 2,
        .modeName = "AM",
    }));
    writeFailure.enqueueStereo(std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F});
    writeFailure.stop();
    QVERIFY(writeFailure.state().failed);
    QCOMPARE(writeFailure.state().droppedFrames, std::uint64_t{2});
    QVERIFY(QString::fromStdString(writeFailure.state().statusText)
                 .contains(QStringLiteral("limit")));
}

void WavRecordingServiceTest::skipsQuietAudioWithPreRollAndTail()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::WavRecordingService recorder(48'000 * 8);
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 1,
        .modeName = "AM",
        .skipQuietParts = true,
        .preRollSeconds = 1,
        .tailSeconds = 1,
    }));
    QVERIFY(recorder.state().active);
    QVERIFY(!recorder.state().writing);
    const std::vector<float> quiet(48'000, 0.1F);
    recorder.enqueueMono(quiet, false);
    recorder.enqueueMono(std::array<float, 1>{0.5F}, true);
    QVERIFY(recorder.state().writing);
    recorder.enqueueMono(std::vector<float>(48'000, 0.2F), false);
    QVERIFY(!recorder.state().writing);
    recorder.enqueueMono(std::vector<float>(48'000, 0.3F), false);
    recorder.stop();

    const QByteArray contents = fileContents(recorder.state().filePath);
    QCOMPARE(littleEndian32(contents, 40), std::uint32_t{(48'000 + 1 + 48'000) * 4});
    QCOMPARE(sampleAt(contents, 44), std::int16_t{3277});
    QCOMPARE(sampleAt(contents, 44 + 48'000 * 4), std::int16_t{16384});
    QCOMPARE(sampleAt(contents, static_cast<int>(contents.size() - 4)),
             std::int16_t{6553});
    QCOMPARE(recorder.state().elapsedSeconds, std::uint64_t{2});
}

void WavRecordingServiceTest::reopensDuringTailWithoutAQuietGap()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::WavRecordingService recorder(48'000 * 8);
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 1,
        .modeName = "AM",
        .skipQuietParts = true,
        .preRollSeconds = 0,
        .tailSeconds = 1,
    }));
    recorder.enqueueMono(std::array<float, 1>{0.5F}, true);
    recorder.enqueueMono(std::array<float, 1>{0.2F}, false);
    recorder.enqueueMono(std::array<float, 1>{0.7F}, true);
    recorder.stop();
    const QByteArray contents = fileContents(recorder.state().filePath);
    QCOMPARE(littleEndian32(contents, 40), std::uint32_t{12});
    QCOMPARE(sampleAt(contents, 44), std::int16_t{16384});
    QCOMPARE(sampleAt(contents, 48), std::int16_t{6553});
    QCOMPARE(sampleAt(contents, 52), std::int16_t{22937});
}

void WavRecordingServiceTest::usesUniqueSanitizedNames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::platform::WavRecordingService first;
    QVERIFY(first.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 145'500'000,
        .modeName = "DMR/P25",
    }));
    first.stop();
    sdr::platform::WavRecordingService second;
    QVERIFY(second.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 145'500'000,
        .modeName = "DMR/P25",
    }));
    second.stop();
    QVERIFY(first.state().filePath != second.state().filePath);
    QVERIFY(QString::fromStdString(first.state().filePath.filename().string())
                 .contains(QStringLiteral("DMR_P25_filtered-audio.wav")));
}

void WavRecordingServiceTest::finalizesWhenDestroyed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::filesystem::path path;
    {
        sdr::platform::WavRecordingService recorder;
        QVERIFY(recorder.start({
            .directory = std::filesystem::path(directory.path().toStdString()),
            .frequencyHz = 1,
            .modeName = "AM",
        }));
        recorder.enqueueMono(std::array<float, 1>{0.25F});
        path = recorder.state().filePath;
    }
    const QByteArray contents = fileContents(path);
    QVERIFY(contents.size() >= 48);
    QCOMPARE(littleEndian32(contents, 40), std::uint32_t{4});
}

void WavRecordingServiceTest::writesWithoutBlockingProducersAndReconcilesTotals()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WriteGate gate;
    sdr::platform::WavRecordingWriterHooks slowWriterHooks;
    slowWriterHooks.beforeWrite = [&gate] { gate.block(); };
    sdr::platform::WavRecordingService recorder(
        8, 0xffff'ffdbU, std::move(slowWriterHooks));
    QVERIFY(recorder.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 1,
        .modeName = "AM",
    }));
    recorder.enqueueStereo(std::array<float, 2>{0.1F, -0.1F});
    if (!gate.waitUntilEntered()) {
        gate.release();
        QFAIL("writer did not reach the controlled write");
    }

    recorder.enqueueStereo(std::array<float, 4>{0.2F, -0.2F, 0.3F, -0.3F});
    const auto whileWriting = recorder.state();
    QCOMPARE(whileWriting.queuedFrames, std::uint64_t{2});
    QCOMPARE(whileWriting.droppedFrames, std::uint64_t{0});

    gate.release();
    recorder.stop();
    const auto state = recorder.state();
    QCOMPARE(state.writtenFrames, std::uint64_t{3});
    QCOMPARE(state.droppedFrames, std::uint64_t{0});
    QCOMPARE(state.writtenFrames + state.droppedFrames, std::uint64_t{3});
    QCOMPARE(littleEndian32(fileContents(state.filePath), 40), std::uint32_t{12});
}

void WavRecordingServiceTest::reportsContentionAndWriteFailureDrops()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WriteGate contentionGate;
    sdr::platform::WavRecordingWriterHooks contentionHooks;
    contentionHooks.afterDequeueLocked = [&contentionGate] { contentionGate.block(); };
    sdr::platform::WavRecordingService contention(
        4, 0xffff'ffdbU, std::move(contentionHooks));
    QVERIFY(contention.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 1,
        .modeName = "AM",
    }));
    contention.enqueueStereo(std::array<float, 2>{0.1F, -0.1F});
    if (!contentionGate.waitUntilEntered()) {
        contentionGate.release();
        QFAIL("writer did not hold the controlled queue lock");
    }
    contention.enqueueStereo(std::array<float, 2>{0.2F, -0.2F});
    contentionGate.release();
    contention.stop();
    QCOMPARE(contention.state().writtenFrames, std::uint64_t{1});
    QCOMPARE(contention.state().droppedFrames, std::uint64_t{1});
    QCOMPARE(contention.state().writtenFrames + contention.state().droppedFrames,
             std::uint64_t{2});

    sdr::platform::WavRecordingWriterHooks failingWriterHooks;
    failingWriterHooks.failWrites = true;
    sdr::platform::WavRecordingService failed(
        4, 0xffff'ffdbU, std::move(failingWriterHooks));
    QVERIFY(failed.start({
        .directory = std::filesystem::path(directory.path().toStdString()),
        .frequencyHz = 2,
        .modeName = "AM",
    }));
    failed.enqueueStereo(std::array<float, 2>{0.1F, -0.1F});
    failed.stop();
    QVERIFY(failed.state().failed);
    QCOMPARE(failed.state().writtenFrames, std::uint64_t{0});
    QCOMPARE(failed.state().droppedFrames, std::uint64_t{1});
    QCOMPARE(failed.state().writtenFrames + failed.state().droppedFrames,
             std::uint64_t{1});
}

void WavRecordingServiceTest::racesShutdownWithActiveProducer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        WriteGate producerGate;
        sdr::platform::WavRecordingWriterHooks hooks;
        hooks.beforeProducerExit = [&producerGate] { producerGate.block(); };
        sdr::platform::WavRecordingService recorder(8, 0xffff'ffdbU,
                                                     std::move(hooks));
        QVERIFY(recorder.start({
            .directory = std::filesystem::path(directory.path().toStdString()),
            .frequencyHz = iteration,
            .modeName = "AM",
        }));

        std::thread producer([&recorder] {
            recorder.enqueueStereo(std::array<float, 2>{0.25F, -0.25F});
        });
        if (!producerGate.waitUntilEntered()) {
            producerGate.release();
            producer.join();
            recorder.stop();
            QFAIL("producer did not reach the shutdown gate");
        }
        std::atomic<bool> stopReturned = false;
        std::thread stopper([&recorder, &stopReturned] {
            recorder.stop();
            stopReturned.store(true, std::memory_order_release);
        });
        producerGate.release();
        producer.join();
        stopper.join();
        QVERIFY(stopReturned.load(std::memory_order_acquire));
        const auto state = recorder.state();
        QCOMPARE(state.writtenFrames + state.droppedFrames,
                 std::uint64_t{1});
        QVERIFY(!state.active);
        QVERIFY(!state.failed);
        QVERIFY(std::filesystem::exists(state.filePath));
    }
}

QTEST_MAIN(WavRecordingServiceTest)
#include "WavRecordingServiceTest.moc"
