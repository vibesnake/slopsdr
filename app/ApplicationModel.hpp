// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "BookmarkTreeModel.hpp"
#include "ApplicationLogModel.hpp"
#include "FrequencyViewport.hpp"
#include "ReceiverRuntime.hpp"
#include "ReceiverBackend.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrl>
#include <QtGlobal>
#include <QVector>
#include <QVariantList>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sdr::app {
struct FrequencyEditResult;
}

class ApplicationModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(quint64 centerFrequency READ centerFrequency NOTIFY centerFrequencyChanged)
    Q_PROPERTY(quint64 listeningFrequency READ listeningFrequency NOTIFY listeningFrequencyChanged)
    Q_PROPERTY(QString centerFrequencyDigits READ centerFrequencyDigits NOTIFY centerFrequencyDigitsChanged)
    Q_PROPERTY(double listeningPosition READ listeningPosition NOTIFY listeningPositionChanged)
    Q_PROPERTY(double filterLowerPosition READ filterLowerPosition NOTIFY filterMarkerChanged)
    Q_PROPERTY(double filterUpperPosition READ filterUpperPosition NOTIFY filterMarkerChanged)
    Q_PROPERTY(quint64 filterLowerFrequency READ filterLowerFrequency NOTIFY filterMarkerChanged)
    Q_PROPERTY(quint64 filterUpperFrequency READ filterUpperFrequency NOTIFY filterMarkerChanged)
    Q_PROPERTY(quint64 visibleLowerFrequency READ visibleLowerFrequency NOTIFY visibleRangeChanged)
    Q_PROPERTY(quint64 visibleUpperFrequency READ visibleUpperFrequency NOTIFY visibleRangeChanged)
    Q_PROPERTY(quint64 visibleCenterFrequency READ visibleCenterFrequency NOTIFY visibleRangeChanged)
    Q_PROPERTY(quint64 visibleSpan READ visibleSpan NOTIFY visibleRangeChanged)
    Q_PROPERTY(double displayZoomFactor READ displayZoomFactor NOTIFY visibleRangeChanged)
    Q_PROPERTY(quint64 displayZoomPercentage READ displayZoomPercentage NOTIFY visibleRangeChanged)
    Q_PROPERTY(quint64 sampleRate READ sampleRate NOTIFY sampleRateChanged)
    Q_PROPERTY(quint64 requestedCaptureBandwidth READ sampleRate NOTIFY sampleRateChanged)
    Q_PROPERTY(quint64 effectiveSampleRate READ effectiveSampleRate NOTIFY effectiveSampleRateChanged)
    Q_PROPERTY(QStringList captureBandwidthOptions READ captureBandwidthOptions NOTIFY captureBandwidthCapabilitiesChanged)
    Q_PROPERTY(bool customCaptureBandwidthSupported READ customCaptureBandwidthSupported NOTIFY captureBandwidthCapabilitiesChanged)
    Q_PROPERTY(quint64 spectrumFftSize READ spectrumFftSize NOTIFY spectrumFftSizeChanged)
    Q_PROPERTY(quint64 effectiveSpectrumFftSize READ effectiveSpectrumFftSize NOTIFY spectrumFftSizeChanged)
    Q_PROPERTY(QStringList spectrumFftSizeOptions READ spectrumFftSizeOptions CONSTANT)
    Q_PROPERTY(double spectrumHertzPerBin READ spectrumHertzPerBin NOTIFY spectrumFftSizeChanged)
    Q_PROPERTY(double effectiveWaterfallRowsPerSecond READ effectiveWaterfallRowsPerSecond NOTIFY waterfallSettingsChanged)
    Q_PROPERTY(quint32 visibleWaterfallHistorySeconds READ visibleWaterfallHistorySeconds NOTIFY waterfallSettingsChanged)
    Q_PROPERTY(QStringList visibleWaterfallHistoryOptions READ visibleWaterfallHistoryOptions CONSTANT)
    Q_PROPERTY(quint64 waterfallHistoryMemoryBudgetBytes READ waterfallHistoryMemoryBudgetBytes CONSTANT)
    Q_PROPERTY(double spectrumWaterfallSplitRatio READ spectrumWaterfallSplitRatio NOTIFY spectrumWaterfallSplitRatioChanged)
    Q_PROPERTY(QAbstractItemModel* bookmarkModel READ bookmarkModel CONSTANT)
    Q_PROPERTY(sdr::app::ApplicationLogModel* applicationLog READ applicationLog CONSTANT)
    Q_PROPERTY(QString sidebarMode READ sidebarMode NOTIFY sidebarModeChanged)
    Q_PROPERTY(bool bookmarksPanelOpen READ bookmarksPanelOpen NOTIFY sidebarModeChanged)
    Q_PROPERTY(bool settingsPanelOpen READ settingsPanelOpen NOTIFY sidebarModeChanged)
    Q_PROPERTY(bool consolePanelOpen READ consolePanelOpen NOTIFY sidebarModeChanged)
    Q_PROPERTY(double bookmarksPanelWidth READ bookmarksPanelWidth NOTIFY bookmarksPanelWidthChanged)
    Q_PROPERTY(double settingsPanelWidth READ settingsPanelWidth NOTIFY settingsPanelWidthChanged)
    Q_PROPERTY(double consolePanelWidth READ consolePanelWidth NOTIFY consolePanelWidthChanged)
    Q_PROPERTY(QString dsdFmeBinaryPath READ dsdFmeBinaryPath NOTIFY dsdFmeBinaryPathChanged)
    Q_PROPERTY(QString dsdFmeBinaryStatus READ dsdFmeBinaryStatus NOTIFY dsdFmeBinaryStatusChanged)
    Q_PROPERTY(bool dsdFmeBinaryValid READ dsdFmeBinaryValid NOTIFY dsdFmeBinaryStatusChanged)
    Q_PROPERTY(QVariantList bookmarkDemodulators READ bookmarkDemodulators CONSTANT)
    Q_PROPERTY(quint64 filterWidth READ filterWidth NOTIFY filterWidthChanged)
    Q_PROPERTY(quint64 minimumFilterWidth READ minimumFilterWidth NOTIFY filterLimitsChanged)
    Q_PROPERTY(quint64 maximumFilterWidth READ maximumFilterWidth NOTIFY filterLimitsChanged)
    Q_PROPERTY(QStringList filterWidthOptions READ filterWidthOptions NOTIFY filterPresetsChanged)
    Q_PROPERTY(double gain READ gain NOTIFY gainChanged)
    Q_PROPERTY(double requestedGain READ requestedGain NOTIFY requestedGainChanged)
    Q_PROPERTY(double ppmCorrection READ ppmCorrection NOTIFY ppmCorrectionChanged)
    Q_PROPERTY(bool ppmCorrectionSupported READ ppmCorrectionSupported NOTIFY deviceCapabilitiesChanged)
    Q_PROPERTY(bool automaticPpmCalibrationSupported READ automaticPpmCalibrationSupported NOTIFY ppmCalibrationChanged)
    Q_PROPERTY(bool ppmCalibrationRunning READ ppmCalibrationRunning NOTIFY ppmCalibrationChanged)
    Q_PROPERTY(QString ppmCalibrationStatus READ ppmCalibrationStatus NOTIFY ppmCalibrationChanged)
    Q_PROPERTY(int ppmCalibrationProgressPercent READ ppmCalibrationProgressPercent NOTIFY ppmCalibrationChanged)
    Q_PROPERTY(int demodulationModeIndex READ demodulationModeIndex NOTIFY demodulationModeChanged)
    Q_PROPERTY(QString demodulationModeName READ demodulationModeName NOTIFY demodulationModeChanged)
    Q_PROPERTY(QStringList demodulationModes READ demodulationModes CONSTANT)
    Q_PROPERTY(double squelchLevel READ squelchLevel NOTIFY squelchStateChanged)
    Q_PROPERTY(bool automaticSquelchEnabled READ automaticSquelchEnabled NOTIFY squelchStateChanged)
    Q_PROPERTY(bool squelchDisabled READ squelchDisabled NOTIFY squelchStateChanged)
    Q_PROPERTY(QString squelchStateText READ squelchStateText NOTIFY squelchStateChanged)
    Q_PROPERTY(bool receiverRunning READ receiverRunning NOTIFY receiverRunningChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString deviceState READ deviceState NOTIFY deviceStateChanged)
    Q_PROPERTY(QString backendDescription READ backendDescription NOTIFY deviceStateChanged)
    Q_PROPERTY(QStringList deviceDisplayNames READ deviceDisplayNames NOTIFY devicesChanged)
    Q_PROPERTY(int selectedDeviceIndex READ selectedDeviceIndex NOTIFY deviceStateChanged)
    Q_PROPERTY(QString deviceCapabilitySummary READ deviceCapabilitySummary NOTIFY deviceCapabilitiesChanged)
    Q_PROPERTY(bool backendReady READ backendReady NOTIFY deviceStateChanged)
    Q_PROPERTY(bool deviceDiscoveryAvailable READ deviceDiscoveryAvailable NOTIFY deviceStateChanged)
    Q_PROPERTY(bool runtimeBusy READ runtimeBusy NOTIFY runtimeBusyChanged)
    Q_PROPERTY(bool mockMode READ mockMode NOTIFY deviceStateChanged)
    Q_PROPERTY(bool gainSupported READ gainSupported NOTIFY deviceCapabilitiesChanged)
    Q_PROPERTY(double minimumGain READ minimumGain NOTIFY deviceCapabilitiesChanged)
    Q_PROPERTY(double maximumGain READ maximumGain NOTIFY deviceCapabilitiesChanged)
    Q_PROPERTY(double gainStep READ gainStep NOTIFY deviceCapabilitiesChanged)
    Q_PROPERTY(QStringList audioDeviceDisplayNames READ audioDeviceDisplayNames NOTIFY audioDevicesChanged)
    Q_PROPERTY(int selectedAudioDeviceIndex READ selectedAudioDeviceIndex NOTIFY audioStateChanged)
    Q_PROPERTY(QString audioStatusText READ audioStatusText NOTIFY audioStateChanged)
    Q_PROPERTY(QString dsdFmeStatusText READ dsdFmeStatusText NOTIFY audioStateChanged)
    Q_PROPERTY(int audioVolume READ audioVolume WRITE setAudioVolume NOTIFY audioStateChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted WRITE setAudioMuted NOTIFY audioStateChanged)
    Q_PROPERTY(bool audioReady READ audioReady NOTIFY audioStateChanged)
    Q_PROPERTY(bool audioRunning READ audioRunning NOTIFY audioStateChanged)
    Q_PROPERTY(quint64 audioOverflowEvents READ audioOverflowEvents NOTIFY audioStateChanged)
    Q_PROPERTY(quint64 audioUnderrunEvents READ audioUnderrunEvents NOTIFY audioStateChanged)
    Q_PROPERTY(quint64 tuningWheelStep READ tuningWheelStep WRITE setTuningWheelStep NOTIFY tuningWheelStepChanged)

public:
    explicit ApplicationModel(QObject* parent = nullptr);
    explicit ApplicationModel(
        std::unique_ptr<sdr::radio::ReceiverBackend> receiver,
        QObject* parent = nullptr);
    explicit ApplicationModel(
        sdr::app::ReceiverRuntime& runtime,
        QObject* parent = nullptr,
        bool verboseDiagnostics = false);

    [[nodiscard]] quint64 centerFrequency() const noexcept;
    [[nodiscard]] quint64 listeningFrequency() const noexcept;
    [[nodiscard]] QString centerFrequencyDigits() const;
    [[nodiscard]] double listeningPosition() const noexcept;
    [[nodiscard]] double filterLowerPosition() const noexcept;
    [[nodiscard]] double filterUpperPosition() const noexcept;
    [[nodiscard]] quint64 filterLowerFrequency() const noexcept;
    [[nodiscard]] quint64 filterUpperFrequency() const noexcept;
    [[nodiscard]] quint64 visibleLowerFrequency() const noexcept;
    [[nodiscard]] quint64 visibleUpperFrequency() const noexcept;
    [[nodiscard]] quint64 visibleCenterFrequency() const noexcept;
    [[nodiscard]] quint64 visibleSpan() const noexcept;
    [[nodiscard]] double displayZoomFactor() const noexcept;
    [[nodiscard]] quint64 displayZoomPercentage() const noexcept;
    [[nodiscard]] quint64 sampleRate() const noexcept;
    [[nodiscard]] quint64 effectiveSampleRate() const noexcept;
    [[nodiscard]] QStringList captureBandwidthOptions() const;
    [[nodiscard]] bool customCaptureBandwidthSupported() const noexcept;
    [[nodiscard]] quint64 spectrumFftSize() const noexcept;
    [[nodiscard]] quint64 effectiveSpectrumFftSize() const noexcept;
    [[nodiscard]] QStringList spectrumFftSizeOptions() const;
    [[nodiscard]] double spectrumHertzPerBin() const noexcept;
    [[nodiscard]] double effectiveWaterfallRowsPerSecond() const noexcept;
    [[nodiscard]] quint32 visibleWaterfallHistorySeconds() const noexcept;
    [[nodiscard]] QStringList visibleWaterfallHistoryOptions() const;
    [[nodiscard]] quint64 waterfallHistoryMemoryBudgetBytes() const noexcept;
    [[nodiscard]] double spectrumWaterfallSplitRatio() const noexcept;
    [[nodiscard]] QAbstractItemModel* bookmarkModel() noexcept;
    [[nodiscard]] sdr::app::ApplicationLogModel* applicationLog() noexcept;
    [[nodiscard]] QString sidebarMode() const;
    [[nodiscard]] bool bookmarksPanelOpen() const noexcept;
    [[nodiscard]] bool settingsPanelOpen() const noexcept;
    [[nodiscard]] bool consolePanelOpen() const noexcept;
    [[nodiscard]] double bookmarksPanelWidth() const noexcept;
    [[nodiscard]] double settingsPanelWidth() const noexcept;
    [[nodiscard]] double consolePanelWidth() const noexcept;
    [[nodiscard]] QString dsdFmeBinaryPath() const;
    [[nodiscard]] QString dsdFmeBinaryStatus() const;
    [[nodiscard]] bool dsdFmeBinaryValid() const noexcept;
    [[nodiscard]] QVariantList bookmarkDemodulators() const;
    [[nodiscard]] quint64 filterWidth() const noexcept;
    [[nodiscard]] quint64 minimumFilterWidth() const noexcept;
    [[nodiscard]] quint64 maximumFilterWidth() const noexcept;
    [[nodiscard]] QStringList filterWidthOptions() const;
    [[nodiscard]] double gain() const noexcept;
    [[nodiscard]] double requestedGain() const noexcept;
    [[nodiscard]] double ppmCorrection() const noexcept;
    [[nodiscard]] bool ppmCorrectionSupported() const noexcept;
    [[nodiscard]] bool automaticPpmCalibrationSupported() const noexcept;
    [[nodiscard]] bool ppmCalibrationRunning() const noexcept;
    [[nodiscard]] QString ppmCalibrationStatus() const;
    [[nodiscard]] int ppmCalibrationProgressPercent() const noexcept;
    [[nodiscard]] int demodulationModeIndex() const noexcept;
    [[nodiscard]] QString demodulationModeName() const;
    [[nodiscard]] QStringList demodulationModes() const;
    [[nodiscard]] double squelchLevel() const noexcept;
    [[nodiscard]] bool automaticSquelchEnabled() const noexcept;
    [[nodiscard]] bool squelchDisabled() const noexcept;
    [[nodiscard]] QString squelchStateText() const;
    [[nodiscard]] bool receiverRunning() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString deviceState() const;
    [[nodiscard]] QString backendDescription() const;
    [[nodiscard]] QStringList deviceDisplayNames() const;
    [[nodiscard]] int selectedDeviceIndex() const noexcept;
    [[nodiscard]] QString deviceCapabilitySummary() const;
    [[nodiscard]] bool backendReady() const noexcept;
    [[nodiscard]] bool deviceDiscoveryAvailable() const noexcept;
    [[nodiscard]] bool runtimeBusy() const noexcept;
    [[nodiscard]] bool mockMode() const noexcept;
    [[nodiscard]] bool verboseDiagnosticsEnabled() const noexcept;
    [[nodiscard]] bool gainSupported() const noexcept;
    [[nodiscard]] double minimumGain() const noexcept;
    [[nodiscard]] double maximumGain() const noexcept;
    [[nodiscard]] double gainStep() const noexcept;
    [[nodiscard]] QStringList audioDeviceDisplayNames() const;
    [[nodiscard]] int selectedAudioDeviceIndex() const noexcept;
    [[nodiscard]] QString audioStatusText() const;
    [[nodiscard]] QString dsdFmeStatusText() const;
    [[nodiscard]] int audioVolume() const noexcept;
    [[nodiscard]] bool audioMuted() const noexcept;
    [[nodiscard]] bool audioReady() const noexcept;
    [[nodiscard]] bool audioRunning() const noexcept;
    [[nodiscard]] quint64 audioOverflowEvents() const noexcept;
    [[nodiscard]] quint64 audioUnderrunEvents() const noexcept;
    [[nodiscard]] const std::vector<sdr::radio::FrequencyRange>&
    deviceSampleRateRanges() const noexcept;
    [[nodiscard]] quint64 tuningWheelStep() const noexcept;
    [[nodiscard]] sdr::radio::FrequencyAxisMapper frequencyViewportAxis(
        sdr::radio::FrequencyPlot plot) const noexcept;
    void setWheelClockForTests(std::function<quint64()> clock);

    void setDeviceFrequencyRanges(
        std::vector<sdr::radio::FrequencyRange> frequencyRanges);
    void clearDeviceFrequencyRanges();

public slots:
    void refreshDevices();
    void selectDeviceIndex(int index);
    void clearDeviceSelection();
    void selectAudioDeviceIndex(int index);
    void setAudioVolume(int volumePercent);
    void setAudioMuted(bool muted);
    void startReception();
    void stopReception();
    void setCenterFrequencyText(const QString& frequencyText);
    void setListeningFrequency(quint64 frequency);
    void adjustCenterFrequencyDigit(int digitIndex, int direction);
    void zeroCenterFrequencyFromDigit(int digitIndex);
    void handleFrequencyWheel(
        bool waterfall, int wheelDelta, int modifierKeys = 0);
    void handleFrequencyWheelWithDeltas(
        bool waterfall,
        int angleDelta,
        int pixelDelta,
        int modifierKeys = 0);
    void shiftCenterFromSpectrum(int wheelDelta);
    void requestWaterfallZoom(int wheelDelta);
    void selectListeningFrequencyAt(double horizontalPosition, double displayWidth);
    void setTuningWheelStep(quint64 step);
    void setSampleRate(quint64 sampleRate);
    void setCaptureBandwidthText(const QString& bandwidthText);
    void setSpectrumFftSize(quint64 fftSize);
    void setVisibleWaterfallHistorySeconds(quint32 seconds);
    void setSpectrumWaterfallSplitRatio(double ratio);
    void commitSpectrumWaterfallSplitRatio();
    void setSidebarMode(const QString& mode);
    void setBookmarksPanelOpen(bool open);
    void setBookmarksPanelWidth(double width);
    void commitBookmarksPanelWidth();
    void setSettingsPanelWidth(double width);
    void commitSettingsPanelWidth();
    void setConsolePanelWidth(double width);
    void commitConsolePanelWidth();
    void setDsdFmeBinaryPath(const QString& path);
    void setDsdFmeBinaryUrl(const QUrl& url);
    void revalidateDsdFmeBinaryPath();
    QString beginAddCurrentBookmark(int parentVisibleRow = -1);
    bool confirmAddCurrentBookmark(const QString& name);
    void cancelAddCurrentBookmark();
    void editBookmark(int visibleRow, const QVariantMap& fields);
    void renameBookmarkGroup(int visibleRow, const QString& name);
    void tuneBookmark(int visibleRow);
    void reportWaterfallHistoryMetrics(
        quint64 bytesUsed,
        quint64 retainedRows,
        double retainedSeconds,
        quint32 requestedSeconds,
        double retainedCapacitySeconds,
        quint64 storedBins,
        bool fitsMemoryBudget,
        quint64 renderedFrames,
        quint64 mergedRenderUpdates,
        double reprojectionMilliseconds,
        quint64 staleReprojectionsDiscarded,
        quint64 viewportBytesUsed,
        quint64 viewportMemoryBudget,
        quint64 viewportRows);
    void setFilterWidth(quint64 filterWidth);
    void setFilterWidthText(const QString& filterWidthText);
    void setGain(double gainDb);
    void previewGain(double gainDb);
    void commitGain(double gainDb);
    void setPpmCorrection(double ppmCorrection);
    void startAutomaticPpmCalibration();
    void cancelAutomaticPpmCalibration();
    void setDemodulationModeIndex(int modeIndex);
    void setSquelchLevel(double squelchLevelDb);
    void enableManualSquelch();
    void enableAutomaticSquelch();
    void setAutomaticSquelchEnabled(bool enabled);
    void disableSquelch();
    void setSquelchDisabled(bool disabled);

signals:
    void centerFrequencyChanged();
    void listeningFrequencyChanged();
    void centerFrequencyDigitsChanged();
    void listeningPositionChanged();
    void filterMarkerChanged();
    void visibleRangeChanged();
    void frequencyViewportChanged();
    void sampleRateChanged();
    void effectiveSampleRateChanged();
    void captureBandwidthCapabilitiesChanged();
    void spectrumFftSizeChanged();
    void waterfallSettingsChanged();
    void spectrumWaterfallSplitRatioChanged();
    void sidebarModeChanged();
    void bookmarksPanelWidthChanged();
    void settingsPanelWidthChanged();
    void consolePanelWidthChanged();
    void dsdFmeBinaryPathChanged();
    void dsdFmeBinaryStatusChanged();
    void filterWidthChanged();
    void filterLimitsChanged();
    void filterPresetsChanged();
    void gainChanged();
    void requestedGainChanged();
    void ppmCorrectionChanged();
    void ppmCalibrationChanged();
    void demodulationModeChanged();
    void squelchStateChanged();
    void receiverRunningChanged();
    void statusTextChanged();
    void devicesChanged();
    void deviceStateChanged();
    void deviceCapabilitiesChanged();
    void runtimeBusyChanged();
    void audioDevicesChanged();
    void audioStateChanged();
    void tuningWheelStepChanged();
    void spectrumReset();
    void waterfallReset();
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

private:
    void applyOperation(
        const sdr::radio::ReceiverState& previousState,
        sdr::radio::OperationResult result);
    void applyRuntimeSnapshot(
        const sdr::app::ReceiverRuntimeSnapshot& snapshot);
    void notifyStateChanges(
        const sdr::radio::ReceiverState& previousState,
        const sdr::radio::ReceiverState& state,
        bool operationSucceeded);
    void receiveRuntimeSpectrumFrame(
        const QVector<float>& normalizedMagnitudes,
        quint64 frameCenterFrequency,
        quint64 frameSampleRate,
        quint64 frameFftSize,
        quint64 frameSequence,
        quint64 frameTimestampNanoseconds,
        quint64 frameTuningGeneration);
    void receiveRuntimeWaterfallFrame(
        const QVector<float>& normalizedMagnitudes,
        quint64 frameCenterFrequency,
        quint64 frameSampleRate,
        quint64 frameFftSize,
        quint64 frameSequence,
        quint64 frameTimestampNanoseconds,
        quint64 frameTuningGeneration);
    [[nodiscard]] const sdr::radio::ReceiverState& receiverState() const noexcept;
    [[nodiscard]] const sdr::radio::ReceiverLimits& receiverLimits() const noexcept;
    [[nodiscard]] const sdr::radio::ReceiverCapabilities&
    receiverCapabilities() const noexcept;
    [[nodiscard]] sdr::radio::ReceiverState displayState() const noexcept;
    [[nodiscard]] sdr::radio::FrequencyRange advertisedRfRangeForCenter(
        std::uint64_t centerFrequency) const noexcept;
    [[nodiscard]] std::vector<sdr::radio::FrequencyRange>
    effectiveCenterFrequencyRanges() const;
    enum class PendingTuningWheelAction {
        None,
        Center,
        Listening,
    };
    enum class PendingViewportWheelAction {
        None,
        Zoom,
    };
    void applyCenterFrequencyEdit(const sdr::app::FrequencyEditResult& edit);
    void initializeWheelTuningCoalescing();
    void handleSpectrumWheelDeltas(
        int angleDelta, int pixelDelta, quint64 timestampNanoseconds);
    void shiftCenterFromSpectrumWithMultiplier(
        qint64 wheelDelta, int accelerationMultiplier);
    void resetCenterWheelAcceleration();
    void updateCenterWheelAcceleration(
        int direction, quint64 timestampNanoseconds);
    [[nodiscard]] quint64 wheelTimestampNanoseconds() const noexcept;
    void shiftListeningFromWheel(int wheelDelta);
    void previewCenterWheelShift(qint64 requestedStep);
    void clearCenterWheelPreview(bool restoreConfirmedViewport);
    void finishCenterWheelRequest(quint64 frequency, bool succeeded);
    void queueTuningWheelAction(
        PendingTuningWheelAction action, qint64 requestedStep);
    void flushPendingWheelTuning();
    void queueViewportWheelAction(
        PendingViewportWheelAction action, int wheelDelta);
    void discardPendingViewportWheelAction();
    void flushPendingViewportWheelAction();
    void requestFilterWidthAdjustment(int wheelDelta);
    void flushPendingFilterWidthAdjustment();
    void emitFrequencyViewportChanges();
    [[nodiscard]] bool canPublishSpectrumFrame(
        quint64 magnitudeCount,
        quint64 frameCenterFrequency,
        quint64 frameSampleRate,
        quint64 frameFftSize,
        quint64 frameSequence,
        quint64 frameTimestampNanoseconds) const noexcept;
    void recordPublishedSpectrumFrame(
        quint64 frameSequence,
        quint64 frameTimestampNanoseconds) noexcept;
    void resetSpectrumFrame();
    void publishLatestSpectrumFrame();
    void restorePersistedRuntimeControls();
    void restorePersistedDisplaySettings();
    void persistSpectrumWaterfallSplitRatio();
    void persistBookmarksPanelWidth();
    void persistSettingsPanelWidth();
    void persistConsolePanelWidth();
    void setStatusText(QString statusText);

    std::unique_ptr<sdr::radio::ReceiverBackend> m_receiver;
    sdr::app::ReceiverRuntime* m_runtime = nullptr;
    sdr::radio::ReceiverState m_runtimeState;
    sdr::radio::ReceiverLimits m_runtimeLimits;
    sdr::radio::ReceiverCapabilities m_runtimeCapabilities;
    quint64 m_runtimeEffectiveSampleRate = 2'000'000;
    quint64 m_spectrumFftSize = 4'096;
    quint64 m_effectiveSpectrumFftSize = 4'096;
    quint64 m_lastSpectrumFrameSequence = 0;
    quint64 m_lastSpectrumFrameTimestampNanoseconds = 0;
    double m_spectrumHertzPerBin = 0.0;
    double m_effectiveWaterfallRowsPerSecond = 60.0;
    quint32 m_visibleWaterfallHistorySeconds = 10;
    double m_spectrumWaterfallSplitRatio = 0.5;
    sdr::app::ApplicationLogModel m_applicationLog;
    sdr::app::BookmarkTreeModel m_bookmarkModel;
    struct PendingBookmarkCreation {
        int parentVisibleRow = -1;
        sdr::app::BookmarkData bookmark;
    };
    std::optional<PendingBookmarkCreation> m_pendingBookmarkCreation;
    QString m_sidebarMode = QStringLiteral("none");
    double m_bookmarksPanelWidth = 280.0;
    double m_settingsPanelWidth = 320.0;
    double m_consolePanelWidth = 420.0;
    QString m_dsdFmeBinaryPath;
    QString m_dsdFmeBinaryStatus;
    bool m_dsdFmeBinaryValid = false;
    QTimer m_spectrumTimer;
    QTimer m_splitRatioPersistenceTimer;
    QTimer m_bookmarksPanelWidthPersistenceTimer;
    QTimer m_settingsPanelWidthPersistenceTimer;
    QTimer m_consolePanelWidthPersistenceTimer;
    QTimer m_wheelTuningTimer;
    QTimer m_viewportWheelTimer;
    QTimer m_filterWidthWheelTimer;
    qint64 m_pendingWheelTuningShift = 0;
    qint64 m_centerWheelDeltaRemainder = 0;
    qint64 m_centerPixelDeltaRemainder = 0;
    int m_centerWheelDirection = 0;
    int m_centerWheelAccelerationMultiplier = 1;
    std::optional<quint64> m_centerWheelLastEventNanoseconds;
    std::function<quint64()> m_wheelClockForTests;
    int m_pendingViewportWheelDelta = 0;
    int m_pendingFilterWidthWheelDelta = 0;
    int m_filterWidthWheelRemainder = 0;
    PendingTuningWheelAction m_pendingTuningWheelAction =
        PendingTuningWheelAction::None;
    PendingViewportWheelAction m_pendingViewportWheelAction =
        PendingViewportWheelAction::None;
    std::optional<quint64> m_pendingCenterWheelFrequency;
    std::optional<quint64> m_pendingListeningWheelFrequency;
    std::optional<std::vector<sdr::radio::FrequencyRange>> m_deviceFrequencyRanges;
    std::vector<sdr::radio::FrequencyRange> m_deviceSampleRateRanges;
    QStringList m_captureBandwidthOptions;
    bool m_customCaptureBandwidthSupported = false;
    QStringList m_deviceIdentifiers;
    QStringList m_deviceDisplayNames;
    QString m_selectedDeviceIdentifier;
    QStringList m_audioDeviceIdentifiers;
    QStringList m_audioDeviceDisplayNames;
    QString m_selectedAudioDeviceIdentifier;
    QString m_audioStatusText = QStringLiteral("Audio output is initializing");
    QString m_dsdFmeStatusText = QStringLiteral("DSD-FME not configured");
    QString m_deviceState = QStringLiteral("Mock device");
    QString m_backendDescription = QStringLiteral("Mock backend - no SDR hardware");
    QString m_deviceCapabilitySummary = QStringLiteral("Mock receiver capabilities");
    double m_minimumGainDb = -10.0;
    double m_maximumGainDb = 100.0;
    double m_gainStepDb = 1.0;
    double m_requestedGainDb = 20.0;
    bool m_gainSupported = true;
    bool m_backendReady = true;
    bool m_deviceDiscoveryAvailable = false;
    bool m_runtimeBusy = false;
    bool m_mockMode = true;
    bool m_automaticPpmCalibrationSupported = false;
    bool m_ppmCalibrationRunning = false;
    QString m_ppmCalibrationStatus = QStringLiteral("idle");
    int m_ppmCalibrationProgressPercent = 0;
    quint64 m_ppmCalibrationDisplayResetGeneration = 0;
    int m_audioVolumePercent = 75;
    bool m_audioMuted = false;
    bool m_audioReady = false;
    bool m_audioRunning = false;
    quint64 m_audioOverflowEvents = 0;
    quint64 m_audioUnderrunEvents = 0;
    quint64 m_tuningWheelStep = 10'000;
    sdr::app::FrequencyViewport m_frequencyViewport;
    quint64 m_waterfallZoomEvents = 0;
    quint64 m_coalescedWaterfallZoomEvents = 0;
    QElapsedTimer m_waterfallHistoryMetricsTimer;
    quint64 m_lastRenderedWaterfallFrames = 0;
    quint64 m_lastMergedWaterfallUpdates = 0;
    bool m_lastWaterfallHistoryFit = true;
    quint32 m_lastWaterfallHistoryRequestedSeconds = 0;
    quint64 m_lastWaterfallHistoryStoredBins = 0;
    bool m_verboseDiagnostics = false;
    QString m_statusText = QStringLiteral("Mock backend ready - no hardware device");
};
