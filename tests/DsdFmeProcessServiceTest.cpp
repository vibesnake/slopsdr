// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "DsdFmeProcessService.hpp"

#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

namespace {

class FakeChildProcess final : public sdr::platform::DsdFmeChildProcess
{
public:
    void start(const QString& program, const QStringList& arguments) override
    {
        startedProgram = program;
        startedArguments = arguments;
        childState = sdr::platform::DsdFmeChildState::Starting;
        ++startCount;
    }

    [[nodiscard]] sdr::platform::DsdFmeChildState state()
        const noexcept override
    {
        return childState;
    }

    [[nodiscard]] qint64 bytesToWrite() const noexcept override
    {
        return pendingWriteBytes;
    }

    [[nodiscard]] qint64 write(const QByteArray& bytes) override
    {
        operationOrder.append(QStringLiteral("write"));
        if (failWrites) {
            return -1;
        }
        const qsizetype accepted = std::min(maximumWriteSize, bytes.size());
        written.append(bytes.first(accepted));
        return accepted;
    }

    [[nodiscard]] qint64 standardOutputBytesAvailable() noexcept override
    {
        return standardOutput.size();
    }

    [[nodiscard]] QByteArray readStandardOutput(qint64 maximumBytes) override
    {
        operationOrder.append(QStringLiteral("stdout"));
        return takeBytes(standardOutput, maximumBytes);
    }

    [[nodiscard]] QByteArray readStandardError(qint64 maximumBytes) override
    {
        operationOrder.append(QStringLiteral("stderr"));
        maximumStandardErrorReadRequest = std::max(
            maximumStandardErrorReadRequest, maximumBytes);
        return takeBytes(standardError, maximumBytes);
    }

    [[nodiscard]] QString errorString() const override
    {
        return error;
    }

    void closeWriteChannel() override
    {
        writeChannelClosed = true;
    }

    void terminate() override
    {
        terminated = true;
        childState = sdr::platform::DsdFmeChildState::NotRunning;
    }

    [[nodiscard]] bool waitForFinished(int) override
    {
        return childState == sdr::platform::DsdFmeChildState::NotRunning;
    }

    void kill() override
    {
        killed = true;
        childState = sdr::platform::DsdFmeChildState::NotRunning;
    }

    QString startedProgram;
    QStringList startedArguments;
    sdr::platform::DsdFmeChildState childState =
        sdr::platform::DsdFmeChildState::NotRunning;
    QByteArray written;
    QByteArray standardOutput;
    QByteArray standardError;
    QStringList operationOrder;
    QString error;
    qint64 pendingWriteBytes = 0;
    qint64 maximumStandardErrorReadRequest = 0;
    qsizetype maximumWriteSize = std::numeric_limits<qsizetype>::max();
    int startCount = 0;
    bool failWrites = false;
    bool writeChannelClosed = false;
    bool terminated = false;
    bool killed = false;

private:
    static QByteArray takeBytes(QByteArray& bytes, qint64 maximumBytes)
    {
        const qsizetype byteCount = static_cast<qsizetype>(std::min<qint64>(
            bytes.size(), std::max<qint64>(maximumBytes, 0)));
        QByteArray result = bytes.first(byteCount);
        bytes.remove(0, byteCount);
        return result;
    }
};

void appendNativeFloat(QByteArray& bytes, float value)
{
    static_assert(sizeof(float) == 4U);
    std::array<char, sizeof(float)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(value));
    bytes.append(encoded.data(), static_cast<qsizetype>(encoded.size()));
}

void appendNativeStereoFrame(QByteArray& bytes, float left, float right)
{
    appendNativeFloat(bytes, left);
    appendNativeFloat(bytes, right);
}

}  // namespace

class DsdFmeProcessServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void launchesExactCommandAndFramesInput();
    void reconstructsEveryFragmentedFloatFrameRemainder();
    void preservesStereoChannelsAndProducesExactSixToOneFrames();
    void preservesOneKilohertzStereoToneFrequency();
    void arbitraryChunkingProducesIdenticalOutput();
    void continuousOutputFeedsSinkWithoutSyntheticGaps();
    void silencesNonFiniteSamplesAndClampsFiniteSamples();
    void measuresBoundedDiscriminatorDiagnostics();
    void countsPartialAndFailedWrites();
    void boundsInputDuringBackpressure();
    void boundsDecodedOutput();
    void processFailureDoesNotRestart();
    void flushesDecodedOutputWithoutRestarting();
    void reconstructsFragmentedStderrWithoutLoggingStdout();
    void boundsStderrWorkPreservesOrderAndKeepsInputMoving();
    void reportsDiagnosticsAtBoundedIntervals();
};

void DsdFmeProcessServiceTest::launchesExactCommandAndFramesInput()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));

    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    QCOMPARE(fake->startedProgram, QStringLiteral("/opt/dsd-fme"));
    QCOMPARE(
        fake->startedArguments,
        QStringList({
            QStringLiteral("-i"),
            QStringLiteral("-"),
            QStringLiteral("-o"),
            QStringLiteral("-"),
        }));

    fake->childState = sdr::platform::DsdFmeChildState::Running;
    const std::array samples{-1.0F, 0.0F, 1.0F};
    service.enqueueDiscriminator(samples);
    service.process();

    QCOMPARE(fake->written.size(), qsizetype{6});
    QCOMPARE(
        static_cast<std::uint8_t>(fake->written.at(0)),
        std::uint8_t{1});
    QCOMPARE(
        static_cast<std::uint8_t>(fake->written.at(1)),
        std::uint8_t{128});
    QCOMPARE(fake->written.at(2), char{0});
    QCOMPARE(fake->written.at(3), char{0});
    QCOMPARE(
        static_cast<std::uint8_t>(fake->written.at(4)),
        std::uint8_t{255});
    QCOMPARE(
        static_cast<std::uint8_t>(fake->written.at(5)),
        std::uint8_t{127});
    QCOMPARE(service.state().state, sdr::platform::DsdFmeState::Running);
}

void DsdFmeProcessServiceTest::reconstructsEveryFragmentedFloatFrameRemainder()
{
    QByteArray encoded;
    appendNativeStereoFrame(encoded, 0.25F, -0.5F);
    QCOMPARE(encoded.size(), qsizetype{8});

    for (qsizetype remainder = 1; remainder <= 7; ++remainder) {
        auto child = std::make_unique<FakeChildProcess>();
        auto* fake = child.get();
        sdr::platform::DsdFmeProcessService service(std::move(child));
        service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
        service.start();
        fake->childState = sdr::platform::DsdFmeChildState::Running;

        fake->standardOutput = encoded.first(remainder);
        service.process();
        QCOMPARE(service.decodedFrameCount(), std::size_t{0});
        QCOMPARE(
            service.state().standardOutputBytesReceived,
            static_cast<quint64>(remainder));

        fake->standardOutput = encoded.sliced(remainder);
        service.process();
        QCOMPARE(service.decodedFrameCount(), std::size_t{6});
        const auto stereo = service.takeDecodedStereo(6);
        QCOMPARE(stereo.size(), std::size_t{12});
        for (std::size_t frame = 0; frame < 6; ++frame) {
            QCOMPARE(stereo[frame * 2U], 0.25F);
            QCOMPARE(stereo[frame * 2U + 1U], -0.5F);
        }
        QCOMPARE(service.state().standardOutputBytesReceived, quint64{8});
        QCOMPARE(service.state().decodedStereoFramesReceived, quint64{1});
        QCOMPARE(service.state().generatedStereoFrames, quint64{6});
    }
}

void DsdFmeProcessServiceTest::preservesStereoChannelsAndProducesExactSixToOneFrames()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;

    for (int frame = 0; frame < 8'000; ++frame) {
        const float left = frame % 2 == 0 ? 0.25F : -0.25F;
        const float right = frame % 2 == 0 ? -0.75F : 0.75F;
        appendNativeStereoFrame(fake->standardOutput, left, right);
    }
    service.process();
    QVERIFY(!fake->standardOutput.isEmpty());
    while (!fake->standardOutput.isEmpty()) {
        service.process();
    }

    QCOMPARE(service.decodedFrameCount(), std::size_t{48'000});
    QCOMPARE(service.state().standardOutputBytesReceived, quint64{64'000});
    QCOMPARE(service.state().decodedStereoFramesReceived, quint64{8'000});
    QCOMPARE(service.state().generatedStereoFrames, quint64{48'000});
    const auto stereo = service.takeDecodedStereo(48'000);
    QCOMPARE(stereo.size(), std::size_t{96'000});
    for (std::size_t frame = 0; frame < 6; ++frame) {
        QCOMPARE(stereo[frame * 2U], 0.25F);
        QCOMPARE(stereo[frame * 2U + 1U], -0.75F);
    }
    QVERIFY(stereo[14] != stereo[15]);
}

void DsdFmeProcessServiceTest::preservesOneKilohertzStereoToneFrequency()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;

    for (int frame = 0; frame < 8'000; ++frame) {
        const float phase = static_cast<float>(
            2.0 * std::numbers::pi * 1'000.0 * frame / 8'000.0);
        const float tone = 0.8F * std::sin(phase);
        appendNativeStereoFrame(fake->standardOutput, tone, -0.5F * tone);
    }
    service.process();
    QVERIFY(!fake->standardOutput.isEmpty());
    while (!fake->standardOutput.isEmpty()) {
        service.process();
    }
    const auto stereo = service.takeDecodedStereo(48'000);
    QCOMPARE(stereo.size(), std::size_t{96'000});

    std::size_t positiveCrossings = 0;
    for (std::size_t frame = 1; frame < 48'000; ++frame) {
        if (stereo[(frame - 1U) * 2U] <= 0.0F &&
            stereo[frame * 2U] > 0.0F) {
            ++positiveCrossings;
        }
    }
    const double measuredFrequency =
        static_cast<double>(positiveCrossings) * 48'000.0 / 47'999.0;
    QVERIFY(std::abs(measuredFrequency - 1'000.0) < 2.0);
    for (std::size_t frame = 0; frame < 48'000; ++frame) {
        QVERIFY(std::isfinite(stereo[frame * 2U]));
        QVERIFY(std::isfinite(stereo[frame * 2U + 1U]));
    }
}

void DsdFmeProcessServiceTest::arbitraryChunkingProducesIdenticalOutput()
{
    QByteArray encoded;
    for (int frame = 0; frame < 257; ++frame) {
        appendNativeStereoFrame(
            encoded,
            static_cast<float>(frame - 128) / 128.0F,
            static_cast<float>(128 - frame) / 256.0F);
    }
    const auto decode = [&encoded](const std::vector<qsizetype>& chunks) {
        auto child = std::make_unique<FakeChildProcess>();
        auto* fake = child.get();
        sdr::platform::DsdFmeProcessService service(std::move(child));
        service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
        service.start();
        fake->childState = sdr::platform::DsdFmeChildState::Running;
        qsizetype offset = 0;
        std::size_t chunkIndex = 0;
        while (offset < encoded.size()) {
            const qsizetype size = std::min(
                encoded.size() - offset,
                chunks[chunkIndex++ % chunks.size()]);
            fake->standardOutput = encoded.mid(offset, size);
            service.process();
            offset += size;
        }
        return service.takeDecodedStereo(257U * 6U);
    };

    const auto contiguous = decode({encoded.size()});
    const auto fragmented = decode({1, 7, 2, 5, 3, 4, 6, 11, 8});
    QCOMPARE(fragmented, contiguous);
}

void DsdFmeProcessServiceTest::continuousOutputFeedsSinkWithoutSyntheticGaps()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;

    for (int frame = 0; frame < 240; ++frame) {
        appendNativeStereoFrame(fake->standardOutput, 0.2F, -0.2F);
    }
    service.process();
    QCOMPARE(service.takeDecodedStereo(1'440).size(), std::size_t{2'880});

    std::size_t simulatedUnderruns = 0;
    for (int block = 0; block < 400; ++block) {
        for (int frame = 0; frame < 80; ++frame) {
            appendNativeStereoFrame(fake->standardOutput, 0.2F, -0.2F);
        }
        service.process();
        const auto sinkBlock = service.takeDecodedStereo(480);
        if (sinkBlock.size() != 960U) {
            ++simulatedUnderruns;
        }
    }
    QCOMPARE(simulatedUnderruns, std::size_t{0});
    QCOMPARE(service.state().outputOverflowEvents, quint64{0});
}

void DsdFmeProcessServiceTest::silencesNonFiniteSamplesAndClampsFiniteSamples()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    appendNativeStereoFrame(
        fake->standardOutput,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity());
    appendNativeStereoFrame(fake->standardOutput, 2.0F, -2.0F);
    appendNativeStereoFrame(fake->standardOutput, 1.0F, -1.0F);
    service.process();

    const auto stereo = service.takeDecodedStereo(18);
    QCOMPARE(stereo.size(), std::size_t{36});
    for (std::size_t sample = 0; sample < 12; ++sample) {
        QCOMPARE(stereo[sample], 0.0F);
    }
    for (const float sample : stereo) {
        QVERIFY(std::isfinite(sample));
        QVERIFY(sample >= -1.0F);
        QVERIFY(sample <= 1.0F);
    }
    QVERIFY(stereo[22] > 0.0F);
    QVERIFY(stereo[23] < 0.0F);
}

void DsdFmeProcessServiceTest::measuresBoundedDiscriminatorDiagnostics()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setDiagnosticsEnabled(true);
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;

    const std::array samples{
        0.5F,
        -0.5F,
        2.0F,
        -2.0F,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
    };
    service.enqueueDiscriminator(samples);
    service.reportInputDiscontinuity(7);
    service.reportDecoderAudioUnderruns(2, 1);

    const auto& diagnostics = service.diagnostics();
    QCOMPARE(diagnostics.discriminatorBlocks, quint64{1});
    QCOMPARE(diagnostics.discriminatorSamples, quint64{6});
    QVERIFY(std::abs(
                diagnostics.discriminatorRms - std::sqrt(2.5 / 6.0)) <
            1.0e-9);
    QCOMPARE(diagnostics.discriminatorPeak, 1.0);
    QCOMPARE(diagnostics.clippedSamples, quint64{2});
    QCOMPARE(diagnostics.nonFiniteSamples, quint64{2});
    QCOMPARE(diagnostics.inputDiscontinuities, quint64{1});
    QCOMPARE(diagnostics.droppedInputSamples, quint64{7});
    QCOMPARE(diagnostics.queuedInputBytes, quint64{12});
    QCOMPARE(diagnostics.peakQueuedInputBytes, quint64{12});
    QCOMPARE(diagnostics.decoderAudioUnderruns, quint64{2});
    QCOMPARE(diagnostics.decoderPlatformAudioUnderruns, quint64{1});
}

void DsdFmeProcessServiceTest::countsPartialAndFailedWrites()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setDiagnosticsEnabled(true);
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    fake->maximumWriteSize = 1;
    service.enqueueDiscriminator(std::array{0.25F, -0.25F});

    service.process();
    QCOMPARE(service.diagnostics().writeAttempts, quint64{1});
    QCOMPARE(service.diagnostics().writtenInputBytes, quint64{1});
    QCOMPARE(service.diagnostics().partialWriteEvents, quint64{1});
    QCOMPARE(service.diagnostics().failedWriteEvents, quint64{0});

    fake->failWrites = true;
    service.process();
    QCOMPARE(service.diagnostics().writeAttempts, quint64{2});
    QCOMPARE(service.diagnostics().partialWriteEvents, quint64{1});
    QCOMPARE(service.diagnostics().failedWriteEvents, quint64{1});
    QCOMPARE(service.state().state, sdr::platform::DsdFmeState::ProcessFailed);
}

void DsdFmeProcessServiceTest::reconstructsFragmentedStderrWithoutLoggingStdout()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    QStringList messages;
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setLogHandler(
        [&messages](
            sdr::platform::DsdFmeLogSeverity,
            const QString& message) { messages.append(message); });
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    fake->standardError = QByteArrayLiteral("first frag");
    fake->standardOutput = QByteArrayLiteral("binary stdout text");
    service.process();
    QVERIFY(!messages.contains(QStringLiteral("first frag")));
    fake->standardError = QByteArrayLiteral("ment\nsecond");
    service.process();
    QVERIFY(messages.contains(QStringLiteral("first fragment")));
    QVERIFY(!messages.contains(QStringLiteral("second")));
    fake->standardError = QByteArrayLiteral("\nrepeat\nrepeat\n");
    const qsizetype updatesBeforeRepeat = messages.size();
    service.process();
    QCOMPARE(messages.size(), updatesBeforeRepeat + 1);
    QVERIFY(messages.join(QLatin1Char('\n')).contains(
        QStringLiteral("repeat (repeated 2 times)")));
    fake->childState = sdr::platform::DsdFmeChildState::NotRunning;
    service.process();
    QVERIFY(messages.join(QLatin1Char('\n')).contains(QStringLiteral("second")));
    QVERIFY(!messages.join(QLatin1Char('\n')).contains(
        QStringLiteral("binary stdout text")));
}

void DsdFmeProcessServiceTest::
    boundsStderrWorkPreservesOrderAndKeepsInputMoving()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    QStringList messages;
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setLogHandler(
        [&messages](
            sdr::platform::DsdFmeLogSeverity,
            const QString& message) { messages.append(message); });
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    service.process();
    messages.clear();
    fake->operationOrder.clear();

    constexpr int lineCount = 2'000;
    QStringList expectedLines;
    for (int index = 0; index < lineCount; ++index) {
        const QString line = QStringLiteral("decoder message %1").arg(index);
        expectedLines.append(line);
        fake->standardError.append(line.toUtf8());
        fake->standardError.append('\n');
    }
    const qsizetype initialStandardErrorBytes = fake->standardError.size();
    const std::vector<float> discriminator(4'096, 0.25F);
    service.enqueueDiscriminator(discriminator);

    service.process();

    QCOMPARE(fake->written.size(), qsizetype{8 * 1024});
    QVERIFY(!fake->standardError.isEmpty());
    QVERIFY(initialStandardErrorBytes - fake->standardError.size() <=
            qsizetype{8 * 1024});
    QCOMPARE(fake->maximumStandardErrorReadRequest, qint64{8 * 1024});
    QVERIFY(fake->operationOrder.indexOf(QStringLiteral("write")) <
            fake->operationOrder.indexOf(QStringLiteral("stderr")));
    QCOMPARE(messages.size(), qsizetype{1});

    for (int pass = 0;
         pass < 1'000 &&
         (fake->standardError.size() > 0 ||
          !messages.join(QLatin1Char('\n')).contains(expectedLines.back()));
         ++pass) {
        const qsizetype updatesBefore = messages.size();
        service.process();
        QVERIFY(messages.size() <= updatesBefore + 1);
    }

    const QStringList retainedLines =
        messages.join(QLatin1Char('\n')).split(
            QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(retainedLines, expectedLines);
}

void DsdFmeProcessServiceTest::boundsInputDuringBackpressure()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setDiagnosticsEnabled(true);
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    fake->pendingWriteBytes = 16 * 1024;

    const std::vector<float> samples(200'000, 0.25F);
    service.enqueueDiscriminator(samples);
    service.process();

    QVERIFY(service.queuedInputBytes() <= std::size_t{96'000});
    QCOMPARE(fake->written.size(), qsizetype{0});
    QVERIFY(service.state().inputOverflowEvents > 0);
    QCOMPARE(service.diagnostics().discriminatorSamples, quint64{48'000});
    QCOMPARE(service.diagnostics().inputDiscontinuities, quint64{1});
    QCOMPARE(service.diagnostics().droppedInputSamples, quint64{152'000});
    QCOMPARE(service.diagnostics().queuedInputBytes, quint64{112'384});
    QCOMPARE(service.diagnostics().peakQueuedInputBytes, quint64{112'384});
    QCOMPARE(
        service.state().state,
        sdr::platform::DsdFmeState::InputOverflow);
}

void DsdFmeProcessServiceTest::reportsDiagnosticsAtBoundedIntervals()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    QStringList messages;
    std::uint64_t nowNanoseconds = 0;
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setDiagnosticsClock(
        [&nowNanoseconds] { return nowNanoseconds; });
    service.setDiagnosticsEnabled(true);
    service.setLogHandler(
        [&messages](
            sdr::platform::DsdFmeLogSeverity,
            const QString& message) { messages.append(message); });
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    fake->maximumWriteSize = 4;
    fake->standardOutput = QByteArray(9'000, '\0');
    service.enqueueDiscriminator(std::array{0.25F, -0.5F, 0.75F});
    service.reportInputDiscontinuity(3);
    service.reportDecoderAudioUnderruns(1, 2);

    const auto diagnosticMessages = [&messages] {
        QStringList result;
        for (const QString& message : messages) {
            if (message.startsWith(
                    QStringLiteral("App-measured decoder diagnostics:"))) {
                result.append(message);
            }
        }
        return result;
    };

    nowNanoseconds = 999'999'999ULL;
    service.process();
    QVERIFY(diagnosticMessages().isEmpty());
    QCOMPARE(service.diagnostics().peakStdoutBacklogBytes, quint64{9'000});

    nowNanoseconds = 1'000'000'000ULL;
    service.process();
    QCOMPARE(diagnosticMessages().size(), qsizetype{1});
    const QString firstSummary = diagnosticMessages().front();
    QVERIFY(firstSummary.contains(QStringLiteral("input-rms=")));
    QVERIFY(firstSummary.contains(QStringLiteral("clipped=0")));
    QVERIFY(firstSummary.contains(QStringLiteral("discontinuities=1")));
    QVERIFY(firstSummary.contains(QStringLiteral("dropped-samples=3")));
    QVERIFY(firstSummary.contains(QStringLiteral("partial-writes=1")));
    QVERIFY(firstSummary.contains(QStringLiteral("stdout-backlog-peak=9000")));
    QVERIFY(firstSummary.contains(QStringLiteral("audio-underruns=1")));
    QVERIFY(firstSummary.contains(
        QStringLiteral("platform-audio-underruns=2")));

    nowNanoseconds = 1'999'999'999ULL;
    service.process();
    QCOMPARE(diagnosticMessages().size(), qsizetype{1});
    service.enqueueDiscriminator(std::array{0.125F});
    nowNanoseconds = 2'000'000'000ULL;
    service.process();
    QCOMPARE(diagnosticMessages().size(), qsizetype{2});
}

void DsdFmeProcessServiceTest::boundsDecodedOutput()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    for (int frame = 0; frame < 8'001; ++frame) {
        appendNativeStereoFrame(fake->standardOutput, 0.25F, -0.25F);
    }

    do {
        service.process();
    } while (!fake->standardOutput.isEmpty());

    QCOMPARE(service.decodedFrameCount(), std::size_t{48'000});
    QVERIFY(service.state().outputOverflowEvents > 0);
    QCOMPARE(
        service.state().state,
        sdr::platform::DsdFmeState::OutputOverflow);
}

void DsdFmeProcessServiceTest::processFailureDoesNotRestart()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/missing/dsd-fme"));
    service.start();
    fake->error = QStringLiteral("executable not found");
    fake->childState = sdr::platform::DsdFmeChildState::NotRunning;

    service.process();
    service.process();
    service.enqueueDiscriminator(std::vector<float>(1'000, 0.5F));
    service.process();

    QCOMPARE(fake->startCount, 1);
    QCOMPARE(
        service.state().state,
        sdr::platform::DsdFmeState::ProcessFailed);
    QVERIFY(service.state().statusText.contains(
        QStringLiteral("executable not found")));
    QVERIFY(service.takeDecodedStereo(100).empty());
    QCOMPARE(service.queuedInputBytes(), std::size_t{0});
}

void DsdFmeProcessServiceTest::flushesDecodedOutputWithoutRestarting()
{
    auto child = std::make_unique<FakeChildProcess>();
    auto* fake = child.get();
    sdr::platform::DsdFmeProcessService service(std::move(child));
    service.setBinaryPath(QStringLiteral("/opt/dsd-fme"));
    service.start();
    fake->childState = sdr::platform::DsdFmeChildState::Running;
    appendNativeStereoFrame(fake->standardOutput, 0.25F, -0.5F);
    service.process();
    QCOMPARE(service.decodedFrameCount(), std::size_t{6});

    appendNativeStereoFrame(fake->standardOutput, -0.75F, 0.75F);
    service.flushDecodedOutput();

    QCOMPARE(service.decodedFrameCount(), std::size_t{0});
    QCOMPARE(fake->startCount, 1);
    QCOMPARE(fake->childState, sdr::platform::DsdFmeChildState::Running);

    appendNativeStereoFrame(fake->standardOutput, 0.5F, -0.25F);
    service.process();
    const auto stereo = service.takeDecodedStereo(6);
    QCOMPARE(stereo.size(), std::size_t{12});
    for (std::size_t frame = 0; frame < 6; ++frame) {
        QCOMPARE(stereo[frame * 2U], 0.5F);
        QCOMPARE(stereo[frame * 2U + 1U], -0.25F);
    }
}

QTEST_APPLESS_MAIN(DsdFmeProcessServiceTest)

#include "DsdFmeProcessServiceTest.moc"
