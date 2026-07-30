// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationModel.hpp"
#include "MockReceiverBackend.hpp"
#include "ReceiverRuntime.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sdr;

QString ppmSettingsKey(const QString& identity)
{
    return QStringLiteral("receiver/ppmByDevice/") +
           QString::fromLatin1(identity.toUtf8().toHex());
}

struct RuntimeTrace {
    int discoveries = 0;
    int backendCreations = 0;
    int backendStops = 0;
    int backendDestructions = 0;
    int audioFlushes = 0;
    int audioDeviceEnumerations = 0;
    int dsdProcessStarts = 0;
    int dsdProcessStops = 0;
    std::uint64_t effectiveSampleRate = 2'000'000;
    std::size_t maximumSpectrumFftSize = 0;
    std::vector<std::uint64_t> requestedSampleRates;
    std::vector<std::uint64_t> requestedCenterFrequencies;
    std::vector<std::uint64_t> requestedListeningFrequencies;
    std::vector<double> requestedGains;
    std::vector<double> requestedPpmCorrections;
    int filterApplications = 0;
    int modeApplications = 0;
    int squelchLevelApplications = 0;
    int manualSquelchApplications = 0;
    int disabledSquelchApplications = 0;
    std::vector<std::string> receiverOperationOrder;
    bool failOpen = false;
    bool failStart = false;
    bool noDevices = false;
    bool firstDeviceMissing = false;
    bool runtimeFailureAfterStart = false;
    bool runtimeFailureReported = false;
    bool dsdFailOnStart = false;
    bool holdCalibrationReads = false;
    bool failCalibrationRead = false;
    bool unconfirmedPpmCapabilities = false;
    bool failPpmCorrection = false;
    std::optional<double> effectiveManualPpmCorrection;
    std::optional<double> squelchSignalStrengthDb = -54.0;
    std::atomic_bool recordingSquelchOpen = false;
    std::string discoveryDriver = "mock";
    bool calibrationActive = false;
    int calibrationBegins = 0;
    int calibrationEnds = 0;
    int calibrationResumes = 0;
    int calibrationSpectrumReads = 0;
    int calibrationAudioReads = 0;
    int calibrationDecoderReads = 0;
    std::uint64_t monotonicNanoseconds = 1;
    double measuredCalibrationPpm = 1.0;
    std::vector<std::string> openedIdentifiers;
    quintptr discoveryThreadToken = 0;
    quintptr openThreadToken = 0;
    quintptr startThreadToken = 0;
    quintptr stopThreadToken = 0;
    quintptr destructionThreadToken = 0;
    quintptr audioServiceThreadToken = 0;
    quintptr audioOpenThreadToken = 0;
    quintptr audioCloseThreadToken = 0;
    QString dsdProgram;
    QStringList dsdArguments;
    std::mutex recordingAudioMutex;
    std::vector<float> recordingAudioSamples;
    std::mutex recordingIqMutex;
    std::vector<std::complex<float>> recordingIqSamples;
};

class RuntimeDsdChild final : public platform::DsdFmeChildProcess
{
public:
    explicit RuntimeDsdChild(std::shared_ptr<RuntimeTrace> trace)
        : m_trace(std::move(trace))
    {
    }

    void start(const QString& program, const QStringList& arguments) override
    {
        m_trace->dsdProgram = program;
        m_trace->dsdArguments = arguments;
        ++m_trace->dsdProcessStarts;
        m_running = !m_trace->dsdFailOnStart;
    }

    [[nodiscard]] platform::DsdFmeChildState state()
        const noexcept override
    {
        return m_running
                   ? platform::DsdFmeChildState::Running
                   : platform::DsdFmeChildState::NotRunning;
    }

    [[nodiscard]] qint64 bytesToWrite() const noexcept override { return 0; }
    [[nodiscard]] qint64 write(const QByteArray& bytes) override
    {
        return bytes.size();
    }
    [[nodiscard]] qint64 standardOutputBytesAvailable() noexcept override
    {
        return 0;
    }
    [[nodiscard]] QByteArray readStandardOutput(qint64) override { return {}; }
    [[nodiscard]] QByteArray readStandardError(qint64) override { return {}; }
    [[nodiscard]] QString errorString() const override
    {
        return m_trace->dsdFailOnStart
                   ? QStringLiteral("simulated decoder failure")
                   : QString{};
    }
    void closeWriteChannel() override {}
    void terminate() override
    {
        if (m_running) {
            ++m_trace->dsdProcessStops;
        }
        m_running = false;
    }
    [[nodiscard]] bool waitForFinished(int) override { return !m_running; }
    void kill() override { m_running = false; }

private:
    std::shared_ptr<RuntimeTrace> m_trace;
    bool m_running = false;
};

class RuntimeAudioSink final : public platform::AudioSinkBackend
{
public:
    explicit RuntimeAudioSink(std::shared_ptr<RuntimeTrace> trace)
        : m_trace(std::move(trace))
    {
    }

    [[nodiscard]] std::vector<platform::AudioOutputDevice> devices() override
    {
        ++m_trace->audioDeviceEnumerations;
        return {{"audio:test", "Runtime test output", true}};
    }

    [[nodiscard]] platform::AudioSinkOpenResult open(
        const std::string&, std::uint32_t) override
    {
        m_trace->audioOpenThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
        return {};
    }

    [[nodiscard]] platform::AudioSinkOpenResult startPlayback() override
    {
        return {};
    }

    void close() noexcept override
    {
        m_trace->audioCloseThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
    }

    [[nodiscard]] std::size_t writableFrames() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t bufferedFrames() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t write(
        std::span<const std::byte> bytes) override
    {
        return bytes.size();
    }

    [[nodiscard]] std::uint64_t takePlatformUnderrunEvents() override
    {
        return 0;
    }

    [[nodiscard]] std::optional<std::string> takeRuntimeError() override
    {
        return std::nullopt;
    }

private:
    std::shared_ptr<RuntimeTrace> m_trace;
};

devices::DeviceCapabilities testCapabilities()
{
    return {
        .receive = true,
        .rtlSdrBlogV4 = false,
        .driverManagedHfBelow27Mhz = false,
        .receiveFrequencyRanges = {{88'000'000, 108'000'000}},
        .hfLimitation = {},
        .ppmCorrectionSupported = true,
        .rtlSdrTestModeSupported = true,
        .receiveSampleRateRanges = {{200'000, 3'200'000}},
        .gainSupported = true,
        .minimumGainDb = 0.0,
        .maximumGainDb = 49.0,
        .gainStepDb = 2.0,
        .complexFloat32StreamingSupported = true,
    };
}

class RuntimeMockSession final : public devices::DeviceSession
{
public:
    explicit RuntimeMockSession(devices::DeviceCapabilities capabilities)
        : m_capabilities(std::move(capabilities))
    {
    }

    [[nodiscard]] const devices::DeviceCapabilities& capabilities()
        const noexcept override
    {
        return m_capabilities;
    }

    [[nodiscard]] devices::DeviceOperationResult tuneCenterFrequency(
        std::uint64_t, devices::HfTuningMode) override
    {
        return {devices::DeviceError::None, true, "Mock device tuned"};
    }

    [[nodiscard]] devices::DeviceOperationResult setPpmCorrection(double) override
    {
        return {devices::DeviceError::None, true, "Mock PPM applied"};
    }

    [[nodiscard]] devices::DeviceOperationResult setSampleRate(
        std::uint64_t) override
    {
        return {devices::DeviceError::None, true, "Mock sample rate applied"};
    }

    [[nodiscard]] devices::DeviceOperationResult setGain(double) override
    {
        return {devices::DeviceError::None, true, "Mock gain applied"};
    }

    [[nodiscard]] devices::DeviceOperationResult startReceiveStream() override
    {
        return {devices::DeviceError::None, true, "Mock stream started"};
    }

    [[nodiscard]] devices::DeviceOperationResult stopReceiveStream() override
    {
        return {devices::DeviceError::None, true, "Mock stream stopped"};
    }

    [[nodiscard]] devices::DeviceReadResult readReceiveSamples(
        std::span<std::complex<float>>,
        std::chrono::milliseconds) override
    {
        return {devices::DeviceReadStatus::Timeout, 0, {}};
    }

private:
    devices::DeviceCapabilities m_capabilities;
};

class RuntimeMockProvider final : public devices::DeviceProvider
{
public:
    explicit RuntimeMockProvider(std::shared_ptr<RuntimeTrace> trace)
        : m_trace(std::move(trace))
    {
    }

    [[nodiscard]] devices::DeviceDiscoveryResult discover() override
    {
        ++m_trace->discoveries;
        m_trace->discoveryThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
        std::vector<devices::DeviceDescriptor> devices;
        if (!m_trace->noDevices) {
            auto capabilities = testCapabilities();
            if (m_trace->unconfirmedPpmCapabilities) {
                capabilities.ppmCorrectionSupported = false;
                capabilities.rtlSdrTestModeSupported = false;
            }
            if (!m_trace->firstDeviceMissing) {
                devices.push_back(
                    {"mock:serial=one",
                     true,
                     "First SDR",
                     m_trace->discoveryDriver,
                     "test",
                     "one",
                     capabilities});
            }
            devices.push_back(
                {"mock:serial=two",
                 true,
                 "Second SDR",
                 m_trace->discoveryDriver,
                 "test",
                 "two",
                 capabilities});
        }
        return {devices::DeviceError::None, std::move(devices), "Mock devices found"};
    }

    [[nodiscard]] devices::DeviceOpenResult open(
        const std::string& identifier) override
    {
        m_trace->openThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
        m_trace->openedIdentifiers.push_back(identifier);
        if (m_trace->failOpen) {
            return {
                devices::DeviceError::DeviceOpenFailed,
                nullptr,
                "Mock device open failed",
            };
        }
        return {
            devices::DeviceError::None,
            std::make_unique<RuntimeMockSession>(testCapabilities()),
            "Mock device opened",
        };
    }

private:
    std::shared_ptr<RuntimeTrace> m_trace;
};

class TrackingBackend final : public radio::ReceiverBackend
{
public:
    explicit TrackingBackend(std::shared_ptr<RuntimeTrace> trace)
        : m_trace(std::move(trace))
        , m_delegate({.ppmCorrectionSupported = true,
                      .startSucceeds = !m_trace->failStart})
    {
        m_capabilities = m_delegate.capabilities();
        m_capabilities.automaticPpmCalibrationSupported = true;
    }

    ~TrackingBackend() override
    {
        ++m_trace->backendDestructions;
        m_trace->destructionThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
    }

    [[nodiscard]] const radio::ReceiverLimits& limits() const noexcept override
    {
        return m_delegate.limits();
    }

    [[nodiscard]] const radio::ReceiverCapabilities& capabilities()
        const noexcept override
    {
        return m_capabilities;
    }

    [[nodiscard]] const radio::ReceiverState& state() const noexcept override
    {
        return m_delegate.state();
    }

    [[nodiscard]] std::optional<double> squelchSignalStrengthDb()
        const noexcept override
    {
        return m_trace->squelchSignalStrengthDb.has_value() &&
                       m_delegate.state().running
                   ? m_trace->squelchSignalStrengthDb
                   : std::nullopt;
    }

    [[nodiscard]] bool squelchOpen() const noexcept override
    {
        return m_trace->recordingSquelchOpen.load();
    }

    [[nodiscard]] std::uint64_t effectiveSampleRate() const noexcept override
    {
        return m_trace->effectiveSampleRate;
    }

    [[nodiscard]] std::uint64_t tuningGeneration() const noexcept override
    {
        return m_delegate.tuningGeneration();
    }

    [[nodiscard]] radio::SpectrumProcessingMetrics spectrumProcessingMetrics()
        const override
    {
        return m_delegate.spectrumProcessingMetrics();
    }
    [[nodiscard]] std::size_t spectrumFftSize() const noexcept override
    {
        return m_delegate.spectrumFftSize();
    }
    [[nodiscard]] std::size_t requestedSpectrumFftSize() const noexcept override
    {
        return m_requestedSpectrumFftSize;
    }
    [[nodiscard]] radio::OperationResult setSpectrumFftSize(
        std::size_t fftSize) override
    {
        const std::size_t effectiveFftSize =
            m_trace->maximumSpectrumFftSize != 0 &&
                    fftSize > m_trace->maximumSpectrumFftSize
                ? m_trace->maximumSpectrumFftSize
                : fftSize;
        auto result = m_delegate.setSpectrumFftSize(effectiveFftSize);
        if (result.succeeded()) {
            m_requestedSpectrumFftSize = fftSize;
        }
        if (result.succeeded() && effectiveFftSize != fftSize) {
            result.stateChanged = true;
            result.adjusted = true;
            result.message = "Simulated FFT allocation fallback";
        }
        return result;
    }
    [[nodiscard]] std::uint32_t spectrumFramesPerSecond() const noexcept override
    {
        return m_delegate.spectrumFramesPerSecond();
    }
    [[nodiscard]] radio::OperationResult setSpectrumFramesPerSecond(
        std::uint32_t framesPerSecond) override
    {
        return m_delegate.setSpectrumFramesPerSecond(framesPerSecond);
    }

    [[nodiscard]] std::optional<radio::SpectrumFrame>
    takeLatestSpectrumFrame() override
    {
        if (m_trace->calibrationActive) {
            ++m_trace->calibrationSpectrumReads;
        }
        return m_delegate.takeLatestSpectrumFrame();
    }

    [[nodiscard]] std::vector<float> takeAudioSamples(
        std::size_t maximumSamples) override
    {
        std::lock_guard lock(m_trace->recordingAudioMutex);
        if (m_trace->calibrationActive) {
            ++m_trace->calibrationAudioReads;
        }
        const std::size_t count = std::min(
            maximumSamples, m_trace->recordingAudioSamples.size());
        std::vector<float> result(
            m_trace->recordingAudioSamples.begin(),
            m_trace->recordingAudioSamples.begin() +
                static_cast<std::ptrdiff_t>(count));
        m_trace->recordingAudioSamples.erase(
            m_trace->recordingAudioSamples.begin(),
            m_trace->recordingAudioSamples.begin() +
                static_cast<std::ptrdiff_t>(count));
        return result;
    }

    [[nodiscard]] std::vector<float> takeDecoderInputSamples(
        std::size_t) override
    {
        if (m_trace->calibrationActive) {
            ++m_trace->calibrationDecoderReads;
        }
        return {};
    }

    void clearAudioSamples() override
    {
        ++m_trace->audioFlushes;
    }

    void setFullBandwidthIqCaptureEnabled(bool enabled) override
    {
        m_iqCaptureEnabled = enabled;
        if (!enabled) clearFullBandwidthIqSamples();
    }

    [[nodiscard]] std::vector<std::complex<float>> takeFullBandwidthIqSamples(
        std::size_t maximumSamples) override
    {
        if (!m_iqCaptureEnabled) return {};
        std::lock_guard lock(m_trace->recordingIqMutex);
        const std::size_t count = std::min(maximumSamples,
            m_trace->recordingIqSamples.size());
        std::vector<std::complex<float>> result(
            m_trace->recordingIqSamples.begin(),
            m_trace->recordingIqSamples.begin() + static_cast<std::ptrdiff_t>(count));
        m_trace->recordingIqSamples.erase(m_trace->recordingIqSamples.begin(),
            m_trace->recordingIqSamples.begin() + static_cast<std::ptrdiff_t>(count));
        return result;
    }

    void clearFullBandwidthIqSamples() override
    {
        std::lock_guard lock(m_trace->recordingIqMutex);
        m_trace->recordingIqSamples.clear();
    }

    [[nodiscard]] std::optional<radio::OperationResult> takeRuntimeError() override
    {
        if (!m_trace->runtimeFailureAfterStart ||
            m_trace->runtimeFailureReported || !m_delegate.state().running) {
            return std::nullopt;
        }
        m_trace->runtimeFailureReported = true;
        static_cast<void>(m_delegate.stopReception());
        return radio::OperationResult{
            radio::ReceiverError::BackendFailure,
            true,
            false,
            "Simulated SDR disconnection",
        };
    }

    [[nodiscard]] radio::OperationResult startReception() override
    {
        m_trace->startThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
        return m_delegate.startReception();
    }

    [[nodiscard]] radio::OperationResult stopReception() override
    {
        ++m_trace->backendStops;
        m_trace->stopThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
        return m_delegate.stopReception();
    }

    [[nodiscard]] radio::OperationResult setCenterFrequency(
        std::uint64_t value) override
    {
        m_trace->receiverOperationOrder.push_back("tune");
        m_trace->requestedCenterFrequencies.push_back(value);
        return m_delegate.setCenterFrequency(value);
    }
    [[nodiscard]] radio::OperationResult setListeningFrequency(
        std::uint64_t value) override
    {
        m_trace->receiverOperationOrder.push_back("listen");
        m_trace->requestedListeningFrequencies.push_back(value);
        return m_delegate.setListeningFrequency(value);
    }
    [[nodiscard]] radio::OperationResult tuneListeningFrequency(
        double value) override
    {
        return m_delegate.tuneListeningFrequency(value);
    }
    [[nodiscard]] radio::OperationResult shiftCenterFrequency(
        std::int64_t value) override
    {
        return m_delegate.shiftCenterFrequency(value);
    }
    [[nodiscard]] radio::OperationResult setSampleRate(
        std::uint64_t value) override
    {
        m_trace->requestedSampleRates.push_back(value);
        return m_delegate.setSampleRate(value);
    }
    [[nodiscard]] radio::OperationResult setFilterWidth(
        std::uint64_t value) override
    {
        ++m_trace->filterApplications;
        return m_delegate.setFilterWidth(value);
    }
    [[nodiscard]] radio::OperationResult setGain(double value) override
    {
        m_trace->requestedGains.push_back(value);
        return m_delegate.setGain(value);
    }
    [[nodiscard]] radio::OperationResult setPpmCorrection(double value) override
    {
        m_trace->receiverOperationOrder.push_back("ppm");
        m_trace->requestedPpmCorrections.push_back(value);
        if (m_trace->failPpmCorrection) {
            return {
                radio::ReceiverError::BackendFailure,
                false,
                false,
                "Simulated PPM write failure",
            };
        }
        const auto result = m_delegate.setPpmCorrection(value);
        if (result.succeeded() && !m_trace->calibrationActive &&
            m_trace->effectiveManualPpmCorrection.has_value()) {
            return m_delegate.setPpmCorrection(
                *m_trace->effectiveManualPpmCorrection);
        }
        return result;
    }
    [[nodiscard]] radio::OperationResult setDemodulationMode(
        radio::DemodulationMode value) override
    {
        ++m_trace->modeApplications;
        return m_delegate.setDemodulationMode(value);
    }
    [[nodiscard]] radio::OperationResult setSquelchLevel(double value) override
    {
        ++m_trace->squelchLevelApplications;
        return m_delegate.setSquelchLevel(value);
    }
    [[nodiscard]] radio::OperationResult enableManualSquelch() override
    {
        ++m_trace->manualSquelchApplications;
        return m_delegate.enableManualSquelch();
    }
    [[nodiscard]] radio::OperationResult disableSquelch() override
    {
        ++m_trace->disabledSquelchApplications;
        return m_delegate.disableSquelch();
    }

    [[nodiscard]] radio::OperationResult beginPpmCalibration() override
    {
        ++m_trace->calibrationBegins;
        m_trace->calibrationActive = true;
        return {
            radio::ReceiverError::None,
            true,
            false,
            "Mock calibration started",
        };
    }

    [[nodiscard]] radio::PpmCalibrationReadResult readPpmCalibrationBytes(
        std::span<std::uint8_t> bytes,
        std::chrono::milliseconds) override
    {
        if (m_trace->failCalibrationRead) {
            return {
                radio::PpmCalibrationReadStatus::Disconnected,
                0,
                false,
                "Simulated calibration disconnect",
            };
        }
        if (m_trace->holdCalibrationReads) {
            return {
                radio::PpmCalibrationReadStatus::Timeout,
                0,
                false,
                {},
            };
        }
        for (auto& byte : bytes) {
            byte = m_nextCalibrationCounterByte++;
        }
        const std::uint64_t complexSamples =
            static_cast<std::uint64_t>(bytes.size() / 2);
        const long double measuredRate =
            static_cast<long double>(m_delegate.state().sampleRate) *
            (1.0L +
             static_cast<long double>(m_trace->measuredCalibrationPpm) /
                 1'000'000.0L);
        m_trace->monotonicNanoseconds +=
            static_cast<std::uint64_t>(std::llround(
                static_cast<long double>(complexSamples) *
                1'000'000'000.0L / measuredRate));
        return {
            radio::PpmCalibrationReadStatus::Bytes,
            bytes.size(),
            false,
            {},
        };
    }

    [[nodiscard]] radio::OperationResult endPpmCalibration() override
    {
        if (m_trace->calibrationActive) {
            ++m_trace->calibrationEnds;
        }
        m_trace->calibrationActive = false;
        return {
            radio::ReceiverError::None,
            true,
            false,
            "Mock calibration stopped",
        };
    }

    [[nodiscard]] radio::OperationResult
    resumeReceptionAfterPpmCalibration() override
    {
        ++m_trace->calibrationResumes;
        return {
            radio::ReceiverError::None,
            false,
            false,
            "Mock reception restored",
        };
    }

private:
    std::shared_ptr<RuntimeTrace> m_trace;
    radio::MockReceiverBackend m_delegate;
    radio::ReceiverCapabilities m_capabilities;
    std::size_t m_requestedSpectrumFftSize = 4'096;
    bool m_iqCaptureEnabled = false;
    std::uint8_t m_nextCalibrationCounterByte = 0;
};

sdr::app::ReceiverRuntime::Factories factoriesFor(
    const std::shared_ptr<RuntimeTrace>& trace)
{
    return {
        .createDeviceProvider = [trace] {
            return std::make_unique<RuntimeMockProvider>(trace);
        },
        .createHardwareBackend = [trace](
                                     std::unique_ptr<devices::DeviceController>
                                         selectedDevice) {
            static_cast<void>(selectedDevice);
            ++trace->backendCreations;
            return std::make_unique<TrackingBackend>(trace);
        },
        .createAudioOutputService = [trace] {
            trace->audioServiceThreadToken = reinterpret_cast<quintptr>(
                QThread::currentThreadId());
            return std::make_unique<platform::AudioOutputService>(
                std::make_unique<RuntimeAudioSink>(trace));
        },
        .createDsdFmeProcessService = [trace] {
            return std::make_unique<platform::DsdFmeProcessService>(
                std::make_unique<RuntimeDsdChild>(trace));
        },
        .monotonicClock = [trace] {
            return trace->monotonicNanoseconds;
        },
    };
}

sdr::app::ReceiverRuntimeSnapshot latestSnapshot(const QSignalSpy& spy)
{
    return qvariant_cast<sdr::app::ReceiverRuntimeSnapshot>(
        spy.last().at(0));
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMilliseconds = 2'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

}  // namespace

class ReceiverRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void refreshesAtStartupSelectsFirstWithoutOpeningOrStarting();
    void showsNoDeviceStateAfterStartupDiscovery();
    void startsUsingTheAutomaticallySelectedDevice();
    void startsDeliberatelyInMockMode();
    void manualSelectionOverridesTheDefaultAndRefreshPreservesIt();
    void refreshHandlesSelectedDeviceDisappearanceWithoutDuplicates();
    void selectedDeviceDisappearanceDuringStartStopsHonestly();
    void reportsDeviceOpenAndStartFailuresWithoutOptimisticState();
    void exposesRuntimeFailuresThroughTheApplicationModel();
    void keepsAudioDeviceEnumerationOffTheRealtimeTimer();
    void exposesEffectiveSampleRateSeparatelyFromTheRequestedRate();
    void changesCaptureBandwidthWhileRunningWithoutManualRestart();
    void persistsValidCaptureBandwidthAndReplacesUnsupportedSavedValue();
    void changesSpectrumFftSizeWhileRunning();
    void reportsRequestedAndEffectiveSpectrumFftFallback();
    void validatesPersistedSpectrumFftSize();
    void keepsSpectrumCadenceStableAcrossVisibleHistoryChanges();
    void presentsWaterfallFromTheCurrentSpectrumStreamWithinOneLiveInterval();
    void persistsAndRestoresReceiverAndSquelchControls();
    void measuresOneShotSquelchForBoundedWindowAndKeepsItManual();
    void scannerRetunesStayFocusedResponsiveAndBounded();
    void wideRangeBlockRetunesAvoidGlobalReceiverReconfiguration();
    void migratesLegacyDemodulatorOrdinalToStableId();
    void rejectsInvalidPersistedReceiverControls();
    void defaultsGainToTwentyDbWithoutPersistingIt();
    void usesNearestSupportedGainWithoutOverwritingSavedPreference();
    void restoresPersistedValidGain();
    void coalescesRapidSpectrumWheelRetunesToTheFinalCenter();
    void recentersRunningReceiverAndFlushesStaleAudio();
    void remembersFilterWidthPerMode();
    void appliesBookmarkAsOneAsynchronousLiveOperation();
    void appliesBookmarkScannerTransitionsDifferentially();
    void managesDsdFmeLifecycleWithoutRestartingOnRetune();
    void keepsReceptionAndDisplayActiveWhenDsdFmeFails();
    void enablesAutoPpmForCanonicalRtlSdrDiscoveryKeyBeforeOpen();
    void cancellationPreservesPreviousPpmAndRunningReception();
    void failurePreservesPreviousPpm();
    void manualPpmReplacesSavedAutomaticCorrection();
    void manualPpmPersistsDriverReadbackAndSurvivesReopen();
    void manualPpmApplyFailurePreservesSavedCorrection();
    void successfulCalibrationPersistsBySerialAndAppliesBeforeTune();
    void stoppedReceptionRemainsStoppedAfterCalibration();
    void testCounterDataNeverReachesReceiverProcessing();
    void stopsBackendAndJoinsWorkerDuringShutdown();
    void recordsMutedAudioAcrossRetunesAndFinalizesOnStop();
    void armsQuietSkippingRecordingAcrossScannerAndRetunes();
    void recordsScannerActivityWithSidecarAlongsideManualRecording();
    void recordsIqAcrossScannerAndSegmentsOnlyCaptureChanges();

private:
    QTemporaryDir m_settingsDirectory;
    QByteArray m_originalXdgConfigHome;
    bool m_hadXdgConfigHome = false;
};

void ReceiverRuntimeTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    m_hadXdgConfigHome = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    m_originalXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", m_settingsDirectory.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("sdrapp-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        m_settingsDirectory.path());
    QVERIFY(QDir().mkpath(
        m_settingsDirectory.path() + QStringLiteral("/sdrapp-tests")));
}

void ReceiverRuntimeTest::cleanupTestCase()
{
    if (m_hadXdgConfigHome) {
        qputenv("XDG_CONFIG_HOME", m_originalXdgConfigHome);
    } else {
        qunsetenv("XDG_CONFIG_HOME");
    }
}

void ReceiverRuntimeTest::init()
{
    QSettings settings;
    settings.setValue(
        QStringLiteral("receiver/captureBandwidthSamplesPerSecond"),
        quint64{2'000'000});
    settings.remove(QStringLiteral("receiver/gainDb"));
    settings.remove(QStringLiteral("receiver/centerFrequencyHz"));
    settings.remove(QStringLiteral("receiver/listeningFrequencyHz"));
    settings.remove(QStringLiteral("receiver/demodulationMode"));
    settings.remove(QStringLiteral("receiver/squelchThresholdDb"));
    settings.remove(QStringLiteral("receiver/squelchDisabled"));
    settings.remove(QStringLiteral("spectrum/fftSize"));
    settings.remove(QStringLiteral("waterfall/rowsPerSecond"));
    settings.remove(QStringLiteral("waterfall/visibleHistorySeconds"));
    settings.remove(QStringLiteral("externalDecoder/dsdFmeBinaryPath"));
    settings.remove(QStringLiteral("receiver/ppmByDevice"));
    settings.sync();
}

void ReceiverRuntimeTest::cleanup()
{
    QSettings settings;
    settings.remove(QStringLiteral("receiver/captureBandwidthSamplesPerSecond"));
    settings.remove(QStringLiteral("receiver/gainDb"));
    settings.remove(QStringLiteral("receiver/centerFrequencyHz"));
    settings.remove(QStringLiteral("receiver/listeningFrequencyHz"));
    settings.remove(QStringLiteral("receiver/demodulationMode"));
    settings.remove(QStringLiteral("receiver/squelchThresholdDb"));
    settings.remove(QStringLiteral("receiver/squelchDisabled"));
    settings.remove(QStringLiteral("spectrum/fftSize"));
    settings.remove(QStringLiteral("waterfall/rowsPerSecond"));
    settings.remove(QStringLiteral("waterfall/visibleHistorySeconds"));
    settings.remove(QStringLiteral("externalDecoder/dsdFmeBinaryPath"));
    settings.remove(QStringLiteral("receiver/ppmByDevice"));
    settings.sync();
}

void ReceiverRuntimeTest::changesSpectrumFftSizeWhileRunning()
{
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.spectrumFftSize() == 4'096; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.spectrumFftSize(), quint64{4'096});

    model.setSpectrumFftSize(262'144);
    QVERIFY(waitUntil([&model] { return model.spectrumFftSize() == 262'144; }));
    QCOMPARE(model.effectiveSpectrumFftSize(), quint64{262'144});
    QVERIFY(model.receiverRunning());
    QVERIFY(spectrumResets.count() >= 1);
    QCOMPARE(waterfallResets.count(), 0);
    runtime.shutdown();
    QSettings settings;
    settings.sync();
    QCOMPARE(
        settings.value(QStringLiteral("spectrum/fftSize")).toULongLong(),
        qulonglong{262'144});
}

void ReceiverRuntimeTest::reportsRequestedAndEffectiveSpectrumFftFallback()
{
    auto trace = std::make_shared<RuntimeTrace>();
    trace->maximumSpectrumFftSize = 65'536;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.backendReady(); }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    model.setSpectrumFftSize(262'144);
    QVERIFY(waitUntil([&model] {
        return model.spectrumFftSize() == 262'144 &&
               model.effectiveSpectrumFftSize() == 65'536;
    }));
    QVERIFY(model.receiverRunning());
    QVERIFY2(
        model.statusText().contains(QStringLiteral("requested: 262144")),
        qPrintable(model.statusText()));
    QVERIFY2(
        model.statusText().contains(QStringLiteral("effective: 65536")),
        qPrintable(model.statusText()));
    runtime.shutdown();
}

void ReceiverRuntimeTest::validatesPersistedSpectrumFftSize()
{
    QSettings settings;
    settings.setValue(QStringLiteral("spectrum/fftSize"), QStringLiteral("12345"));
    settings.sync();
    {
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Mock);
        ApplicationModel model(runtime);
        runtime.start();
        QVERIFY(waitUntil([&model] { return model.backendReady(); }));
        QCOMPARE(model.spectrumFftSize(), quint64{4'096});
        QCOMPARE(model.effectiveSpectrumFftSize(), quint64{4'096});
        runtime.shutdown();
    }

    settings.setValue(
        QStringLiteral("spectrum/fftSize"), QStringLiteral("131072"));
    settings.sync();
    {
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Mock);
        ApplicationModel model(runtime);
        runtime.start();
        QVERIFY(waitUntil([&model] {
            return model.spectrumFftSize() == 131'072;
        }));
        QCOMPARE(model.effectiveSpectrumFftSize(), quint64{131'072});
        runtime.shutdown();
    }
}

void ReceiverRuntimeTest::keepsSpectrumCadenceStableAcrossVisibleHistoryChanges()
{
    QSettings obsoleteSettings;
    obsoleteSettings.setValue(QStringLiteral("waterfall/rowsPerSecond"), 120U);
    obsoleteSettings.sync();
    {
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Mock);
        ApplicationModel model(runtime);
        QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
        QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
        QSignalSpy waterfallFrames(&model, &ApplicationModel::waterfallFrameReady);
        runtime.start();
        QVERIFY(waitUntil([&model] { return model.backendReady(); }));
        QCOMPARE(model.visibleWaterfallHistorySeconds(), 10.0);
        QVERIFY(model.visibleWaterfallHistoryOptions().contains(
            QStringLiteral("1 s")));
        QVERIFY(model.visibleWaterfallHistoryOptions().contains(
            QStringLiteral("2.5 s")));
        QCOMPARE(model.effectiveWaterfallRowsPerSecond(), 60.0);
        model.startReception();
        QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

        QSignalSpy receiverRunningChanges(
            &model, &ApplicationModel::receiverRunningChanged);
        QSignalSpy fftChanges(&model, &ApplicationModel::spectrumFftSizeChanged);
        for (const double seconds : {30.0, 1.0, 2.5, 5.0, 60.0, 2.5}) {
            spectrumFrames.clear();
            waterfallFrames.clear();
            model.setVisibleWaterfallHistorySeconds(seconds);
            QVERIFY(waitUntil([&model, seconds] {
                return model.visibleWaterfallHistorySeconds() == seconds &&
                       model.effectiveWaterfallRowsPerSecond() == 60.0;
            }));
            QVERIFY(waitUntil([&spectrumFrames] {
                return spectrumFrames.count() >= 1;
            }));
            QVERIFY(waitUntil([&waterfallFrames] {
                return waterfallFrames.count() >= 1;
            }));
            for (const auto& arguments : spectrumFrames) {
                QVERIFY(arguments.front().value<QVector<float>>().size() >= 2);
            }
        }
        QVERIFY(model.receiverRunning());
        QCOMPARE(waterfallResets.count(), 0);
        QCOMPARE(receiverRunningChanges.count(), 0);
        QCOMPARE(fftChanges.count(), 0);
        runtime.shutdown();
    }

    QSettings persistedSettings;
    persistedSettings.sync();
    QVERIFY(!persistedSettings.contains(QStringLiteral("waterfall/rowsPerSecond")));
    QCOMPARE(persistedSettings.value(
                 QStringLiteral("waterfall/visibleHistorySeconds")).toDouble(),
             2.5);

    sdr::app::ReceiverRuntime restored(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel restoredModel(restored);
    restored.start();
    QVERIFY2(waitUntil([&restoredModel] {
        return restoredModel.visibleWaterfallHistorySeconds() == 2.5 &&
               restoredModel.effectiveWaterfallRowsPerSecond() == 60.0;
    }), qPrintable(QStringLiteral("restored source cadence=%1 history=%2")
                       .arg(restoredModel.effectiveWaterfallRowsPerSecond())
                       .arg(restoredModel.visibleWaterfallHistorySeconds())));
    restored.shutdown();
}

void ReceiverRuntimeTest::
    presentsWaterfallFromTheCurrentSpectrumStreamWithinOneLiveInterval()
{
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    QSignalSpy waterfallFrames(&model, &ApplicationModel::waterfallFrameReady);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.backendReady(); }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    spectrumFrames.clear();
    waterfallFrames.clear();
    QElapsedTimer latency;
    latency.start();
    QVERIFY(waitUntil([&spectrumFrames] {
        return spectrumFrames.count() >= 1;
    }));
    const qint64 spectrumPresentedMilliseconds = latency.elapsed();
    QVERIFY(waitUntil([&waterfallFrames] {
        return waterfallFrames.count() >= 1;
    }));
    const qint64 relativePresentationMilliseconds =
        latency.elapsed() - spectrumPresentedMilliseconds;
    QVERIFY2(
        relativePresentationMilliseconds <= 120,
        qPrintable(QStringLiteral(
            "spectrum-to-waterfall presentation latency was %1 ms")
                       .arg(relativePresentationMilliseconds)));

    const auto waterfall = waterfallFrames.first();
    const quint64 waterfallSequence = waterfall.at(4).toULongLong();
    const quint64 waterfallTimestamp = waterfall.at(5).toULongLong();
    const auto matchingSpectrum = std::ranges::find_if(
        spectrumFrames,
        [waterfallSequence, waterfallTimestamp](const QList<QVariant>& frame) {
            return frame.at(4).toULongLong() == waterfallSequence &&
                   frame.at(5).toULongLong() == waterfallTimestamp;
        });
    QVERIFY2(
        matchingSpectrum != spectrumFrames.end(),
        "The waterfall row was not derived from a spectrum-presented FFT frame");
    runtime.shutdown();
}

void ReceiverRuntimeTest::refreshesAtStartupSelectsFirstWithoutOpeningOrStarting()
{
    auto trace = std::make_shared<RuntimeTrace>();
    const quintptr guiThreadToken = reinterpret_cast<quintptr>(
        QThread::currentThreadId());
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    QSignalSpy snapshots(&runtime, &sdr::app::ReceiverRuntime::snapshotChanged);

    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    const auto snapshot = latestSnapshot(snapshots);
    QVERIFY(runtime.workerThreadRunning());
    QVERIFY(snapshot.workerThreadToken != guiThreadToken);
    QVERIFY(trace->audioServiceThreadToken != guiThreadToken);
    QVERIFY(snapshot.audioReady);
    QVERIFY(snapshot.backendReady);
    QVERIFY(!snapshot.receiverState.running);
    QCOMPARE(model.selectedDeviceIndex(), 0);
    QVERIFY(model.statusText().contains(QStringLiteral("Found")));
    QCOMPARE(trace->discoveries, 1);
    QVERIFY(trace->openedIdentifiers.empty());

    runtime.shutdown();
    QVERIFY(!runtime.workerThreadRunning());
}

void ReceiverRuntimeTest::showsNoDeviceStateAfterStartupDiscovery()
{
    auto trace = std::make_shared<RuntimeTrace>();
    trace->noDevices = true;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();

    QVERIFY(waitUntil([&model] {
        return model.statusText().contains(QStringLiteral("No SDR devices found"));
    }));
    QVERIFY(!model.backendReady());
    QCOMPARE(model.selectedDeviceIndex(), -1);
    QVERIFY(!model.receiverRunning());
    QVERIFY(trace->openedIdentifiers.empty());
}

void ReceiverRuntimeTest::startsUsingTheAutomaticallySelectedDevice()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    QVERIFY(trace->openedIdentifiers.empty());

    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(trace->openedIdentifiers, std::vector<std::string>{"mock:serial=one"});
}

void ReceiverRuntimeTest::startsDeliberatelyInMockMode()
{
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);

    runtime.start();
    QVERIFY(waitUntil([&model] { return model.backendReady(); }));
    QVERIFY(model.mockMode());
    QVERIFY(!model.deviceDiscoveryAvailable());
    QCOMPARE(model.deviceState(), QStringLiteral("Mock device"));

    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    runtime.shutdown();
    QVERIFY(!runtime.workerThreadRunning());
}

void ReceiverRuntimeTest::manualSelectionOverridesTheDefaultAndRefreshPreservesIt()
{
    auto trace = std::make_shared<RuntimeTrace>();
    const quintptr guiThreadToken = reinterpret_cast<quintptr>(
        QThread::currentThreadId());
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    QSignalSpy snapshots(&runtime, &sdr::app::ReceiverRuntime::snapshotChanged);
    runtime.start();
    QVERIFY(waitUntil([&snapshots] { return !snapshots.empty(); }));

    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    QVERIFY(trace->openedIdentifiers.empty());
    QVERIFY(trace->discoveryThreadToken != guiThreadToken);

    model.selectDeviceIndex(1);
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 1; }));
    QVERIFY(trace->openedIdentifiers.empty());
    QCOMPARE(model.selectedDeviceIndex(), 1);
    QVERIFY(!model.receiverRunning());
    QVERIFY(model.ppmCorrectionSupported());
    QVERIFY(model.gainSupported());
    QCOMPARE(model.minimumGain(), 0.0);
    QCOMPARE(model.maximumGain(), 49.0);
    QCOMPARE(model.gainStep(), 2.0);
    QCOMPARE(model.deviceSampleRateRanges().size(), std::size_t{1});
    QVERIFY(model.deviceCapabilitySummary().contains(QStringLiteral("Sample rate")));
    QCOMPARE(trace->backendCreations, 0);

    model.refreshDevices();
    QVERIFY(waitUntil([&trace] { return trace->discoveries >= 2; }));
    QCOMPARE(model.selectedDeviceIndex(), 1);
    QCOMPARE(model.deviceDisplayNames().size(), 2);

    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(trace->openedIdentifiers, std::vector<std::string>{"mock:serial=two"});
    QVERIFY(trace->openThreadToken != guiThreadToken);
    model.setCenterFrequencyText(QStringLiteral("200000000"));
    QVERIFY(waitUntil([&model] {
        return model.centerFrequency() == quint64{108'000'000};
    }));
    QVERIFY(trace->startThreadToken != guiThreadToken);
    model.setDemodulationModeIndex(
        static_cast<int>(radio::DemodulationMode::Nfm));
    QVERIFY(waitUntil([&trace] { return trace->audioFlushes == 1; }));
    model.stopReception();
    QVERIFY(waitUntil([&model] { return !model.receiverRunning(); }));
    model.clearDeviceSelection();
    QVERIFY(waitUntil([&model] { return !model.backendReady(); }));
    QCOMPARE(model.selectedDeviceIndex(), -1);
}

void ReceiverRuntimeTest::refreshHandlesSelectedDeviceDisappearanceWithoutDuplicates()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));

    model.refreshDevices();
    QVERIFY(waitUntil([&trace] { return trace->discoveries >= 2; }));
    QCOMPARE(model.deviceDisplayNames().size(), 2);
    QCOMPARE(model.selectedDeviceIndex(), 0);

    trace->firstDeviceMissing = true;
    model.refreshDevices();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 1; }));
    QCOMPARE(model.selectedDeviceIndex(), 0);
    QVERIFY(model.deviceDisplayNames().front().contains(QStringLiteral("Second SDR")));
    QVERIFY(!model.receiverRunning());
    QVERIFY(trace->openedIdentifiers.empty());
}

void ReceiverRuntimeTest::selectedDeviceDisappearanceDuringStartStopsHonestly()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    trace->firstDeviceMissing = true;

    model.startReception();
    QVERIFY(waitUntil([&model] {
        return model.statusText().contains(QStringLiteral("disappeared"));
    }));
    QVERIFY(!model.receiverRunning());
    QCOMPARE(model.selectedDeviceIndex(), 0);
    QVERIFY(model.deviceDisplayNames().front().contains(QStringLiteral("Second SDR")));
    QVERIFY(trace->openedIdentifiers.empty());
}

void ReceiverRuntimeTest::reportsDeviceOpenAndStartFailuresWithoutOptimisticState()
{
    auto openTrace = std::make_shared<RuntimeTrace>();
    openTrace->failOpen = true;
    sdr::app::ReceiverRuntime openRuntime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(openTrace));
    ApplicationModel openModel(openRuntime);
    openRuntime.start();
    openModel.refreshDevices();
    QVERIFY(waitUntil(
        [&openModel] { return openModel.deviceDisplayNames().size() == 2; }));
    openModel.selectDeviceIndex(0);
    openModel.startReception();
    QVERIFY(waitUntil([&openModel] {
        return openModel.statusText().contains(QStringLiteral("Opening the selected SDR failed"));
    }));
    QVERIFY(openModel.backendReady());
    QVERIFY(!openModel.receiverRunning());

    auto startTrace = std::make_shared<RuntimeTrace>();
    startTrace->failStart = true;
    sdr::app::ReceiverRuntime startRuntime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(startTrace));
    ApplicationModel startModel(startRuntime);
    startRuntime.start();
    startModel.refreshDevices();
    QVERIFY(waitUntil(
        [&startModel] { return startModel.deviceDisplayNames().size() == 2; }));
    startModel.selectDeviceIndex(0);
    QVERIFY(waitUntil([&startModel] { return startModel.backendReady(); }));
    startModel.startReception();
    QVERIFY(waitUntil([&startModel] {
        return startModel.statusText().contains(QStringLiteral("failed to start"));
    }));
    QVERIFY(!startModel.receiverRunning());
}

void ReceiverRuntimeTest::exposesRuntimeFailuresThroughTheApplicationModel()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    model.refreshDevices();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.selectDeviceIndex(0);
    QVERIFY(waitUntil([&model] { return model.backendReady(); }));

    trace->runtimeFailureAfterStart = true;
    model.startReception();
    QVERIFY(waitUntil([&model] {
        return model.statusText().contains(QStringLiteral("disconnection"));
    }));
    QVERIFY(!model.receiverRunning());
    QVERIFY(trace->runtimeFailureReported);
}

void ReceiverRuntimeTest::keepsAudioDeviceEnumerationOffTheRealtimeTimer()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    QSignalSpy snapshots(&runtime, &sdr::app::ReceiverRuntime::snapshotChanged);

    runtime.start();
    QVERIFY(waitUntil([&snapshots] { return !snapshots.empty(); }));
    QCOMPARE(trace->audioDeviceEnumerations, 1);

    QTest::qWait(250);
    QCOMPARE(trace->audioDeviceEnumerations, 1);
    runtime.shutdown();
}

void ReceiverRuntimeTest::exposesEffectiveSampleRateSeparatelyFromTheRequestedRate()
{
    auto trace = std::make_shared<RuntimeTrace>();
    trace->effectiveSampleRate = 2'400'000;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    QSignalSpy effectiveRateSpy(
        &model, &ApplicationModel::effectiveSampleRateChanged);

    runtime.start();
    model.refreshDevices();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.selectDeviceIndex(0);
    QVERIFY(waitUntil([&model] { return model.backendReady(); }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    QCOMPARE(model.sampleRate(), quint64{2'000'000});
    QCOMPARE(model.effectiveSampleRate(), quint64{2'400'000});
    QCOMPARE(model.visibleLowerFrequency(), quint64{98'800'000});
    QCOMPARE(model.visibleUpperFrequency(), quint64{101'200'000});
    QVERIFY(effectiveRateSpy.count() >= 1);
    runtime.shutdown();
}

void ReceiverRuntimeTest::changesCaptureBandwidthWhileRunningWithoutManualRestart()
{
    auto trace = std::make_shared<RuntimeTrace>();
    trace->effectiveSampleRate = 2'400'000;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    model.setSampleRate(1'500'000);
    QVERIFY(waitUntil([&model] {
        return model.receiverRunning() && model.sampleRate() == quint64{1'500'000};
    }));
    QCOMPARE(model.effectiveSampleRate(), quint64{2'400'000});
    const std::vector<std::uint64_t> expectedSampleRates{
        2'000'000,
        1'500'000,
    };
    QCOMPARE(trace->requestedSampleRates, expectedSampleRates);
    runtime.shutdown();
}

void ReceiverRuntimeTest::persistsValidCaptureBandwidthAndReplacesUnsupportedSavedValue()
{
    QSettings settings;
    settings.setValue(
        QStringLiteral("receiver/captureBandwidthSamplesPerSecond"),
        quint64{1'500'000});
    {
        auto trace = std::make_shared<RuntimeTrace>();
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Hardware,
            factoriesFor(trace));
        ApplicationModel model(runtime);
        runtime.start();
        QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
        QCOMPARE(model.sampleRate(), quint64{1'500'000});
        runtime.shutdown();
    }

    settings.setValue(
        QStringLiteral("receiver/captureBandwidthSamplesPerSecond"),
        quint64{8'000'000});
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    QCOMPARE(model.sampleRate(), quint64{2'000'000});
    QVERIFY(model.statusText().contains(QStringLiteral("unsupported")));
    runtime.shutdown();
}

void ReceiverRuntimeTest::defaultsGainToTwentyDbWithoutPersistingIt()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.gain(), 20.0);
    QVERIFY(!trace->requestedGains.empty());
    QCOMPARE(trace->requestedGains.front(), 20.0);
    QVERIFY(!QSettings().contains(QStringLiteral("receiver/gainDb")));
    runtime.shutdown();
}

void ReceiverRuntimeTest::measuresOneShotSquelchForBoundedWindowAndKeepsItManual()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] {
        return model.receiverRunning() && model.autoSquelchAvailable();
    }));

    const int applicationsBefore = trace->squelchLevelApplications;
    QElapsedTimer elapsed;
    elapsed.start();
    model.autoSquelch();
    QVERIFY(waitUntil([&model] {
        return model.statusText().startsWith(QStringLiteral("Squelch set to"));
    }, 2'000));
    QVERIFY(elapsed.elapsed() >= 350);
    QVERIFY(elapsed.elapsed() < 1'000);
    QCOMPARE(model.squelchLevel(), -52.0);
    QCOMPARE(trace->squelchLevelApplications, applicationsBefore + 1);
    QVERIFY(model.squelchStateText() == QStringLiteral("Manual"));
    QCOMPARE(
        QSettings().value(QStringLiteral("receiver/squelchThresholdDb"))
            .toDouble(),
        -52.0);

    QTest::qWait(150);
    QCOMPARE(model.squelchLevel(), -52.0);
    QCOMPARE(trace->squelchLevelApplications, applicationsBefore + 1);
    runtime.shutdown();
}

void ReceiverRuntimeTest::persistsAndRestoresReceiverAndSquelchControls()
{
    {
        auto trace = std::make_shared<RuntimeTrace>();
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Hardware,
            factoriesFor(trace));
        ApplicationModel model(runtime);
        runtime.start();
        QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
        model.startReception();
        QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

        model.setCenterFrequencyText(QStringLiteral("100200000"));
        QVERIFY(waitUntil([&model] {
            return model.centerFrequency() == quint64{100'200'000};
        }));
        model.setListeningFrequency(100'300'000);
        QVERIFY(waitUntil([&model] {
            return model.listeningFrequency() == quint64{100'300'000};
        }));
        model.setDemodulationModeIndex(
            static_cast<int>(radio::DemodulationMode::Usb));
        QVERIFY(waitUntil([&model] {
            return model.demodulationModeName() == QStringLiteral("USB");
        }));
        model.commitGain(21.0);
        QVERIFY(waitUntil([&model] { return model.requestedGain() == 21.0; }));
        model.setSquelchLevel(-62.0);
        QVERIFY(waitUntil([&model] { return model.squelchLevel() == -62.0; }));
        model.disableSquelch();
        QVERIFY(waitUntil([&model] { return model.squelchDisabled(); }));
        runtime.shutdown();
    }

    QSettings settings;
    settings.sync();
    QCOMPARE(
        settings.value(QStringLiteral("receiver/centerFrequencyHz")).toULongLong(),
        qulonglong{100'200'000});
    QCOMPARE(
        settings.value(QStringLiteral("receiver/listeningFrequencyHz")).toULongLong(),
        qulonglong{100'300'000});
    QCOMPARE(
        settings.value(QStringLiteral("receiver/demodulationMode")).toString(),
        QStringLiteral("usb"));
    QCOMPARE(
        settings.value(QStringLiteral("receiver/gainDb")).toDouble(), 21.0);
    QCOMPARE(
        settings.value(QStringLiteral("receiver/squelchThresholdDb")).toDouble(),
        -62.0);
    QVERIFY(settings.value(QStringLiteral("receiver/squelchDisabled")).toBool());

    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);

    // QML sees the saved controls as soon as its model is constructed, before
    // asynchronous discovery or backend initialization can publish defaults.
    QCOMPARE(model.centerFrequency(), quint64{100'200'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'300'000});
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.requestedGain(), 21.0);
    QCOMPARE(model.squelchLevel(), -62.0);
    QVERIFY(model.squelchDisabled());

    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    QCOMPARE(model.centerFrequency(), quint64{100'200'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'300'000});
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.requestedGain(), 21.0);
    QCOMPARE(model.squelchLevel(), -62.0);
    QVERIFY(model.squelchDisabled());

    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.centerFrequency(), quint64{100'200'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'300'000});
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.requestedGain(), 21.0);
    QCOMPARE(model.squelchLevel(), -62.0);
    QVERIFY(model.squelchDisabled());
    QVERIFY(!trace->requestedGains.empty());
    QCOMPARE(trace->requestedGains.front(), 22.0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::scannerRetunesStayFocusedResponsiveAndBounded()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    constexpr quint64 manuallySelectedFrequency = 100'050'000;
    model.setListeningFrequency(manuallySelectedFrequency);
    QVERIFY(waitUntil([&model] {
        return model.listeningFrequency() == manuallySelectedFrequency;
    }));
    QTest::qWait(50);

    QSettings settings;
    settings.sync();
    QCOMPARE(
        settings.value(QStringLiteral("receiver/listeningFrequencyHz"))
            .toULongLong(),
        qulonglong{manuallySelectedFrequency});

    QSignalSpy normalSpectrumFrames(
        &model, &ApplicationModel::spectrumFrameReady);
    QSignalSpy normalWaterfallFrames(
        &model, &ApplicationModel::waterfallFrameReady);
    QTest::qWait(120);
    const qsizetype normalSpectrumFrameCount = normalSpectrumFrames.count();
    const qsizetype normalWaterfallFrameCount = normalWaterfallFrames.count();
    QVERIFY(normalSpectrumFrameCount > 0);
    QVERIFY(normalWaterfallFrameCount > 0);

    const auto listeningRequestsBeforeScan =
        trace->requestedListeningFrequencies.size();
    const auto centerRequestsBeforeScan =
        trace->requestedCenterFrequencies.size();
    const auto sampleRateRequestsBeforeScan = trace->requestedSampleRates.size();
    const auto gainRequestsBeforeScan = trace->requestedGains.size();
    const auto ppmRequestsBeforeScan = trace->requestedPpmCorrections.size();
    const int filterApplicationsBeforeScan = trace->filterApplications;
    const int modeApplicationsBeforeScan = trace->modeApplications;
    const int squelchLevelApplicationsBeforeScan =
        trace->squelchLevelApplications;
    const int manualSquelchApplicationsBeforeScan =
        trace->manualSquelchApplications;
    const int disabledSquelchApplicationsBeforeScan =
        trace->disabledSquelchApplications;
    const int audioFlushesBeforeScan = trace->audioFlushes;

    QSignalSpy operationPending(
        &runtime, &sdr::app::ReceiverRuntime::operationPending);
    QSignalSpy runtimeBusyChanges(&model, &ApplicationModel::runtimeBusyChanged);
    QSignalSpy filterChanges(&model, &ApplicationModel::filterWidthChanged);
    QSignalSpy gainChanges(&model, &ApplicationModel::gainChanged);
    QSignalSpy requestedGainChanges(
        &model, &ApplicationModel::requestedGainChanged);
    QSignalSpy modeChanges(&model, &ApplicationModel::demodulationModeChanged);
    QSignalSpy squelchChanges(&model, &ApplicationModel::squelchStateChanged);
    QSignalSpy deviceChanges(&model, &ApplicationModel::deviceStateChanged);
    QSignalSpy capabilityChanges(
        &model, &ApplicationModel::deviceCapabilitiesChanged);
    QSignalSpy audioChanges(&model, &ApplicationModel::audioStateChanged);
    QSignalSpy statusChanges(&model, &ApplicationModel::statusTextChanged);
    QSignalSpy listeningChanges(
        &model, &ApplicationModel::listeningFrequencyChanged);
    QSignalSpy scannerChanges(&model, &ApplicationModel::scannerChanged);
    QSignalSpy scanFrequencyChanges(
        &model, &ApplicationModel::scanCurrentFrequencyChanged);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    QSignalSpy waterfallFrames(&model, &ApplicationModel::waterfallFrameReady);

    model.setScanLowerFrequency(100'000'000);
    model.setScanUpperFrequency(100'200'000);
    model.setScanStepSize(10'000);
    model.setScanDwellMilliseconds(50);
    model.setScanResumeDelayMilliseconds(20);
    scannerChanges.clear();
    scanFrequencyChanges.clear();
    trace->receiverOperationOrder.clear();
    model.startScan();
    QVERIFY(waitUntil([&trace, listeningRequestsBeforeScan] {
        return trace->requestedListeningFrequencies.size() >=
               listeningRequestsBeforeScan + 4;
    }));
    QCOMPARE(scannerChanges.count(), 2);
    QVERIFY(scanFrequencyChanges.count() >= 3);
    model.pauseOrResumeScan();
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));
    QVERIFY(listeningChanges.count() >= 2);
    QVERIFY(spectrumFrames.count() >= normalSpectrumFrameCount / 2);
    QVERIFY(waterfallFrames.count() >= normalWaterfallFrameCount / 2);

    QCOMPARE(
        trace->requestedCenterFrequencies.size(),
        centerRequestsBeforeScan + 1);
    QCOMPARE(trace->requestedCenterFrequencies.back(), quint64{100'100'000});
    QVERIFY(!trace->receiverOperationOrder.empty());
    QCOMPARE(trace->receiverOperationOrder.front(), std::string("tune"));
    QCOMPARE(model.centerFrequency(), quint64{100'100'000});
    QCOMPARE(trace->requestedSampleRates.size(), sampleRateRequestsBeforeScan);
    QCOMPARE(trace->requestedGains.size(), gainRequestsBeforeScan);
    QCOMPARE(trace->requestedPpmCorrections.size(), ppmRequestsBeforeScan);
    QCOMPARE(trace->filterApplications, filterApplicationsBeforeScan);
    QCOMPARE(trace->modeApplications, modeApplicationsBeforeScan);
    QCOMPARE(
        trace->squelchLevelApplications, squelchLevelApplicationsBeforeScan);
    QCOMPARE(
        trace->manualSquelchApplications, manualSquelchApplicationsBeforeScan);
    QCOMPARE(
        trace->disabledSquelchApplications,
        disabledSquelchApplicationsBeforeScan);
    QCOMPARE(trace->audioFlushes, audioFlushesBeforeScan + 1);
    QCOMPARE(operationPending.count(), 1);
    QCOMPARE(runtimeBusyChanges.count(), 2);
    QCOMPARE(filterChanges.count(), 0);
    QCOMPARE(gainChanges.count(), 0);
    QCOMPARE(requestedGainChanges.count(), 0);
    QCOMPARE(modeChanges.count(), 0);
    QCOMPARE(squelchChanges.count(), 0);
    QCOMPARE(deviceChanges.count(), 0);
    QCOMPARE(capabilityChanges.count(), 0);
    QCOMPARE(audioChanges.count(), 0);
    QVERIFY(statusChanges.count() <= 3);

    const auto centerRequestsWhilePaused =
        trace->requestedCenterFrequencies.size();
    const auto listeningRequestsWhilePaused =
        trace->requestedListeningFrequencies.size();
    model.setCenterFrequencyText(QStringLiteral("101000000"));
    model.shiftCenterFromSpectrum(120);
    model.setListeningFrequency(100'150'000);
    model.selectListeningFrequencyAt(25.0, 100.0);
    QTest::qWait(50);
    QCOMPARE(
        trace->requestedCenterFrequencies.size(), centerRequestsWhilePaused);
    QCOMPARE(
        trace->requestedListeningFrequencies.size(),
        listeningRequestsWhilePaused);
    QCOMPARE(model.centerFrequency(), quint64{100'100'000});

    settings.sync();
    QCOMPARE(
        settings.value(QStringLiteral("receiver/listeningFrequencyHz"))
            .toULongLong(),
        qulonglong{100'100'000});

    model.stopScan();
    QTest::qWait(20);
    const auto requestsBeforeBurst =
        trace->requestedListeningFrequencies.size();
    constexpr quint64 finalBurstFrequency = 100'199'000;
    for (quint64 offset = 0; offset < 200; ++offset) {
        runtime.requestScannerListeningFrequency(100'000'000 + offset * 1'000);
    }
    QVERIFY(waitUntil([&model] {
        return model.listeningFrequency() == finalBurstFrequency;
    }));
    const auto burstBackendRequests =
        trace->requestedListeningFrequencies.size() - requestsBeforeBurst;
    QVERIFY(burstBackendRequests >= 1);
    QVERIFY(burstBackendRequests <= 2);
    QCOMPARE(trace->requestedListeningFrequencies.back(), finalBurstFrequency);

    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, this, [&heartbeatCount] {
        ++heartbeatCount;
    });
    heartbeat.start();
    model.setScanDwellMilliseconds(1);
    model.startScan();
    QTest::qWait(100);
    model.pauseOrResumeScan();
    heartbeat.stop();
    QVERIFY(heartbeatCount >= 5);
    QCOMPARE(model.scanState(), QStringLiteral("Paused"));
    QVERIFY(model.receiverRunning());
    runtime.shutdown();
}

void ReceiverRuntimeTest::wideRangeBlockRetunesAvoidGlobalReceiverReconfiguration()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    model.setScanTypeIndex(1);
    model.setScanLowerFrequency(100'000'000);
    model.setScanUpperFrequency(104'000'000);
    model.setScanStepSize(1'000'000);
    model.setScanDwellMilliseconds(100'000);
    const auto centerRequests = trace->requestedCenterFrequencies.size();
    const int audioFlushes = trace->audioFlushes;
    const int filterApplications = trace->filterApplications;
    const int modeApplications = trace->modeApplications;
    QSignalSpy operationPending(
        &runtime, &sdr::app::ReceiverRuntime::operationPending);
    QSignalSpy runtimeBusyChanges(&model, &ApplicationModel::runtimeBusyChanged);

    model.startScan();
    QVERIFY(waitUntil([&model] {
        return model.scanState() == QLatin1String("Running");
    }));
    QCOMPARE(trace->requestedCenterFrequencies.size(), centerRequests + 1);
    QCOMPARE(operationPending.count(), 0);
    QCOMPARE(runtimeBusyChanges.count(), 0);
    QCOMPARE(trace->filterApplications, filterApplications);
    QCOMPARE(trace->modeApplications, modeApplications);

    model.pauseOrResumeScan();
    const quint64 firstCenter = model.centerFrequency();
    model.skipScanFrequency();
    QTest::qWait(20);
    QCOMPARE(model.centerFrequency(), firstCenter);
    QCOMPARE(trace->requestedCenterFrequencies.size(), centerRequests + 1);

    model.skipScanFrequency();
    QVERIFY(waitUntil([&model] {
        return model.scanState() == QLatin1String("Paused");
    }));
    QCOMPARE(trace->requestedCenterFrequencies.size(), centerRequests + 2);
    QVERIFY(model.centerFrequency() != firstCenter);
    QCOMPARE(model.listeningFrequency(), quint64{102'000'000});
    QCOMPARE(operationPending.count(), 0);
    QCOMPARE(runtimeBusyChanges.count(), 0);
    QCOMPARE(trace->filterApplications, filterApplications);
    QCOMPARE(trace->modeApplications, modeApplications);
    QCOMPARE(trace->audioFlushes, audioFlushes + 2);
    runtime.shutdown();
}

void ReceiverRuntimeTest::rejectsInvalidPersistedReceiverControls()
{
    QSettings settings;
    settings.setValue(
        QStringLiteral("receiver/centerFrequencyHz"), QStringLiteral("invalid"));
    settings.setValue(
        QStringLiteral("receiver/listeningFrequencyHz"), quint64{200'000'000});
    settings.setValue(QStringLiteral("receiver/demodulationMode"), 99);
    settings.setValue(QStringLiteral("receiver/gainDb"), 1'000.0);
    settings.setValue(QStringLiteral("receiver/squelchThresholdDb"), -200.0);
    settings.setValue(QStringLiteral("receiver/squelchDisabled"), QStringLiteral("maybe"));
    settings.sync();

    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);

    QCOMPARE(model.centerFrequency(), quint64{100'000'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'000'000});
    QCOMPARE(model.demodulationModeName(), QStringLiteral("AM"));
    QCOMPARE(model.requestedGain(), 20.0);
    QCOMPARE(model.squelchLevel(), -80.0);
    QVERIFY(!model.squelchDisabled());

    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    QCOMPARE(model.demodulationModeName(), QStringLiteral("AM"));
    QCOMPARE(model.requestedGain(), 20.0);
    QVERIFY(!model.squelchDisabled());
    runtime.shutdown();
}

void ReceiverRuntimeTest::migratesLegacyDemodulatorOrdinalToStableId()
{
    QSettings settings;
    settings.setValue(
        QStringLiteral("receiver/demodulationMode"),
        static_cast<int>(radio::DemodulationMode::Usb));
    settings.sync();

    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Mock);
    ApplicationModel model(runtime);
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(
        settings.value(QStringLiteral("receiver/demodulationMode")).toString(),
        QStringLiteral("usb"));
    runtime.shutdown();
}

void ReceiverRuntimeTest::usesNearestSupportedGainWithoutOverwritingSavedPreference()
{
    QSettings settings;
    settings.setValue(QStringLiteral("receiver/gainDb"), 21.0);
    settings.sync();
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.requestedGain(), 21.0);
    QCOMPARE(model.gain(), 22.0);
    QCOMPARE(trace->requestedGains.front(), 22.0);
    QCOMPARE(QSettings().value(QStringLiteral("receiver/gainDb")).toDouble(), 21.0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::restoresPersistedValidGain()
{
    QSettings settings;
    settings.setValue(QStringLiteral("receiver/gainDb"), 22.0);
    settings.sync();
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.requestedGain(), 22.0);
    QCOMPARE(model.gain(), 22.0);
    QCOMPARE(trace->requestedGains.front(), 22.0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::coalescesRapidSpectrumWheelRetunesToTheFinalCenter()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    QSignalSpy centerRequests(
        &runtime,
        &sdr::app::ReceiverRuntime::setCenterFrequencyRequested);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    trace->requestedCenterFrequencies.clear();
    const int backendCreations = trace->backendCreations;
    const int backendStops = trace->backendStops;
    const int audioFlushes = trace->audioFlushes;
    // Keep this test focused on hardware coalescing rather than wheel speed.
    quint64 wheelTimestamp = 0;
    model.setWheelClockForTests([&wheelTimestamp] { return wheelTimestamp; });

    for (int event = 0; event < 4; ++event) {
        model.handleFrequencyWheel(false, 30, Qt::NoModifier);
        wheelTimestamp += 400'000'000;
    }
    for (int event = 0; event < 19; ++event) {
        model.handleFrequencyWheel(false, 120, Qt::NoModifier);
        wheelTimestamp += 400'000'000;
    }

    QCOMPARE(model.centerFrequency(), quint64{100'200'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'200'000});
    QVERIFY(waitUntil([&model, &centerRequests, &trace] {
        return !model.runtimeBusy() && centerRequests.count() == 1 &&
               trace->requestedCenterFrequencies.size() == 1;
    }));
    QCOMPARE(
        centerRequests.front().front().toULongLong(),
        qulonglong{100'200'000});
    QCOMPARE(
        trace->requestedCenterFrequencies.front(),
        std::uint64_t{100'200'000});
    QCOMPARE(model.centerFrequency(), quint64{100'200'000});
    QCOMPARE(model.listeningFrequency(), quint64{100'200'000});
    QCOMPARE(trace->backendCreations, backendCreations);
    QCOMPARE(trace->backendStops, backendStops);
    QCOMPARE(trace->audioFlushes, audioFlushes + 1);
    QCOMPARE(waterfallResets.count(), 0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::recentersRunningReceiverAndFlushesStaleAudio()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    model.selectListeningFrequencyAt(75.0, 100.0);
    QVERIFY(waitUntil([&model] {
        return model.listeningFrequency() == quint64{100'500'000};
    }));
    model.setCenterFrequencyText(QStringLiteral("101000000"));
    QVERIFY(waitUntil([&model] {
        return model.centerFrequency() == quint64{101'000'000} &&
               model.listeningFrequency() == quint64{101'000'000};
    }));
    QVERIFY(trace->audioFlushes >= 1);
    runtime.shutdown();
}

void ReceiverRuntimeTest::remembersFilterWidthPerMode()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.setFilterWidth(9'000);
    QVERIFY(waitUntil([&model] { return model.filterWidth() == quint64{9'000}; }));
    model.setDemodulationModeIndex(static_cast<int>(radio::DemodulationMode::Nfm));
    QVERIFY(waitUntil([&model] { return model.demodulationModeName() == QStringLiteral("NFM"); }));
    model.setFilterWidth(8'330);
    QVERIFY(waitUntil([&model] { return model.filterWidth() == quint64{8'330}; }));
    model.setDemodulationModeIndex(static_cast<int>(radio::DemodulationMode::Am));
    QVERIFY(waitUntil([&model] { return model.filterWidth() == quint64{9'000}; }));
    runtime.shutdown();
}

void ReceiverRuntimeTest::appliesBookmarkAsOneAsynchronousLiveOperation()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(model.bookmarkModel());
    QVERIFY(bookmarks);
    sdr::app::BookmarkData data;
    data.name = QStringLiteral("Live AM");
    data.listeningFrequency = 101'250'000;
    data.requestedGainDb = 27.0;
    data.demodulatorId = QStringLiteral("am");
    data.filterLowHz = -4'500;
    data.filterHighHz = 4'500;
    data.squelchThresholdDb = -58.0;
    data.squelchEnabled = false;
    const QString uuid = bookmarks->addBookmark(-1, data);
    const QString expectedBookmarkPath = QDir(m_settingsDirectory.path()).filePath(
        QCoreApplication::applicationName() + QStringLiteral("/bookmarks.json"));
    QCOMPARE(
        QDir::cleanPath(bookmarks->filePath()),
        QDir::cleanPath(expectedBookmarkPath));
    QVERIFY(QTest::qWaitFor(
        [bookmarks] { return !bookmarks->persistencePending(); }));
    QVERIFY(QFile::exists(expectedBookmarkPath));
    const int backendCreations = trace->backendCreations;
    const int backendStops = trace->backendStops;
    const int audioFlushes = trace->audioFlushes;
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    QSignalSpy requests(&runtime,
        &sdr::app::ReceiverRuntime::applyBookmarkRequested);
    model.tuneBookmark(bookmarks->visibleRowForUuid(uuid));
    QCOMPARE(requests.count(), 1);
    QVERIFY(waitUntil([&model] {
        return !model.runtimeBusy() &&
               model.listeningFrequency() == quint64{101'250'000};
    }));
    QVERIFY(model.receiverRunning());
    QCOMPARE(model.requestedGain(), 27.0);
    QCOMPARE(model.filterWidth(), quint64{9'000});
    QCOMPARE(model.squelchLevel(), -58.0);
    QVERIFY(model.squelchDisabled());
    QCOMPARE(trace->backendCreations, backendCreations);
    QCOMPARE(trace->backendStops, backendStops);
    QCOMPARE(trace->audioFlushes, audioFlushes);
    QCOMPARE(waterfallResets.count(), 0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::appliesBookmarkScannerTransitionsDifferentially()
{
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    auto* bookmarks = qobject_cast<sdr::app::BookmarkTreeModel*>(
        model.bookmarkModel());
    QVERIFY(bookmarks);
    const auto add = [bookmarks](const QString& name, quint64 frequency) {
        sdr::app::BookmarkData bookmark;
        bookmark.name = name;
        bookmark.listeningFrequency = frequency;
        bookmark.requestedGainDb = 20.0;
        bookmark.demodulatorId = QStringLiteral("am");
        bookmark.filterLowHz = -6'250;
        bookmark.filterHighHz = 6'250;
        bookmark.squelchThresholdDb = -80.0;
        bookmark.squelchEnabled = true;
        bookmark.hasSavedSquelch = true;
        bookmark.scannerIncluded = true;
        return bookmarks->addBookmark(-1, bookmark);
    };
    QVERIFY(!add(QStringLiteral("Current"), 100'000'000).isEmpty());
    QVERIFY(!add(QStringLiteral("Next"), 100'010'000).isEmpty());
    sdr::app::BookmarkData changed;
    changed.name = QStringLiteral("Changed USB");
    changed.listeningFrequency = 100'020'000;
    changed.requestedGainDb = 27.0;
    changed.demodulatorId = QStringLiteral("usb");
    changed.filterLowHz = 0;
    changed.filterHighHz = 2'700;
    changed.squelchThresholdDb = -55.0;
    changed.squelchEnabled = false;
    changed.hasSavedSquelch = true;
    changed.scannerIncluded = true;
    QVERIFY(!bookmarks->addBookmark(-1, changed).isEmpty());

    const auto gainsBefore = trace->requestedGains.size();
    const int modesBefore = trace->modeApplications;
    const int filtersBefore = trace->filterApplications;
    const int squelchBefore = trace->squelchLevelApplications;
    const int disabledBefore = trace->disabledSquelchApplications;
    const int stopsBefore = trace->backendStops;
    QSignalSpy runningChanges(&model, &ApplicationModel::receiverRunningChanged);
    QSignalSpy fftChanges(&model, &ApplicationModel::spectrumFftSizeChanged);
    QSignalSpy waterfallChanges(&model, &ApplicationModel::waterfallSettingsChanged);
    QSignalSpy modeChanges(&model, &ApplicationModel::demodulationModeChanged);
    QSignalSpy filterChanges(&model, &ApplicationModel::filterWidthChanged);
    QSignalSpy gainChanges(&model, &ApplicationModel::requestedGainChanged);
    QSignalSpy squelchChanges(&model, &ApplicationModel::squelchStateChanged);

    model.setBookmarkScanDwellMilliseconds(100'000);
    model.startBookmarkScan();
    QVERIFY(waitUntil([&model] {
        return model.bookmarkScanState() == QLatin1String("Running");
    }));
    QCOMPARE(trace->requestedGains.size(), gainsBefore);
    QCOMPARE(trace->modeApplications, modesBefore);
    QCOMPARE(trace->filterApplications, filtersBefore);
    QCOMPARE(trace->squelchLevelApplications, squelchBefore);
    QCOMPARE(trace->disabledSquelchApplications, disabledBefore);
    QCOMPARE(modeChanges.count(), 0);
    QCOMPARE(filterChanges.count(), 0);
    QCOMPARE(gainChanges.count(), 0);
    QCOMPARE(squelchChanges.count(), 0);

    model.pauseOrResumeBookmarkScan();
    model.skipBookmarkScan();
    QVERIFY(waitUntil([&model] {
        return model.bookmarkScanCurrentName() == QLatin1String("Next") &&
               model.bookmarkScanState() == QLatin1String("Paused");
    }));
    QCOMPARE(model.listeningFrequency(), quint64{100'010'000});
    QCOMPARE(trace->requestedGains.size(), gainsBefore);
    QCOMPARE(trace->modeApplications, modesBefore);
    QCOMPARE(trace->filterApplications, filtersBefore);
    QCOMPARE(trace->squelchLevelApplications, squelchBefore);
    QCOMPARE(modeChanges.count(), 0);
    QCOMPARE(filterChanges.count(), 0);
    QCOMPARE(gainChanges.count(), 0);
    QCOMPARE(squelchChanges.count(), 0);
    QCOMPARE(runningChanges.count(), 0);
    QCOMPARE(fftChanges.count(), 0);
    QCOMPARE(waterfallChanges.count(), 0);
    QCOMPARE(trace->backendStops, stopsBefore);
    QVERIFY(model.receiverRunning());

    modeChanges.clear();
    filterChanges.clear();
    gainChanges.clear();
    squelchChanges.clear();
    model.skipBookmarkScan();
    QVERIFY(waitUntil([&model] {
        return model.bookmarkScanCurrentName() == QLatin1String("Changed USB") &&
               model.bookmarkScanState() == QLatin1String("Paused");
    }));
    QCOMPARE(model.demodulationModeName(), QStringLiteral("USB"));
    QCOMPARE(model.filterWidth(), quint64{2'700});
    QCOMPARE(model.requestedGain(), 27.0);
    QCOMPARE(model.squelchLevel(), -55.0);
    QVERIFY(model.squelchDisabled());
    QCOMPARE(modeChanges.count(), 1);
    QCOMPARE(filterChanges.count(), 1);
    QCOMPARE(gainChanges.count(), 1);
    QCOMPARE(squelchChanges.count(), 1);
    QCOMPARE(runningChanges.count(), 0);
    QCOMPARE(fftChanges.count(), 0);
    QCOMPARE(waterfallChanges.count(), 0);
    QCOMPARE(trace->backendStops, stopsBefore);
    model.stopBookmarkScan();
    runtime.shutdown();
}

void ReceiverRuntimeTest::managesDsdFmeLifecycleWithoutRestartingOnRetune()
{
    QSettings().setValue(
        QStringLiteral("externalDecoder/dsdFmeBinaryPath"),
        QStringLiteral("/test/dsd-fme"));
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    model.setDemodulationModeIndex(
        static_cast<int>(radio::DemodulationMode::DigitalDecoderOutput));
    QVERIFY(waitUntil([&model, &trace] {
        return trace->dsdProcessStarts == 1 &&
               model.dsdFmeStatusText() == QStringLiteral("DSD-FME running");
    }));
    QVERIFY(model.decoderRunning());
    QVERIFY(waitUntil([&model] {
        return model.applicationLog()->formattedText().contains(
            QStringLiteral("[Info] [DSD-FME] Decoder is running"));
    }));
    QCOMPARE(trace->dsdProgram, QStringLiteral("/test/dsd-fme"));
    QCOMPARE(
        trace->dsdArguments,
        QStringList({
            QStringLiteral("-i"),
            QStringLiteral("-"),
            QStringLiteral("-o"),
            QStringLiteral("-"),
        }));
    QCOMPARE(model.filterWidth(), quint64{12'500});

    model.setCenterFrequencyText(QStringLiteral("101000000"));
    QVERIFY(waitUntil([&model] {
        return model.centerFrequency() == quint64{101'000'000};
    }));
    QCOMPARE(trace->dsdProcessStarts, 1);
    QCOMPARE(trace->dsdProcessStops, 0);

    model.setDsdFmeBinaryPath(QStringLiteral("/test/dsd-fme-new"));
    QVERIFY(waitUntil([&trace] {
        return trace->dsdProcessStarts == 2 &&
               trace->dsdProcessStops == 1;
    }));
    QCOMPARE(trace->dsdProgram, QStringLiteral("/test/dsd-fme-new"));

    model.setDemodulationModeIndex(
        static_cast<int>(radio::DemodulationMode::Nfm));
    QVERIFY(waitUntil([&trace] { return trace->dsdProcessStops == 2; }));
    QVERIFY(model.receiverRunning());
    runtime.shutdown();
}

void ReceiverRuntimeTest::keepsReceptionAndDisplayActiveWhenDsdFmeFails()
{
    QSettings().setValue(
        QStringLiteral("externalDecoder/dsdFmeBinaryPath"),
        QStringLiteral("/test/failing-dsd-fme"));
    auto trace = std::make_shared<RuntimeTrace>();
    trace->dsdFailOnStart = true;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    QSignalSpy spectrumFrames(&model, &ApplicationModel::spectrumFrameReady);
    QSignalSpy waterfallFrames(&model, &ApplicationModel::waterfallFrameReady);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.setDemodulationModeIndex(
        static_cast<int>(radio::DemodulationMode::DigitalDecoderOutput));

    QVERIFY(waitUntil([&model, &spectrumFrames, &waterfallFrames] {
        return model.dsdFmeStatusText().contains(
                   QStringLiteral("simulated decoder failure")) &&
               spectrumFrames.count() > 0 &&
               waterfallFrames.count() > 0;
    }));
    QVERIFY(model.receiverRunning());
    QCOMPARE(trace->dsdProcessStarts, 1);
    runtime.shutdown();
}

void ReceiverRuntimeTest::
    enablesAutoPpmForCanonicalRtlSdrDiscoveryKeyBeforeOpen()
{
    auto trace = std::make_shared<RuntimeTrace>();
    trace->unconfirmedPpmCapabilities = true;
    trace->discoveryDriver = "RTLSDR";
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();

    QVERIFY(waitUntil([&model] {
        return model.selectedDeviceIndex() == 0 &&
               model.automaticPpmCalibrationSupported();
    }));
    QVERIFY(trace->openedIdentifiers.empty());
    runtime.shutdown();
}

void ReceiverRuntimeTest::cancellationPreservesPreviousPpmAndRunningReception()
{
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=one")), 7.0);
    auto trace = std::make_shared<RuntimeTrace>();
    trace->holdCalibrationReads = true;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] {
        return model.selectedDeviceIndex() == 0 &&
               model.automaticPpmCalibrationSupported();
    }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.ppmCorrection(), 7.0);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);

    model.startAutomaticPpmCalibration();
    QVERIFY(waitUntil([&model] { return model.ppmCalibrationRunning(); }));
    QVERIFY(model.receiverRunning());
    QCOMPARE(spectrumResets.count(), 0);
    QCOMPARE(waterfallResets.count(), 0);
    model.cancelAutomaticPpmCalibration();
    QVERIFY(waitUntil([&model] {
        return !model.ppmCalibrationRunning() &&
               model.ppmCalibrationStatus() == QStringLiteral("cancelled");
    }));

    QVERIFY(model.receiverRunning());
    QCOMPARE(model.ppmCorrection(), 7.0);
    QVERIFY(!trace->requestedPpmCorrections.empty());
    QCOMPARE(trace->requestedPpmCorrections.back(), 7.0);
    QCOMPARE(
        QSettings()
            .value(ppmSettingsKey(QStringLiteral("mock:serial=one")))
            .toDouble(),
        7.0);
    QCOMPARE(spectrumResets.count(), 0);
    QCOMPARE(waterfallResets.count(), 0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::failurePreservesPreviousPpm()
{
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=one")), -6.0);
    auto trace = std::make_shared<RuntimeTrace>();
    trace->failCalibrationRead = true;
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] {
        return model.automaticPpmCalibrationSupported();
    }));
    model.startAutomaticPpmCalibration();
    QVERIFY(waitUntil([&model] {
        return !model.ppmCalibrationRunning() &&
               model.ppmCalibrationStatus() == QStringLiteral("failed");
    }));

    QVERIFY(!model.receiverRunning());
    QCOMPARE(model.ppmCorrection(), -6.0);
    QVERIFY(!trace->requestedPpmCorrections.empty());
    QCOMPARE(trace->requestedPpmCorrections.back(), -6.0);
    QCOMPARE(
        QSettings()
            .value(ppmSettingsKey(QStringLiteral("mock:serial=one")))
            .toDouble(),
        -6.0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::manualPpmReplacesSavedAutomaticCorrection()
{
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=one")), 7.0);
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=two")), 12.0);
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    QCOMPARE(model.ppmCorrection(), 7.0);

    model.setPpmCorrection(3.0);
    QVERIFY(waitUntil([&model] { return model.ppmCorrection() == 3.0; }));
    QCOMPARE(
        QSettings().value(ppmSettingsKey(QStringLiteral("mock:serial=one")))
            .toDouble(),
        3.0);
    QCOMPARE(
        QSettings().value(ppmSettingsKey(QStringLiteral("mock:serial=two")))
            .toDouble(),
        12.0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::manualPpmPersistsDriverReadbackAndSurvivesReopen()
{
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=one")), 7.0);
    auto trace = std::make_shared<RuntimeTrace>();
    trace->effectiveManualPpmCorrection = 4.0;
    {
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Hardware,
            factoriesFor(trace));
        ApplicationModel model(runtime);
        runtime.start();
        QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
        model.startReception();
        QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
        model.setPpmCorrection(4.6);
        QVERIFY(waitUntil([&model] { return model.ppmCorrection() == 4.0; }));
        QVERIFY(waitUntil([] {
            return QSettings()
                       .value(ppmSettingsKey(QStringLiteral("mock:serial=one")))
                       .toDouble() == 4.0;
        }));
        runtime.shutdown();
    }

    auto reopenTrace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime reopened(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(reopenTrace));
    ApplicationModel reopenedModel(reopened);
    reopened.start();
    QVERIFY(waitUntil([&reopenedModel] {
        return reopenedModel.selectedDeviceIndex() == 0;
    }));
    reopenedModel.startReception();
    QVERIFY(waitUntil([&reopenedModel] {
        return reopenedModel.receiverRunning();
    }));
    QCOMPARE(reopenedModel.ppmCorrection(), 4.0);
    QCOMPARE(reopenTrace->requestedPpmCorrections.front(), 4.0);
    reopened.shutdown();
}

void ReceiverRuntimeTest::manualPpmApplyFailurePreservesSavedCorrection()
{
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=one")), -6.0);
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.selectedDeviceIndex() == 0; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    trace->failPpmCorrection = true;
    model.setPpmCorrection(8.0);
    QVERIFY(waitUntil([&model] {
        return model.statusText().contains(QStringLiteral("failure"));
    }));
    QCOMPARE(model.ppmCorrection(), -6.0);
    QCOMPARE(
        QSettings().value(ppmSettingsKey(QStringLiteral("mock:serial=one")))
            .toDouble(),
        -6.0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::
    successfulCalibrationPersistsBySerialAndAppliesBeforeTune()
{
    QSettings().setValue(
        QStringLiteral("receiver/captureBandwidthSamplesPerSecond"),
        quint64{200'000});
    QSettings().setValue(
        ppmSettingsKey(QStringLiteral("mock:serial=two")), 12.0);
    {
        auto trace = std::make_shared<RuntimeTrace>();
        sdr::app::ReceiverRuntime runtime(
            sdr::app::ReceiverRuntime::StartupMode::Hardware,
            factoriesFor(trace));
        ApplicationModel model(runtime);
        runtime.start();
        QVERIFY(waitUntil([&model] {
            return model.automaticPpmCalibrationSupported();
        }));
        model.startAutomaticPpmCalibration();
        QVERIFY2(
            waitUntil(
                [&model] {
                    return !model.ppmCalibrationRunning() &&
                           model.ppmCalibrationStatus() ==
                               QStringLiteral("completed");
                },
                5'000),
            qPrintable(
                model.ppmCalibrationStatus() + QStringLiteral(": ") +
                model.statusText()));
        QCOMPARE(model.ppmCorrection(), 1.0);
        QVERIFY(!model.receiverRunning());
        runtime.shutdown();
    }

    QCOMPARE(
        QSettings()
            .value(ppmSettingsKey(QStringLiteral("mock:serial=one")))
            .toDouble(),
        1.0);
    QCOMPARE(
        QSettings()
            .value(ppmSettingsKey(QStringLiteral("mock:serial=two")))
            .toDouble(),
        12.0);

    auto reopenTrace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime reopened(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(reopenTrace));
    ApplicationModel reopenedModel(reopened);
    reopened.start();
    QVERIFY(waitUntil([&reopenedModel] {
        return reopenedModel.selectedDeviceIndex() == 0;
    }));
    reopenedModel.startReception();
    QVERIFY(waitUntil([&reopenedModel] {
        return reopenedModel.receiverRunning();
    }));
    const auto ppm = std::ranges::find(
        reopenTrace->receiverOperationOrder, std::string("ppm"));
    const auto tune = std::ranges::find(
        reopenTrace->receiverOperationOrder, std::string("tune"));
    QVERIFY(ppm != reopenTrace->receiverOperationOrder.end());
    QVERIFY(tune != reopenTrace->receiverOperationOrder.end());
    QVERIFY(ppm < tune);
    QCOMPARE(reopenedModel.ppmCorrection(), 1.0);
    reopened.shutdown();
}

void ReceiverRuntimeTest::stoppedReceptionRemainsStoppedAfterCalibration()
{
    QSettings().setValue(
        QStringLiteral("receiver/captureBandwidthSamplesPerSecond"),
        quint64{200'000});
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    QSignalSpy spectrumResets(&model, &ApplicationModel::spectrumReset);
    QSignalSpy waterfallResets(&model, &ApplicationModel::waterfallReset);
    runtime.start();
    QVERIFY(waitUntil([&model] {
        return model.automaticPpmCalibrationSupported();
    }));
    QVERIFY(!model.receiverRunning());
    model.startAutomaticPpmCalibration();
    QVERIFY2(
        waitUntil(
            [&model] {
                return model.ppmCalibrationStatus() ==
                           QStringLiteral("completed") &&
                       !model.ppmCalibrationRunning();
            },
            5'000),
        qPrintable(
            model.ppmCalibrationStatus() + QStringLiteral(": ") +
            model.statusText()));
    QVERIFY(!model.receiverRunning());
    QVERIFY(spectrumResets.count() >= 1);
    QVERIFY(waterfallResets.count() >= 1);
    runtime.shutdown();
}

void ReceiverRuntimeTest::testCounterDataNeverReachesReceiverProcessing()
{
    QSettings().setValue(
        QStringLiteral("receiver/captureBandwidthSamplesPerSecond"),
        quint64{200'000});
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] {
        return model.automaticPpmCalibrationSupported();
    }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.startAutomaticPpmCalibration();
    QVERIFY(waitUntil(
        [&model] {
            return !model.ppmCalibrationRunning() &&
                   model.ppmCalibrationStatus() ==
                       QStringLiteral("completed");
        },
        5'000));
    QVERIFY(model.receiverRunning());
    QCOMPARE(trace->calibrationSpectrumReads, 0);
    QCOMPARE(trace->calibrationAudioReads, 0);
    QCOMPARE(trace->calibrationDecoderReads, 0);
    runtime.shutdown();
}

void ReceiverRuntimeTest::recordsMutedAudioAcrossRetunesAndFinalizesOnStop()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.setRecordingsFolder(recordings.path());
    QVERIFY(model.recordingsFolderValid());
    model.setAudioMuted(true);
    model.setAudioVolume(0);
    model.startAudioRecording();
    QVERIFY(waitUntil([&model] { return model.recordingActive(); }));
    QCOMPARE(model.recordingElapsedText(), QStringLiteral("00:00:00"));
    {
        std::lock_guard lock(trace->recordingAudioMutex);
        trace->recordingAudioSamples = {0.5F, -0.5F};
    }
    QVERIFY(waitUntil([&trace] {
        std::lock_guard lock(trace->recordingAudioMutex);
        return trace->recordingAudioSamples.empty();
    }));
    model.setListeningFrequency(99'500'000);
    QVERIFY(waitUntil([&model] {
        return model.listeningFrequency() == 99'500'000 &&
               model.recordingActive();
    }));
    model.setScanLowerFrequency(99'400'000);
    model.setScanUpperFrequency(99'500'000);
    model.setScanStepSize(50'000);
    model.startScan();
    QVERIFY(waitUntil([&model] { return model.scannerOwnsTuning(); }));
    QVERIFY(model.recordingActive());
    model.stopScan();
    model.stopReception();
    QVERIFY(waitUntil([&model] {
        return !model.receiverRunning() && !model.recordingActive();
    }));
    const QStringList wavs = QDir(recordings.path()).entryList(
        {QStringLiteral("*.wav")}, QDir::Files);
    QCOMPARE(wavs.size(), 1);
    QFile file(recordings.filePath(wavs.front()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(bytes.size() >= 52);
    QCOMPARE(bytes.mid(0, 4), QByteArray("RIFF"));
    QCOMPARE(bytes.mid(36, 4), QByteArray("data"));
    QCOMPARE(bytes.mid(44, 2), QByteArray::fromHex("0040"));
    QCOMPARE(bytes.mid(46, 2), QByteArray::fromHex("0040"));
    QCOMPARE(bytes.mid(48, 2), QByteArray::fromHex("00c0"));
    QCOMPARE(bytes.mid(50, 2), QByteArray::fromHex("00c0"));
    runtime.shutdown();
}

void ReceiverRuntimeTest::armsQuietSkippingRecordingAcrossScannerAndRetunes()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.setRecordingsFolder(recordings.path());
    model.setSkipQuietRecordingParts(true);
    model.setRecordingPreRollSeconds(0);
    model.setRecordingTailSeconds(0);
    model.startAudioRecording();
    QVERIFY(waitUntil([&model] { return model.recordingActive(); }));
    QVERIFY(!model.recordingWriting());
    {
        std::lock_guard lock(trace->recordingAudioMutex);
        trace->recordingAudioSamples = {0.1F};
    }
    QVERIFY(waitUntil([&trace] {
        std::lock_guard lock(trace->recordingAudioMutex);
        return trace->recordingAudioSamples.empty();
    }));
    QVERIFY(!model.recordingWriting());
    trace->recordingSquelchOpen.store(true);
    {
        std::lock_guard lock(trace->recordingAudioMutex);
        trace->recordingAudioSamples = {0.5F};
    }
    QVERIFY(waitUntil([&model] { return model.recordingWriting(); }));
    model.setListeningFrequency(99'500'000);
    QVERIFY(waitUntil([&model] {
        return model.listeningFrequency() == 99'500'000 && model.recordingActive();
    }));
    model.setScanLowerFrequency(99'400'000);
    model.setScanUpperFrequency(99'500'000);
    model.setScanStepSize(50'000);
    model.startScan();
    QVERIFY(waitUntil([&model] { return model.scannerOwnsTuning(); }));
    QVERIFY(model.recordingActive());
    model.stopReception();
    QVERIFY(waitUntil([&model] { return !model.recordingActive(); }));
    const QStringList wavs = QDir(recordings.path()).entryList(
        {QStringLiteral("*.wav")}, QDir::Files);
    QCOMPARE(wavs.size(), 1);
    QFile file(recordings.filePath(wavs.front()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QCOMPARE(bytes.size(), 48);
    QCOMPARE(bytes.mid(44, 2), QByteArray::fromHex("0040"));
    runtime.shutdown();
}

void ReceiverRuntimeTest::recordsScannerActivityWithSidecarAlongsideManualRecording()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware, factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.setRecordingsFolder(recordings.path());
    model.setRecordingPreRollSeconds(0);
    model.setRecordingTailSeconds(0);
    model.startAudioRecording();
    QVERIFY(waitUntil([&model] { return model.recordingActive(); }));
    model.setRecordScannerActivity(true);
    model.setScanLowerFrequency(99'400'000);
    model.setScanUpperFrequency(99'500'000);
    model.setScanStepSize(50'000);
    model.startScan();
    QVERIFY(waitUntil([&model] { return model.scannerOwnsTuning(); }));
    QVERIFY(waitUntil([&model] { return model.scannerRecordingArmed(); }));
    trace->recordingSquelchOpen.store(true);
    {
        std::lock_guard lock(trace->recordingAudioMutex);
        trace->recordingAudioSamples = {0.5F};
    }
    QVERIFY(waitUntil([&model] { return model.scannerRecordingWriting(); }));
    trace->recordingSquelchOpen.store(false);
    {
        std::lock_guard lock(trace->recordingAudioMutex);
        trace->recordingAudioSamples = {0.1F};
    }
    QVERIFY(waitUntil([&model] { return model.scannerRecordingArmed(); }));
    QVERIFY(model.recordingActive());
    model.stopScan();
    QVERIFY(waitUntil([&model] { return !model.scannerOwnsTuning(); }));
    model.stopAudioRecording();
    QVERIFY(waitUntil([&model] { return !model.recordingActive(); }));
    const QStringList sidecars = QDir(recordings.path()).entryList(
        {QStringLiteral("*_scanner-filtered-audio.json")}, QDir::Files);
    QCOMPARE(sidecars.size(), 1);
    QFile sidecar(recordings.filePath(sidecars.front()));
    QVERIFY(sidecar.open(QIODevice::ReadOnly));
    const auto json = QJsonDocument::fromJson(sidecar.readAll()).object();
    QVERIFY(json.contains(QStringLiteral("start_time")));
    QVERIFY(json.contains(QStringLiteral("end_time")));
    QVERIFY(json.value(QStringLiteral("listening_frequency_hz")).toInteger() > 0);
    QVERIFY(!json.value(QStringLiteral("mode")).toString().isEmpty());
    QVERIFY(!json.value(QStringLiteral("scanner_source")).toString().isEmpty());
    QCOMPARE(json.value(QStringLiteral("wav_format")).toObject()
                 .value(QStringLiteral("channels")).toInt(), 2);
    QVERIFY(json.value(QStringLiteral("duration_seconds")).toDouble() > 0.0);
    QCOMPARE(QDir(recordings.path()).entryList(
                 {QStringLiteral("*.wav")}, QDir::Files).size(), 2);
    runtime.shutdown();
}

void ReceiverRuntimeTest::recordsIqAcrossScannerAndSegmentsOnlyCaptureChanges()
{
    QTemporaryDir recordings;
    QVERIFY(recordings.isValid());
    auto trace = std::make_shared<RuntimeTrace>();
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware, factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));
    model.setRecordingsFolder(recordings.path());
    model.startIqRecording();
    QVERIFY(waitUntil([&model] { return model.iqRecordingActive(); }));
    {
        std::lock_guard lock(trace->recordingIqMutex);
        trace->recordingIqSamples = {{0.5F, -0.25F}};
    }
    QVERIFY(waitUntil([&trace] {
        std::lock_guard lock(trace->recordingIqMutex);
        return trace->recordingIqSamples.empty();
    }));
    model.setListeningFrequency(99'500'000);
    QVERIFY(waitUntil([&model] { return model.listeningFrequency() == 99'500'000; }));
    QCOMPARE(QDir(recordings.path()).entryList({QStringLiteral("*.cf32")}, QDir::Files).size(), 1);
    model.setScanLowerFrequency(99'400'000);
    model.setScanUpperFrequency(99'500'000);
    model.setScanStepSize(50'000);
    model.startScan();
    QVERIFY(waitUntil([&model] { return model.scannerOwnsTuning(); }));
    QVERIFY(model.iqRecordingActive());
    model.stopScan();
    QVERIFY(waitUntil([&model] { return !model.scannerOwnsTuning(); }));
    model.setCenterFrequencyText(QStringLiteral("102000000"));
    QVERIFY(waitUntil([&model] { return model.centerFrequency() == 102'000'000; }));
    QVERIFY(waitUntil([&recordings] {
        return QDir(recordings.path()).entryList(
            {QStringLiteral("*.cf32")}, QDir::Files).size() >= 2;
    }));
    model.setSampleRate(2'400'000);
    QVERIFY(waitUntil([&model] { return model.sampleRate() == 2'400'000; }));
    QVERIFY(waitUntil([&recordings] {
        return QDir(recordings.path()).entryList(
            {QStringLiteral("*.cf32")}, QDir::Files).size() >= 3;
    }));
    model.stopIqRecording();
    QVERIFY(waitUntil([&model] { return !model.iqRecordingActive(); }));
    const auto files = QDir(recordings.path()).entryList(
        {QStringLiteral("*.cf32")}, QDir::Files);
    QVERIFY(files.size() >= 2);
    for (const auto& fileName : files) {
        const auto jsonName = fileName.left(fileName.size() - 5) + QStringLiteral(".json");
        QVERIFY(QFile::exists(recordings.filePath(jsonName)));
    }
    runtime.shutdown();
}

void ReceiverRuntimeTest::stopsBackendAndJoinsWorkerDuringShutdown()
{
    auto trace = std::make_shared<RuntimeTrace>();
    const quintptr guiThreadToken = reinterpret_cast<quintptr>(
        QThread::currentThreadId());
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        factoriesFor(trace));
    ApplicationModel model(runtime);
    runtime.start();
    model.refreshDevices();
    QVERIFY(waitUntil([&model] { return model.deviceDisplayNames().size() == 2; }));
    model.selectDeviceIndex(0);
    QVERIFY(waitUntil([&model] { return model.backendReady(); }));
    model.startReception();
    QVERIFY(waitUntil([&model] { return model.receiverRunning(); }));

    runtime.shutdown();

    QVERIFY(!runtime.workerThreadRunning());
    QCOMPARE(trace->backendStops, 1);
    QCOMPARE(trace->backendDestructions, 1);
    QVERIFY(trace->stopThreadToken != guiThreadToken);
    QVERIFY(trace->destructionThreadToken != guiThreadToken);
    QVERIFY(trace->audioOpenThreadToken != guiThreadToken);
    QVERIFY(trace->audioCloseThreadToken != guiThreadToken);
}

QTEST_GUILESS_MAIN(ReceiverRuntimeTest)

#include "ReceiverRuntimeTest.moc"
