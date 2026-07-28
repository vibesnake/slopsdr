// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "AudioSampleBuffer.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace sdr::platform {

enum class DsdFmeChildState {
    NotRunning,
    Starting,
    Running,
};

enum class DsdFmeLogSeverity {
    Debug,
    Info,
    Warning,
    Error,
};

class DsdFmeChildProcess
{
public:
    virtual ~DsdFmeChildProcess() = default;

    virtual void start(const QString& program, const QStringList& arguments) = 0;
    [[nodiscard]] virtual DsdFmeChildState state() const noexcept = 0;
    [[nodiscard]] virtual qint64 bytesToWrite() const noexcept = 0;
    [[nodiscard]] virtual qint64 write(const QByteArray& bytes) = 0;
    [[nodiscard]] virtual qint64 standardOutputBytesAvailable() noexcept = 0;
    [[nodiscard]] virtual QByteArray readStandardOutput(qint64 maximumBytes) = 0;
    [[nodiscard]] virtual QByteArray readStandardError(qint64 maximumBytes) = 0;
    [[nodiscard]] virtual QString errorString() const = 0;
    [[nodiscard]] virtual int exitCode() const noexcept { return 0; }
    [[nodiscard]] virtual bool crashed() const noexcept { return false; }
    virtual void closeWriteChannel() = 0;
    virtual void terminate() = 0;
    [[nodiscard]] virtual bool waitForFinished(int milliseconds) = 0;
    virtual void kill() = 0;
};

enum class DsdFmeState {
    NotConfigured,
    Starting,
    Running,
    Stopped,
    ProcessFailed,
    InputOverflow,
    OutputOverflow,
};

struct DsdFmeProcessState {
    DsdFmeState state = DsdFmeState::NotConfigured;
    QString statusText = QStringLiteral("DSD-FME not configured");
    QString recentStandardError;
    quint64 inputOverflowEvents = 0;
    quint64 outputOverflowEvents = 0;
    quint64 standardOutputBytesReceived = 0;
    quint64 decodedStereoFramesReceived = 0;
    quint64 generatedStereoFrames = 0;

    friend bool operator==(
        const DsdFmeProcessState& left,
        const DsdFmeProcessState& right)
    {
        return left.state == right.state
            && left.statusText == right.statusText
            && left.recentStandardError == right.recentStandardError
            && left.inputOverflowEvents == right.inputOverflowEvents
            && left.outputOverflowEvents == right.outputOverflowEvents;
    }
};

struct DsdFmeDiagnostics {
    quint64 discriminatorBlocks = 0;
    quint64 discriminatorSamples = 0;
    double discriminatorRms = 0.0;
    double discriminatorPeak = 0.0;
    quint64 clippedSamples = 0;
    quint64 nonFiniteSamples = 0;
    quint64 inputDiscontinuities = 0;
    quint64 droppedInputSamples = 0;
    quint64 queuedInputBytes = 0;
    quint64 peakQueuedInputBytes = 0;
    quint64 writeAttempts = 0;
    quint64 writtenInputBytes = 0;
    quint64 partialWriteEvents = 0;
    quint64 failedWriteEvents = 0;
    quint64 stdoutBacklogBytes = 0;
    quint64 peakStdoutBacklogBytes = 0;
    quint64 decoderAudioUnderruns = 0;
    quint64 decoderPlatformAudioUnderruns = 0;
};

class DsdFmeProcessService final
{
public:
    using MonotonicClock = std::function<std::uint64_t()>;

    explicit DsdFmeProcessService(
        std::unique_ptr<DsdFmeChildProcess> child);
    ~DsdFmeProcessService();

    DsdFmeProcessService(const DsdFmeProcessService&) = delete;
    DsdFmeProcessService& operator=(const DsdFmeProcessService&) = delete;

    void setBinaryPath(QString path);
    void setLogHandler(
        std::function<void(DsdFmeLogSeverity, const QString&)> handler);
    void setDiagnosticsClock(MonotonicClock clock);
    void setDiagnosticsEnabled(bool enabled);
    [[nodiscard]] const QString& binaryPath() const noexcept;
    void start();
    void stop();
    void process();

    void enqueueDiscriminator(std::span<const float> samples);
    void reportInputDiscontinuity(quint64 droppedSamples);
    void reportDecoderAudioUnderruns(
        quint64 softwareUnderruns,
        quint64 platformUnderruns);
    [[nodiscard]] std::vector<float> takeDecodedStereo(
        std::size_t maximumFrames);
    void flushDecodedOutput();

    [[nodiscard]] const DsdFmeProcessState& state() const noexcept;
    [[nodiscard]] const DsdFmeDiagnostics& diagnostics() const noexcept;
    [[nodiscard]] std::size_t queuedInputBytes() const noexcept;
    [[nodiscard]] std::size_t decodedFrameCount() const;

private:
    void updateChildState();
    void drainStandardError();
    void flushStandardErrorLine();
    void log(DsdFmeLogSeverity severity, const QString& message);
    void drainStandardOutput();
    void writePendingInput();
    void resetDiagnostics();
    void updateQueuedInputDiagnostics();
    void updateStdoutBacklogDiagnostics();
    void reportDiagnosticsIfDue();
    void appendDecodedFrame(float left, float right);
    void checkDecodedOutputRate();
    void setProcessFailure();
    void setStatus(DsdFmeState state, QString text);

    std::unique_ptr<DsdFmeChildProcess> m_child;
    QString m_binaryPath;
    DsdFmeProcessState m_state;
    DsdFmeDiagnostics m_diagnostics;
    QByteArray m_inputBytes;
    QByteArray m_stdoutBytes;
    QByteArray m_stderrLineBytes;
    qint64 m_stdoutBytesToDiscard = 0;
    radio::AudioSampleBuffer m_decodedSamples;
    bool m_active = false;
    bool m_startAttempted = false;
    bool m_seenRunning = false;
    bool m_havePreviousDecodedFrame = false;
    float m_previousDecodedLeft = 0.0F;
    float m_previousDecodedRight = 0.0F;
    quint64 m_lastRateCheckDecodedFrames = 0;
    quint64 m_lastRateCheckGeneratedFrames = 0;
    std::function<void(DsdFmeLogSeverity, const QString&)> m_logHandler;
    MonotonicClock m_diagnosticsClock;
    bool m_diagnosticsEnabled = false;
    bool m_diagnosticsReportClockStarted = false;
    std::uint64_t m_lastDiagnosticsReportNanoseconds = 0;
    double m_discriminatorSumSquares = 0.0;
    QString m_lastLoggedDecoderMessage;
    std::chrono::steady_clock::time_point m_lastDecoderMessageTime{};
};

[[nodiscard]] std::unique_ptr<DsdFmeChildProcess> makeQProcessDsdFmeChild();
[[nodiscard]] std::unique_ptr<DsdFmeProcessService> makeDsdFmeProcessService();

}  // namespace sdr::platform
