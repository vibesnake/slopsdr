// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "FrequencyAlignedDisplay.hpp"
#include "FilterIndicator.hpp"
#include "SpectrumAverager.hpp"

#include <QImage>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>

class ApplicationModel;

class SpectrumWaterfallItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(SpectrumWaterfallView)
    Q_PROPERTY(QObject* applicationModel READ applicationModel WRITE setApplicationModel NOTIFY applicationModelChanged)
    Q_PROPERTY(bool waterfall READ waterfall WRITE setWaterfall NOTIFY waterfallChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(QString waterfallPaletteName READ waterfallPaletteName CONSTANT)
    Q_PROPERTY(quint64 historyMemoryBudgetBytes READ historyMemoryBudgetBytes WRITE setHistoryMemoryBudgetBytes NOTIFY historyMetricsChanged)
    Q_PROPERTY(quint64 viewportHistoryMemoryBudgetBytes READ viewportHistoryMemoryBudgetBytes WRITE setViewportHistoryMemoryBudgetBytes NOTIFY historyMetricsChanged)
    Q_PROPERTY(double effectiveRowsPerSecond READ effectiveRowsPerSecond WRITE setEffectiveRowsPerSecond NOTIFY historyMetricsChanged)
    Q_PROPERTY(double visibleHistorySeconds READ visibleHistorySeconds WRITE setVisibleHistorySeconds NOTIFY historyMetricsChanged)
    Q_PROPERTY(quint64 historyMemoryUsageBytes READ historyMemoryUsageBytes NOTIFY historyMetricsChanged)
    Q_PROPERTY(quint64 viewportHistoryMemoryUsageBytes READ viewportHistoryMemoryUsageBytes NOTIFY historyMetricsChanged)
    Q_PROPERTY(quint64 storedHistoryBins READ storedHistoryBins NOTIFY historyMetricsChanged)
    Q_PROPERTY(double retainedHistoryCapacitySeconds READ retainedHistoryCapacitySeconds NOTIFY historyMetricsChanged)
    Q_PROPERTY(double retainedHistorySeconds READ retainedHistorySeconds NOTIFY historyMetricsChanged)
    Q_PROPERTY(bool historyConfigurationFitsMemoryBudget READ historyConfigurationFitsMemoryBudget NOTIFY historyMetricsChanged)
    Q_PROPERTY(float waterfallMinimumDbfs READ waterfallMinimumDbfs WRITE setWaterfallMinimumDbfs NOTIFY waterfallRangeChanged)
    Q_PROPERTY(float waterfallMaximumDbfs READ waterfallMaximumDbfs WRITE setWaterfallMaximumDbfs NOTIFY waterfallRangeChanged)
    Q_PROPERTY(float spectrumMinimumDbfs READ spectrumMinimumDbfs WRITE setSpectrumMinimumDbfs NOTIFY spectrumRangeChanged)
    Q_PROPERTY(float spectrumMaximumDbfs READ spectrumMaximumDbfs WRITE setSpectrumMaximumDbfs NOTIFY spectrumRangeChanged)
    Q_PROPERTY(QVariantList majorDbfsTicks READ majorDbfsTicks NOTIFY spectrumRangeChanged)
    Q_PROPERTY(QVariantList minorDbfsTicks READ minorDbfsTicks NOTIFY spectrumRangeChanged)
    Q_PROPERTY(float noiseFloorDbfs READ noiseFloorDbfs NOTIFY noiseFloorChanged)
    Q_PROPERTY(bool noiseFloorAvailable READ noiseFloorAvailable NOTIFY noiseFloorChanged)
    Q_PROPERTY(bool maximumHoldEnabled READ maximumHoldEnabled WRITE setMaximumHoldEnabled NOTIFY maximumHoldEnabledChanged)
    Q_PROPERTY(int spectrumAveragingStrength READ spectrumAveragingStrength WRITE setSpectrumAveragingStrength NOTIFY spectrumAveragingStrengthChanged)
    Q_PROPERTY(bool filterWidthAdjustmentActive READ filterWidthAdjustmentActive WRITE setFilterWidthAdjustmentActive NOTIFY filterWidthAdjustmentActiveChanged)
    Q_PROPERTY(QString waterfallAggregation READ waterfallAggregation WRITE setWaterfallAggregation NOTIFY waterfallAggregationChanged)

public:
    explicit SpectrumWaterfallItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* applicationModel() const noexcept;
    void setApplicationModel(QObject* applicationModel);

    [[nodiscard]] bool waterfall() const noexcept;
    void setWaterfall(bool waterfall);
    [[nodiscard]] bool paused() const noexcept;
    void setPaused(bool paused);
    [[nodiscard]] QString waterfallPaletteName() const;

    [[nodiscard]] quint64 historyMemoryBudgetBytes() const noexcept;
    void setHistoryMemoryBudgetBytes(quint64 bytes);
    [[nodiscard]] quint64 viewportHistoryMemoryBudgetBytes() const noexcept;
    void setViewportHistoryMemoryBudgetBytes(quint64 bytes);
    [[nodiscard]] double effectiveRowsPerSecond() const noexcept;
    void setEffectiveRowsPerSecond(double rowsPerSecond);
    [[nodiscard]] double visibleHistorySeconds() const noexcept;
    void setVisibleHistorySeconds(double seconds);
    [[nodiscard]] quint64 historyMemoryUsageBytes() const noexcept;
    [[nodiscard]] quint64 viewportHistoryMemoryUsageBytes() const noexcept;
    [[nodiscard]] quint64 storedHistoryBins() const noexcept;
    [[nodiscard]] double retainedHistoryCapacitySeconds() const noexcept;
    [[nodiscard]] double retainedHistorySeconds() const noexcept;
    [[nodiscard]] bool historyConfigurationFitsMemoryBudget() const noexcept;

    [[nodiscard]] float waterfallMinimumDbfs() const noexcept;
    void setWaterfallMinimumDbfs(float minimumDbfs);
    [[nodiscard]] float waterfallMaximumDbfs() const noexcept;
    void setWaterfallMaximumDbfs(float maximumDbfs);
    [[nodiscard]] float spectrumMinimumDbfs() const noexcept;
    void setSpectrumMinimumDbfs(float minimumDbfs);
    [[nodiscard]] float spectrumMaximumDbfs() const noexcept;
    void setSpectrumMaximumDbfs(float maximumDbfs);
    [[nodiscard]] QVariantList majorDbfsTicks() const;
    [[nodiscard]] QVariantList minorDbfsTicks() const;
    [[nodiscard]] float noiseFloorDbfs() const noexcept;
    [[nodiscard]] bool noiseFloorAvailable() const noexcept;
    [[nodiscard]] bool maximumHoldEnabled() const noexcept;
    void setMaximumHoldEnabled(bool enabled);
    [[nodiscard]] int spectrumAveragingStrength() const noexcept;
    void setSpectrumAveragingStrength(int strength);
    [[nodiscard]] bool filterWidthAdjustmentActive() const noexcept;
    void setFilterWidthAdjustmentActive(bool active);
    [[nodiscard]] const QVector<float>& maximumHoldDbfs() const noexcept;
    [[nodiscard]] bool spectrumHoldsAvailable() const noexcept;
    [[nodiscard]] QString waterfallAggregation() const;
    void setWaterfallAggregation(const QString& aggregation);
    [[nodiscard]] QRgb waterfallColorForNormalizedMagnitude(float magnitude) const noexcept;
    [[nodiscard]] QRgb spectrumColorForDbfs(float dbfs) const noexcept;
    [[nodiscard]] QRgb emptyWaterfallColor() const noexcept;

    Q_INVOKABLE float recommendedAmplitudeScaleMargin(
        float panelWidth,
        float devicePixelRatio) const noexcept;
    Q_INVOKABLE float yForDbfs(float dbfs, float displayHeight) const noexcept;
    Q_INVOKABLE float xForFrequency(quint64 frequency) const noexcept;
    [[nodiscard]] QRectF frequencyPlotRect() const noexcept;
    [[nodiscard]] std::uint64_t waterfallReprojectionCount() const noexcept;

signals:
    void applicationModelChanged();
    void waterfallChanged();
    void pausedChanged();
    void historyMetricsChanged();
    void waterfallRangeChanged();
    void spectrumRangeChanged();
    void noiseFloorChanged();
    void maximumHoldEnabledChanged();
    void spectrumAveragingStrengthChanged();
    void filterWidthAdjustmentActiveChanged();
    void waterfallAggregationChanged();

protected:
    [[nodiscard]] QSGNode* updatePaintNode(
        QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(
        const QRectF& newGeometry,
        const QRectF& oldGeometry) override;

private slots:
    void receiveFrame(const QVector<float>& normalizedMagnitudes);
    void receiveSpectrumFrame(
        const QVector<float>& normalizedMagnitudes,
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize,
        quint64 sequence,
        quint64 timestampNanoseconds,
        quint64 tuningGeneration);
    void receiveWaterfallFrame(
        const QVector<float>& normalizedMagnitudes,
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize,
        quint64 sequence,
        quint64 timestampNanoseconds,
        quint64 tuningGeneration);
    void clearSpectrumFrame();
    void resetSpectrumHolds();
    void clearWaterfallFrames();
    void frequencyAxisChanged();

private:
    friend class SpectrumWaterfallItemTest;

    [[nodiscard]] QSGNode* updateSpectrumNode(QSGNode* oldNode);
    [[nodiscard]] QSGNode* updateWaterfallNode(QSGNode* oldNode);
    void updateFilterIndicatorNode(QSGNode* node);
    [[nodiscard]] QVector<float> displayFrame(
        const sdr::radio::SpectrumFrame& frame) const;
    [[nodiscard]] QVector<float> displayFrame(
        const sdr::gui::WaterfallHistoryRow& frame) const;
    [[nodiscard]] QVector<float> displayFrame(
        const sdr::gui::ViewportWaterfallHistoryRow& frame) const;
    [[nodiscard]] sdr::radio::FrequencyAxisMapper frequencyAxis() const noexcept;
    [[nodiscard]] int displayColumnCount() const noexcept;
    [[nodiscard]] float spectrumDisplayLevel(float magnitude) const noexcept;
    [[nodiscard]] float waterfallDbfs(float magnitude) const noexcept;
    [[nodiscard]] float waterfallDbfsForLinearPower(float power) const noexcept;
    [[nodiscard]] float yForDbfs(float dbfs) const noexcept;
    [[nodiscard]] bool rebuildWaterfallImage(
        std::uint64_t mappingAnchorTimestampNanoseconds,
        const sdr::gui::WaterfallRasterGeometry& geometry);
    void reportScrollDiagnostics(float verticalPhase);
    void scheduleRasterResize();
    void commitRasterResize();
    void refreshRasterScreenConnection();
    void rebaseWaterfallRenderAnchor(
        double fractionalPhase,
        std::uint64_t nowNanoseconds);
    void updateHistoryConfiguration(std::size_t sourceBins = 0);
    [[nodiscard]] sdr::gui::WaterfallViewportDescriptor
    currentWaterfallViewport(
        const sdr::radio::SpectrumFrame& capture) const noexcept;
    void synchronizeWaterfallViewport(
        const sdr::radio::SpectrumFrame& capture);
    void invalidateWaterfallViewport();
    void updateNoiseFloor(std::span<const float> normalizedMagnitudes);
    void resetSpectrumAverage();
    void updateSpectrumHolds(
        const QVector<float>& normalizedMagnitudes,
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize,
        quint64 sequence,
        quint64 timestampNanoseconds);
    [[nodiscard]] bool frameMatchesCurrentSpectrumGeometry(
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize) const noexcept;
    void projectSpectrumHolds();
    void setLatestFrame(
        std::span<const float> normalizedMagnitudes,
        quint64 centerFrequency,
        quint64 sampleRate,
        quint64 fftSize,
        quint64 sequence,
        quint64 timestampNanoseconds,
        quint64 tuningGeneration);
    void reportHistoryMetrics();

    QPointer<ApplicationModel> m_applicationModel;
    QMetaObject::Connection m_frameConnection;
    QMetaObject::Connection m_waterfallFrameConnection;
    QMetaObject::Connection m_resetConnection;
    QMetaObject::Connection m_waterfallResetConnection;
    QMetaObject::Connection m_centerFrequencyConnection;
    QMetaObject::Connection m_frequencyViewportConnection;
    QMetaObject::Connection m_listeningFrequencyConnection;
    QMetaObject::Connection m_filterMarkerConnection;
    QMetaObject::Connection m_filterWidthConnection;
    QMetaObject::Connection m_demodulationModeConnection;
    QMetaObject::Connection m_scannerConnection;
    QMetaObject::Connection m_requestedGainConnection;
    QMetaObject::Connection m_effectiveGainConnection;
    QMetaObject::Connection m_effectiveSampleRateConnection;
    QMetaObject::Connection m_spectrumFftSizeConnection;
    QMetaObject::Connection m_receiverRunningConnection;
    QMetaObject::Connection m_deviceStateConnection;
    QMetaObject::Connection m_windowScreenConnection;
    QMetaObject::Connection m_screenDpiConnection;
    sdr::radio::SpectrumFrame m_latestFrame;
    sdr::gui::WaterfallHistoryBuffer m_waterfallHistory{128};
    sdr::gui::ViewportWaterfallHistoryBuffer m_viewportWaterfallHistory;
    QVector<float> m_noiseScratch;
    QVector<float> m_maximumHoldDbfs;
    QVector<float> m_projectedMaximumHoldDbfs;
    sdr::gui::SpectrumAverager m_spectrumAverager;
    QImage m_waterfallImage;
    bool m_waterfall = false;
    bool m_paused = false;
    bool m_waterfallClearedForScannerPause = false;
    bool m_frameDirty = false;
    bool m_projectionDirty = false;
    int m_pendingWaterfallRows = 0;
    bool m_modeChanged = false;
    float m_waterfallMinimumDbfs = -120.0F;
    float m_waterfallMaximumDbfs = -20.0F;
    float m_spectrumMinimumDbfs = -120.0F;
    float m_spectrumMaximumDbfs = -20.0F;
    float m_noiseFloorDbfs = -120.0F;
    bool m_noiseFloorAvailable = false;
    bool m_maximumHoldEnabled = false;
    bool m_filterWidthAdjustmentActive = false;
    bool m_holdProjectionDirty = false;
    quint64 m_holdCenterFrequency = 0;
    quint64 m_holdSampleRate = 0;
    quint64 m_holdFftSize = 0;
    quint64 m_holdLastSequence = 0;
    quint64 m_holdLastTimestampNanoseconds = 0;
    double m_observedRequestedGainDb = 0.0;
    double m_observedEffectiveGainDb = 0.0;
    bool m_observedReceiverRunning = false;
    int m_observedSelectedDeviceIndex = -1;
    bool m_observedBackendReady = false;
    double m_effectiveRowsPerSecond = 60.0;
    double m_visibleHistorySeconds = 10.0;
    double m_retainedHistoryDurationSeconds = 10.0;
    std::uint64_t m_fallbackSequence = 1;
    std::uint64_t m_waterfallViewportGeneration = 1;
    std::uint64_t m_completedWaterfallViewportGeneration = 0;
    sdr::gui::WaterfallViewportDescriptor m_waterfallViewport;
    std::uint64_t m_lastHighResolutionRasterRows = 0;
    std::uint64_t m_lastCompactRasterRows = 0;
    std::uint64_t m_waterfallReprojectionCount = 0;
    std::uint64_t m_staleReprojectionsDiscarded = 0;
    double m_lastReprojectionMilliseconds = 0.0;
    std::uint64_t m_renderClockOriginNanoseconds = 0;
    std::uint64_t m_initialRenderTimestampNanoseconds = 0;
    std::uint64_t m_scrollPhaseResets = 0;
    std::uint64_t m_lastRenderFrameTimestampNanoseconds = 0;
    std::uint64_t m_lastRenderFrameIntervalNanoseconds = 0;
    std::uint64_t m_renderedFrames = 0;
    std::uint64_t m_mergedRenderUpdates = 0;
    std::unordered_map<std::uint64_t, QVector<float>> m_projectedRowCache;
    std::unordered_map<std::uint64_t, QVector<float>>
        m_projectedViewportRowCache;
    std::unordered_set<std::uint64_t> m_retainedWaterfallSequences;
    std::unordered_map<
        std::uint64_t,
        const sdr::gui::ViewportWaterfallHistoryRow*>
        m_matchingViewportRows;
    int m_cachedProjectionColumns = 0;
    int m_stagingPixelRows = 1;
    sdr::gui::WaterfallRasterGeometry m_rasterGeometry;
    bool m_scrollRasterDirty = false;
    std::size_t m_historySourceBins = 0;
    sdr::gui::WaterfallHistoryPlan m_historyPlan;
    QElapsedTimer m_scrollDiagnosticsTimer;
    QTimer m_scrollTimer;
    QTimer m_resizeCoalesceTimer;
    sdr::gui::WaterfallAggregation m_waterfallAggregation =
        sdr::gui::WaterfallAggregation::Original;
};
