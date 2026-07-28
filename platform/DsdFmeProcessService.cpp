// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "DsdFmeProcessService.hpp"

#include <QDir>
#include <QProcess>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sdr::platform {
namespace {

constexpr std::size_t inputBytesPerSample = sizeof(std::int16_t);
constexpr std::size_t decodedStereoBytesPerFrame = sizeof(float) * 2U;
constexpr std::size_t maximumQueuedInputBytes =
    radio::receiverAudioSampleRate * inputBytesPerSample;
constexpr qint64 maximumChildPendingBytes = 16 * 1024;
constexpr qsizetype maximumWriteBytes = 8 * 1024;
constexpr qint64 maximumReadBytesPerProcess = 8 * 1024;
constexpr qsizetype maximumRecentStandardErrorBytes = 16 * 1024;
constexpr qsizetype maximumBufferedStandardErrorBytes = 32 * 1024;
constexpr qsizetype maximumStandardErrorLogBytesPerProcess = 8 * 1024;
constexpr qsizetype maximumStandardErrorLineBytes = 4 * 1024;
constexpr int maximumStandardErrorLinesPerProcess = 64;
constexpr std::size_t decodedOutputCapacitySamples =
    radio::receiverAudioSampleRate * 2;
constexpr int gracefulShutdownMilliseconds = 250;
constexpr int forcedShutdownMilliseconds = 100;
constexpr int decoderUpsamplingFactor = 6;
constexpr auto repeatedMessageInterval = std::chrono::seconds(1);

float sanitizedDecodedSample(const char* bytes) noexcept
{
    static_assert(sizeof(float) == 4U);
    float sample = 0.0F;
    std::memcpy(&sample, bytes, sizeof(sample));
    return std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
}

void appendInt16LittleEndian(QByteArray& bytes, std::int16_t value)
{
    const auto bits = static_cast<std::uint16_t>(value);
    bytes.append(static_cast<char>(bits & 0xffU));
    bytes.append(static_cast<char>((bits >> 8U) & 0xffU));
}

class QProcessDsdFmeChild final : public DsdFmeChildProcess
{
public:
    void start(const QString& program, const QStringList& arguments) override
    {
        m_process.setProcessChannelMode(QProcess::SeparateChannels);
        m_process.start(program, arguments, QIODevice::ReadWrite);
    }

    [[nodiscard]] DsdFmeChildState state() const noexcept override
    {
        switch (m_process.state()) {
        case QProcess::NotRunning:
            return DsdFmeChildState::NotRunning;
        case QProcess::Starting:
            return DsdFmeChildState::Starting;
        case QProcess::Running:
            return DsdFmeChildState::Running;
        }
        return DsdFmeChildState::NotRunning;
    }

    [[nodiscard]] qint64 bytesToWrite() const noexcept override
    {
        return m_process.bytesToWrite();
    }

    [[nodiscard]] qint64 write(const QByteArray& bytes) override
    {
        return m_process.write(bytes);
    }

    [[nodiscard]] qint64 standardOutputBytesAvailable() noexcept override
    {
        if (!m_process.isOpen()) {
            return 0;
        }
        m_process.setReadChannel(QProcess::StandardOutput);
        return m_process.bytesAvailable();
    }

    [[nodiscard]] QByteArray readStandardOutput(qint64 maximumBytes) override
    {
        if (!m_process.isOpen() || maximumBytes <= 0) {
            return {};
        }
        m_process.setReadChannel(QProcess::StandardOutput);
        return m_process.read(maximumBytes);
    }

    [[nodiscard]] QByteArray readStandardError(qint64 maximumBytes) override
    {
        if (!m_process.isOpen() || maximumBytes <= 0) {
            return {};
        }
        m_process.setReadChannel(QProcess::StandardError);
        return m_process.read(maximumBytes);
    }

    [[nodiscard]] QString errorString() const override
    {
        return m_process.errorString();
    }

    [[nodiscard]] int exitCode() const noexcept override
    {
        return m_process.exitCode();
    }

    [[nodiscard]] bool crashed() const noexcept override
    {
        return m_process.exitStatus() == QProcess::CrashExit;
    }

    void closeWriteChannel() override
    {
        m_process.closeWriteChannel();
    }

    void terminate() override
    {
        m_process.terminate();
    }

    [[nodiscard]] bool waitForFinished(int milliseconds) override
    {
        return m_process.waitForFinished(milliseconds);
    }

    void kill() override
    {
        m_process.kill();
    }

private:
    QProcess m_process;
};

}  // namespace

DsdFmeProcessService::DsdFmeProcessService(
    std::unique_ptr<DsdFmeChildProcess> child)
    : m_child(std::move(child))
    , m_decodedSamples(decodedOutputCapacitySamples)
{
    if (!m_child) {
        throw std::invalid_argument(
            "DSD-FME process service requires a child-process implementation");
    }
}

DsdFmeProcessService::~DsdFmeProcessService()
{
    stop();
}

void DsdFmeProcessService::setBinaryPath(QString path)
{
    path = path.trimmed();
    if (path == QLatin1String("~")) {
        path = QDir::homePath();
    } else if (path.startsWith(QLatin1String("~/"))) {
        path = QDir::homePath() + path.mid(1);
    }
    if (!path.isEmpty() && QDir::isRelativePath(path)) {
        path = QDir::current().absoluteFilePath(path);
    }
    if (!path.isEmpty()) {
        path = QDir::cleanPath(path);
    }
    if (m_binaryPath == path) {
        return;
    }
    const bool restart = m_active;
    stop();
    m_binaryPath = std::move(path);
    if (restart) {
        start();
    } else if (m_binaryPath.isEmpty()) {
        setStatus(
            DsdFmeState::NotConfigured,
            QStringLiteral("DSD-FME not configured"));
    } else {
        setStatus(DsdFmeState::Stopped, QStringLiteral("DSD-FME stopped"));
    }
}

void DsdFmeProcessService::setLogHandler(
    std::function<void(DsdFmeLogSeverity, const QString&)> handler)
{
    m_logHandler = std::move(handler);
}

const QString& DsdFmeProcessService::binaryPath() const noexcept
{
    return m_binaryPath;
}

void DsdFmeProcessService::start()
{
    if (m_active) {
        return;
    }
    m_active = true;
    m_startAttempted = false;
    m_seenRunning = false;
    m_inputBytes.clear();
    m_stderrLineBytes.clear();
    m_state.standardOutputBytesReceived = 0;
    m_state.decodedStereoFramesReceived = 0;
    m_state.generatedStereoFrames = 0;
    m_lastRateCheckDecodedFrames = 0;
    m_lastRateCheckGeneratedFrames = 0;
    flushDecodedOutput();
    if (m_binaryPath.isEmpty()) {
        setStatus(
            DsdFmeState::NotConfigured,
            QStringLiteral("DSD-FME not configured"));
        log(
            DsdFmeLogSeverity::Warning,
            QStringLiteral("Cannot start: no decoder binary is configured"));
        return;
    }
    m_child->start(
        m_binaryPath,
        {QStringLiteral("-i"), QStringLiteral("-"),
         QStringLiteral("-o"), QStringLiteral("-")});
    m_startAttempted = true;
    setStatus(DsdFmeState::Starting, QStringLiteral("DSD-FME starting"));
    log(
        DsdFmeLogSeverity::Info,
        QStringLiteral("Starting %1 with arguments: -i - -o -")
            .arg(m_binaryPath));
}

void DsdFmeProcessService::stop()
{
    const bool wasActive =
        m_active || m_child->state() != DsdFmeChildState::NotRunning;
    m_active = false;
    m_inputBytes.clear();
    m_stdoutBytes.clear();
    flushDecodedOutput();
    if (m_child->state() != DsdFmeChildState::NotRunning) {
        m_child->closeWriteChannel();
        m_child->terminate();
        if (!m_child->waitForFinished(gracefulShutdownMilliseconds)) {
            m_child->kill();
            static_cast<void>(
                m_child->waitForFinished(forcedShutdownMilliseconds));
        }
    }
    drainStandardError();
    flushStandardErrorLine();
    m_startAttempted = false;
    m_seenRunning = false;
    setStatus(
        m_binaryPath.isEmpty()
            ? DsdFmeState::NotConfigured
            : DsdFmeState::Stopped,
        m_binaryPath.isEmpty()
            ? QStringLiteral("DSD-FME not configured")
            : QStringLiteral("DSD-FME stopped"));
    if (wasActive) {
        log(DsdFmeLogSeverity::Info, QStringLiteral("Decoder stopped"));
    }
}

void DsdFmeProcessService::process()
{
    if (m_child->state() == DsdFmeChildState::Running) {
        writePendingInput();
    }
    drainStandardOutput();
    drainStandardError();
    updateChildState();
}

void DsdFmeProcessService::enqueueDiscriminator(
    std::span<const float> samples)
{
    if (!m_active || samples.empty() ||
        m_state.state == DsdFmeState::NotConfigured ||
        m_state.state == DsdFmeState::Stopped ||
        m_state.state == DsdFmeState::ProcessFailed) {
        return;
    }

    const std::size_t incomingBytes = samples.size() * inputBytesPerSample;
    if (incomingBytes >= maximumQueuedInputBytes) {
        m_inputBytes.clear();
        samples = samples.last(maximumQueuedInputBytes / inputBytesPerSample);
        ++m_state.inputOverflowEvents;
        log(
            DsdFmeLogSeverity::Warning,
            QStringLiteral("Decoder input overflow; stale input was dropped"));
        setStatus(
            DsdFmeState::InputOverflow,
            QStringLiteral("DSD-FME input overflow"));
    } else if (
        static_cast<std::size_t>(m_inputBytes.size()) + incomingBytes >
        maximumQueuedInputBytes) {
        const std::size_t excess =
            static_cast<std::size_t>(m_inputBytes.size()) + incomingBytes -
            maximumQueuedInputBytes;
        const qsizetype alignedExcess = static_cast<qsizetype>(
            ((excess + inputBytesPerSample - 1) / inputBytesPerSample) *
            inputBytesPerSample);
        m_inputBytes.remove(0, alignedExcess);
        ++m_state.inputOverflowEvents;
        log(
            DsdFmeLogSeverity::Warning,
            QStringLiteral("Decoder input overflow; stale input was dropped"));
        setStatus(
            DsdFmeState::InputOverflow,
            QStringLiteral("DSD-FME input overflow"));
    }

    m_inputBytes.reserve(static_cast<qsizetype>(maximumQueuedInputBytes));
    for (const float sample : samples) {
        const float bounded = std::isfinite(sample)
                                  ? std::clamp(sample, -1.0F, 1.0F)
                                  : 0.0F;
        const auto pcm = static_cast<std::int16_t>(std::lrint(
            bounded * std::numeric_limits<std::int16_t>::max()));
        appendInt16LittleEndian(m_inputBytes, pcm);
    }
}

std::vector<float> DsdFmeProcessService::takeDecodedStereo(
    std::size_t maximumFrames)
{
    if (maximumFrames >
        std::numeric_limits<std::size_t>::max() / 2U) {
        maximumFrames = std::numeric_limits<std::size_t>::max() / 2U;
    }
    auto samples = m_decodedSamples.take(maximumFrames * 2U);
    if (samples.size() % 2U != 0U) {
        samples.pop_back();
    }
    return samples;
}

void DsdFmeProcessService::flushDecodedOutput()
{
    m_stdoutBytesToDiscard = m_child->standardOutputBytesAvailable();
    m_stdoutBytes.clear();
    m_decodedSamples.clear();
    m_havePreviousDecodedFrame = false;
    m_previousDecodedLeft = 0.0F;
    m_previousDecodedRight = 0.0F;
    m_lastRateCheckDecodedFrames = 0;
    m_lastRateCheckGeneratedFrames = 0;
}

const DsdFmeProcessState& DsdFmeProcessService::state() const noexcept
{
    return m_state;
}

std::size_t DsdFmeProcessService::queuedInputBytes() const noexcept
{
    return static_cast<std::size_t>(m_inputBytes.size());
}

std::size_t DsdFmeProcessService::decodedFrameCount() const
{
    return m_decodedSamples.size() / 2U;
}

void DsdFmeProcessService::updateChildState()
{
    const auto childState = m_child->state();
    if (!m_active) {
        return;
    }
    if (childState == DsdFmeChildState::Running) {
        m_seenRunning = true;
        if (m_state.state == DsdFmeState::Starting) {
            setStatus(DsdFmeState::Running, QStringLiteral("DSD-FME running"));
            log(DsdFmeLogSeverity::Info, QStringLiteral("Decoder is running"));
        }
        return;
    }
    if (childState == DsdFmeChildState::Starting) {
        return;
    }
    if ((m_startAttempted || m_seenRunning) &&
        m_state.state != DsdFmeState::ProcessFailed) {
        flushStandardErrorLine();
        setProcessFailure();
    }
}

void DsdFmeProcessService::drainStandardError()
{
    QStringList lines;
    QString repeatedLine;
    int repeatedLineCount = 0;
    int processedLineCount = 0;
    qsizetype loggedBytes = 0;
    const auto appendLine = [&lines, &repeatedLine, &repeatedLineCount](
                                QString line) {
        if (line.isEmpty()) {
            return;
        }
        if (line == repeatedLine) {
            ++repeatedLineCount;
            return;
        }
        if (!repeatedLine.isEmpty()) {
            lines.append(
                repeatedLineCount == 1
                    ? repeatedLine
                    : QStringLiteral("%1 (repeated %2 times)")
                          .arg(
                              repeatedLine,
                              QString::number(repeatedLineCount)));
        }
        repeatedLine = std::move(line);
        repeatedLineCount = 1;
    };
    const auto collectLines = [this,
                               &appendLine,
                               &loggedBytes,
                               &processedLineCount] {
        while (processedLineCount < maximumStandardErrorLinesPerProcess &&
               loggedBytes < maximumStandardErrorLogBytesPerProcess) {
            const qsizetype newline = m_stderrLineBytes.indexOf('\n');
            const bool completeLine = newline >= 0;
            if (!completeLine &&
                m_stderrLineBytes.size() <= maximumStandardErrorLineBytes) {
                break;
            }

            qsizetype lineBytes = maximumStandardErrorLineBytes;
            qsizetype removeBytes = maximumStandardErrorLineBytes;
            if (completeLine && newline <= maximumStandardErrorLineBytes) {
                lineBytes = newline;
                removeBytes = newline + 1;
            }
            if (loggedBytes + removeBytes >
                maximumStandardErrorLogBytesPerProcess) {
                break;
            }
            QByteArray line = m_stderrLineBytes.first(lineBytes);
            m_stderrLineBytes.remove(0, removeBytes);
            loggedBytes += removeBytes;
            ++processedLineCount;
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            appendLine(QString::fromUtf8(line).trimmed());
        }
    };

    collectLines();
    const qsizetype availableBufferBytes =
        maximumBufferedStandardErrorBytes - m_stderrLineBytes.size();
    if (availableBufferBytes > 0) {
        const QByteArray chunk = m_child->readStandardError(
            std::min<qint64>(maximumReadBytesPerProcess, availableBufferBytes));
        if (!chunk.isEmpty()) {
            QByteArray retained = m_state.recentStandardError.toUtf8();
            retained.append(chunk);
            if (retained.size() > maximumRecentStandardErrorBytes) {
                retained = retained.last(maximumRecentStandardErrorBytes);
            }
            m_state.recentStandardError = QString::fromUtf8(retained);
            m_stderrLineBytes.append(chunk);
            collectLines();
        }
    }

    if (!repeatedLine.isEmpty()) {
        lines.append(
            repeatedLineCount == 1
                ? repeatedLine
                : QStringLiteral("%1 (repeated %2 times)")
                      .arg(
                          repeatedLine,
                          QString::number(repeatedLineCount)));
    }
    if (!lines.isEmpty()) {
        log(DsdFmeLogSeverity::Info, lines.join(QLatin1Char('\n')));
    }
}

void DsdFmeProcessService::flushStandardErrorLine()
{
    if (m_stderrLineBytes.isEmpty()) {
        return;
    }
    QByteArray line = std::move(m_stderrLineBytes);
    m_stderrLineBytes.clear();
    if (line.endsWith('\r')) {
        line.chop(1);
    }
    log(DsdFmeLogSeverity::Info, QString::fromUtf8(line));
}

void DsdFmeProcessService::drainStandardOutput()
{
    QByteArray chunk = m_child->readStandardOutput(maximumReadBytesPerProcess);
    if (m_stdoutBytesToDiscard > 0 && !chunk.isEmpty()) {
        const qsizetype discardBytes = static_cast<qsizetype>(std::min<qint64>(
            m_stdoutBytesToDiscard, chunk.size()));
        chunk.remove(0, discardBytes);
        m_stdoutBytesToDiscard -= discardBytes;
    }
    m_state.standardOutputBytesReceived += static_cast<quint64>(chunk.size());
    m_stdoutBytes.append(chunk);
    const qsizetype completeBytes =
        m_stdoutBytes.size() -
        (m_stdoutBytes.size() % static_cast<qsizetype>(decodedStereoBytesPerFrame));
    for (qsizetype offset = 0;
         offset < completeBytes;
         offset += static_cast<qsizetype>(decodedStereoBytesPerFrame)) {
        const char* frame = m_stdoutBytes.constData() + offset;
        const float left = sanitizedDecodedSample(frame);
        const float right = sanitizedDecodedSample(frame + sizeof(float));
        ++m_state.decodedStereoFramesReceived;
        appendDecodedFrame(left, right);
    }
    if (completeBytes > 0) {
        m_stdoutBytes.remove(0, completeBytes);
    }
    checkDecodedOutputRate();
}

void DsdFmeProcessService::writePendingInput()
{
    if (m_inputBytes.isEmpty() ||
        m_child->bytesToWrite() >= maximumChildPendingBytes) {
        return;
    }
    const qint64 allowance =
        maximumChildPendingBytes - m_child->bytesToWrite();
    const qsizetype byteCount = std::min(
        {m_inputBytes.size(), maximumWriteBytes,
         static_cast<qsizetype>(allowance)});
    if (byteCount <= 0) {
        return;
    }
    const qint64 written = m_child->write(m_inputBytes.first(byteCount));
    if (written < 0 || written > byteCount) {
        log(
            DsdFmeLogSeverity::Error,
            QStringLiteral("Decoder stdin pipe error: %1")
                .arg(m_child->errorString()));
        setProcessFailure();
        return;
    }
    if (written > 0) {
        m_inputBytes.remove(0, static_cast<qsizetype>(written));
    }
}

void DsdFmeProcessService::appendDecodedFrame(float left, float right)
{
    if (!m_havePreviousDecodedFrame) {
        m_previousDecodedLeft = left;
        m_previousDecodedRight = right;
        m_havePreviousDecodedFrame = true;
    }

    std::array<float, decoderUpsamplingFactor * 2> output{};
    for (int index = 0; index < decoderUpsamplingFactor; ++index) {
        const float fraction =
            static_cast<float>(index) /
            static_cast<float>(decoderUpsamplingFactor);
        output[static_cast<std::size_t>(index) * 2U] =
            m_previousDecodedLeft
            + (left - m_previousDecodedLeft) * fraction;
        output[static_cast<std::size_t>(index) * 2U + 1U] =
            m_previousDecodedRight
            + (right - m_previousDecodedRight) * fraction;
    }
    m_state.generatedStereoFrames += decoderUpsamplingFactor;
    const auto result = m_decodedSamples.push(output);
    if (result.droppedSamples > 0) {
        ++m_state.outputOverflowEvents;
        log(
            DsdFmeLogSeverity::Warning,
            QStringLiteral("Decoder output overflow; stale audio was dropped"));
        setStatus(
            DsdFmeState::OutputOverflow,
            QStringLiteral("DSD-FME output overflow"));
    }
    m_previousDecodedLeft = left;
    m_previousDecodedRight = right;
}

void DsdFmeProcessService::checkDecodedOutputRate()
{
    const quint64 decodedDelta =
        m_state.decodedStereoFramesReceived - m_lastRateCheckDecodedFrames;
    if (decodedDelta < static_cast<quint64>(
                           radio::receiverAudioSampleRate / 8)) {
        return;
    }
    const quint64 generatedDelta =
        m_state.generatedStereoFrames - m_lastRateCheckGeneratedFrames;
    const quint64 expectedFrames =
        decodedDelta * static_cast<quint64>(decoderUpsamplingFactor);
    if (generatedDelta != expectedFrames) {
        log(
            DsdFmeLogSeverity::Warning,
            QStringLiteral(
                "DSD-FME decoded audio rate mismatch: %1 stereo frames "
                "produced %2 stereo frames; expected %3")
                .arg(decodedDelta)
                .arg(generatedDelta)
                .arg(expectedFrames));
    }
    m_lastRateCheckDecodedFrames = m_state.decodedStereoFramesReceived;
    m_lastRateCheckGeneratedFrames = m_state.generatedStereoFrames;
}

void DsdFmeProcessService::setProcessFailure()
{
    m_inputBytes.clear();
    flushDecodedOutput();
    QString detail = m_child->errorString().trimmed();
    if (detail.isEmpty()) {
        detail = m_state.recentStandardError.trimmed();
    }
    setStatus(
        DsdFmeState::ProcessFailed,
        detail.isEmpty()
            ? QStringLiteral("DSD-FME process failed")
            : QStringLiteral("DSD-FME process failed: %1").arg(detail));
    log(
        DsdFmeLogSeverity::Error,
        QStringLiteral("Decoder %1 (exit code %2): %3")
            .arg(
                m_child->crashed() ? QStringLiteral("crashed")
                                   : QStringLiteral("exited"),
                QString::number(m_child->exitCode()),
                detail.isEmpty() ? QStringLiteral("process failed") : detail));
}

void DsdFmeProcessService::setStatus(DsdFmeState state, QString text)
{
    m_state.state = state;
    m_state.statusText = std::move(text);
}

void DsdFmeProcessService::log(
    DsdFmeLogSeverity severity, const QString& message)
{
    const QString trimmed = message.trimmed();
    if (!m_logHandler || trimmed.isEmpty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (trimmed == m_lastLoggedDecoderMessage &&
        now - m_lastDecoderMessageTime < repeatedMessageInterval) {
        return;
    }
    m_lastLoggedDecoderMessage = trimmed;
    m_lastDecoderMessageTime = now;
    m_logHandler(severity, trimmed);
}

std::unique_ptr<DsdFmeChildProcess> makeQProcessDsdFmeChild()
{
    return std::make_unique<QProcessDsdFmeChild>();
}

std::unique_ptr<DsdFmeProcessService> makeDsdFmeProcessService()
{
    return std::make_unique<DsdFmeProcessService>(
        makeQProcessDsdFmeChild());
}

}  // namespace sdr::platform
