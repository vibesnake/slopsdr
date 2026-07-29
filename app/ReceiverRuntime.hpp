// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "DeviceController.hpp"
#include "AudioOutputService.hpp"
#include "DsdFmeProcessService.hpp"
#include "ReceiverBackend.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QtGlobal>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sdr::app {

struct ReceiverRuntimeSnapshot {
    radio::ReceiverState receiverState;
    radio::ReceiverLimits receiverLimits;
    radio::ReceiverCapabilities receiverCapabilities;
    bool squelchOpen = false;
    quint64 effectiveSampleRate = 0;
    quint64 tuningGeneration = 0;
    // The requested value is persisted even when the backend must use a
    // smaller plan. Frames and display mapping use the effective value.
    quint64 spectrumFftSize = 4'096;
    quint64 effectiveSpectrumFftSize = 4'096;
    double spectrumHertzPerBin = 0.0;
    double effectiveSpectrumFramesPerSecond = 60.0;
    quint32 visibleWaterfallHistorySeconds = 10;
    QStringList deviceIdentifiers;
    QStringList deviceDisplayNames;
    QString selectedDeviceIdentifier;
    QStringList audioDeviceIdentifiers;
    QStringList audioDeviceDisplayNames;
    QString selectedAudioDeviceIdentifier;
    QString deviceState = QStringLiteral("No device selected");
    QString deviceCapabilitySummary = QStringLiteral("No device capabilities");
    QString backendDescription = QStringLiteral("No receiver backend");
    QString statusText = QStringLiteral("No device selected");
    QString audioStatusText = QStringLiteral("Audio output is initializing");
    QString dsdFmeStatusText = QStringLiteral("DSD-FME not configured");
    std::vector<radio::FrequencyRange> deviceFrequencyRanges;
    std::vector<radio::FrequencyRange> deviceSampleRateRanges;
    QStringList captureBandwidthOptions;
    bool customCaptureBandwidthSupported = false;
    double minimumGainDb = 0.0;
    double maximumGainDb = 0.0;
    double gainStepDb = 0.0;
    double requestedGainDb = 20.0;
    bool gainSupported = false;
    bool backendReady = false;
    bool discoveryAvailable = false;
    bool mockMode = false;
    bool automaticPpmCalibrationSupported = false;
    bool ppmCalibrationRunning = false;
    QString ppmCalibrationStatus = QStringLiteral("idle");
    int ppmCalibrationProgressPercent = 0;
    quint64 ppmCalibrationDisplayResetGeneration = 0;
    bool operationSucceeded = true;
    int audioVolumePercent = 75;
    bool audioMuted = false;
    bool audioReady = false;
    bool audioRunning = false;
    bool decoderRunning = false;
    quint64 audioOverflowEvents = 0;
    quint64 audioUnderrunEvents = 0;
    quintptr workerThreadToken = 0;
};

class ReceiverRuntime final : public QObject
{
    Q_OBJECT

public:
    enum class StartupMode {
        Hardware,
        Mock,
    };
    Q_ENUM(StartupMode)

    using DeviceProviderFactory =
        std::function<std::unique_ptr<devices::DeviceProvider>()>;
    using HardwareBackendFactory = std::function<
        std::unique_ptr<radio::ReceiverBackend>(
            std::unique_ptr<devices::DeviceController>)>;
    using AudioOutputServiceFactory =
        std::function<std::unique_ptr<platform::AudioOutputService>()>;
    using DsdFmeProcessServiceFactory =
        std::function<std::unique_ptr<platform::DsdFmeProcessService>()>;
    using ApplicationLogHandler =
        std::function<void(int, const QString&, const QString&)>;
    using MonotonicClock = std::function<std::uint64_t()>;

    struct Factories {
        DeviceProviderFactory createDeviceProvider;
        HardwareBackendFactory createHardwareBackend;
        AudioOutputServiceFactory createAudioOutputService;
        DsdFmeProcessServiceFactory createDsdFmeProcessService;
        MonotonicClock monotonicClock;
        std::size_t initialSpectrumFftSize = 4'096;
    };

    explicit ReceiverRuntime(
        StartupMode startupMode,
        QObject* parent = nullptr,
        bool verboseAudioMetrics = false);
    ReceiverRuntime(
        StartupMode startupMode,
        Factories factories,
        QObject* parent = nullptr,
        bool verboseAudioMetrics = false);
    ~ReceiverRuntime() override;

    ReceiverRuntime(const ReceiverRuntime&) = delete;
    ReceiverRuntime& operator=(const ReceiverRuntime&) = delete;

    [[nodiscard]] StartupMode startupMode() const noexcept;
    [[nodiscard]] bool workerThreadRunning() const noexcept;
    void setApplicationLogHandler(ApplicationLogHandler handler);

public slots:
    void start();
    void shutdown();
    void refreshDevices();
    void selectDevice(const QString& identifier);
    void clearDeviceSelection();
    void selectAudioDevice(const QString& identifier);
    void setAudioVolume(int volumePercent);
    void setAudioMuted(bool muted);
    void setDsdFmeBinaryPath(const QString& path);
    void startReception();
    void stopReception();
    void setCenterFrequency(quint64 frequency);
    void setListeningFrequency(quint64 frequency);
    void requestScannerListeningFrequency(quint64 frequency);
    void requestScannerCenterFrequency(quint64 frequency);
    void cancelScannerListeningFrequencyRequests();
    void shiftCenterFrequency(qint64 requestedStep);
    void setSampleRate(quint64 sampleRate);
    void setSpectrumFftSize(quint64 fftSize);
    void setVisibleWaterfallHistorySeconds(quint32 seconds);
    void setFilterWidth(quint64 filterWidth);
    void setGain(double gainDb);
    void setPpmCorrection(double ppmCorrection);
    void startAutomaticPpmCalibration();
    void cancelAutomaticPpmCalibration();
    void setDemodulationMode(int mode);
    void setSquelchLevel(double squelchLevelDb);
    void enableManualSquelch();
    void enableAutomaticSquelch();
    void disableSquelch();
    void applyBookmark(quint64 frequency, double requestedGainDb,
        int demodulationMode, quint64 filterWidth,
        double squelchThresholdDb, bool squelchEnabled,
        bool applySquelch = true);

signals:
    void snapshotChanged(const sdr::app::ReceiverRuntimeSnapshot& snapshot);
    void spectrumFrameReady(
        const QVector<float>& normalizedMagnitudes,
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize,
        quint64 sequence,
        quint64 timestampNanoseconds,
        quint64 tuningGeneration);
    void waterfallFrameReady(
        const QVector<float>& normalizedMagnitudes,
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize,
        quint64 sequence,
        quint64 timestampNanoseconds,
        quint64 tuningGeneration);
    void operationPending(const QString& description);
    void centerFrequencyRequestCompleted(quint64 frequency, bool succeeded);
    void scannerListeningFrequencyChanged(
        quint64 frequency,
        bool succeeded,
        const QString& message);
    void scannerCenterFrequencyChanged(
        quint64 requestedFrequency,
        quint64 appliedCenterFrequency,
        quint64 appliedListeningFrequency,
        bool succeeded,
        const QString& message);

signals:
    void initializeRequested();
    void refreshDevicesRequested();
    void selectDeviceRequested(const QString& identifier);
    void clearDeviceSelectionRequested();
    void selectAudioDeviceRequested(const QString& identifier);
    void setAudioVolumeRequested(int volumePercent);
    void setAudioMutedRequested(bool muted);
    void setDsdFmeBinaryPathRequested(const QString& path);
    void startReceptionRequested();
    void stopReceptionRequested();
    void setCenterFrequencyRequested(quint64 frequency);
    void setListeningFrequencyRequested(quint64 frequency);
    void setScannerListeningFrequencyRequested(quint64 frequency);
    void setScannerCenterFrequencyRequested(quint64 frequency);
    void shiftCenterFrequencyRequested(qint64 requestedStep);
    void setSampleRateRequested(quint64 sampleRate);
    void setSpectrumFftSizeRequested(quint64 fftSize);
    void setVisibleWaterfallHistorySecondsRequested(quint32 seconds);
    void setFilterWidthRequested(quint64 filterWidth);
    void setGainRequested(double gainDb);
    void setPpmCorrectionRequested(double ppmCorrection);
    void startAutomaticPpmCalibrationRequested();
    void cancelAutomaticPpmCalibrationRequested();
    void setDemodulationModeRequested(int mode);
    void setSquelchLevelRequested(double squelchLevelDb);
    void enableManualSquelchRequested();
    void enableAutomaticSquelchRequested();
    void disableSquelchRequested();
    void applyBookmarkRequested(quint64 frequency, double requestedGainDb,
        int demodulationMode, quint64 filterWidth,
        double squelchThresholdDb, bool squelchEnabled,
        bool applySquelch);

private:
    class Worker;

    void markPending(const QString& description);
    void finishScannerListeningFrequencyRequest(
        quint64 requestedFrequency,
        quint64 appliedFrequency,
        bool succeeded,
        const QString& message);

    StartupMode m_startupMode;
    QThread m_workerThread;
    Worker* m_worker = nullptr;
    bool m_started = false;
    std::optional<quint64> m_latestScannerListeningFrequency;
    bool m_scannerListeningFrequencyRequestActive = false;
};

}  // namespace sdr::app

Q_DECLARE_METATYPE(sdr::app::ReceiverRuntimeSnapshot)
