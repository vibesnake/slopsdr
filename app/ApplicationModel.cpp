// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationModel.hpp"

#include "DemodulatorRegistry.hpp"
#include "FrequencyMapping.hpp"
#include "FrequencyDigitController.hpp"
#include "MockReceiverBackend.hpp"
#include "ReceiverControlSettings.hpp"

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr quint64 maximumTuningWheelStep = 1'000'000'000;
constexpr int displayPollIntervalMilliseconds = 33;
constexpr int wheelTuningCoalescingMilliseconds = 40;
constexpr int displayWheelCoalescingMilliseconds = 16;
constexpr double wheelDeltaPerStep = 120.0;
constexpr qint64 touchpadPixelsPerWheelStep = 40;
constexpr qint64 maximumTouchpadPixelsPerEvent = 120;
constexpr quint64 wheelAccelerationResetNanoseconds = 300'000'000;
constexpr quint64 wheelAccelerationModerateNanoseconds = 120'000'000;
constexpr quint64 wheelAccelerationFastNanoseconds = 60'000'000;
constexpr quint64 wheelAccelerationVeryFastNanoseconds = 30'000'000;
constexpr int minimumDiscreteWheelTriggerDelta = 100;
constexpr quint64 waterfallHistoryMemoryBudget = 16ULL * 1'024ULL * 1'024ULL;
constexpr auto spectrumWaterfallSplitRatioSetting =
    "display/spectrumWaterfallSplitRatio";
constexpr double defaultSpectrumWaterfallSplitRatio = 0.5;
constexpr double minimumSpectrumWaterfallSplitRatio = 0.2;
constexpr double maximumSpectrumWaterfallSplitRatio = 0.8;
constexpr int splitRatioPersistenceDelayMilliseconds = 150;
constexpr auto sidebarModeSetting = "display/sidebarMode";
constexpr auto legacyBookmarksPanelOpenSetting = "display/bookmarksPanelOpen";
constexpr auto bookmarksPanelWidthSetting = "display/bookmarksPanelWidth";
constexpr auto scanPanelWidthSetting = "display/scanPanelWidth";
constexpr auto settingsPanelWidthSetting = "display/settingsPanelWidth";
constexpr auto consolePanelWidthSetting = "display/consolePanelWidth";
constexpr auto scanLowerFrequencySetting = "scanner/lowerFrequencyHz";
constexpr auto scanUpperFrequencySetting = "scanner/upperFrequencyHz";
constexpr auto scanStepSizeSetting = "scanner/stepSizeHz";
constexpr auto scanDwellMillisecondsSetting = "scanner/dwellMilliseconds";
constexpr auto scanResumeDelayMillisecondsSetting =
    "scanner/resumeDelayMilliseconds";
constexpr auto scanTypeSetting = "scanner/scanType";
constexpr auto scanPresetsSetting = "scanner/presets";
constexpr auto currentPassbandScanType = "currentPassband";
constexpr auto wideRangeScanType = "wideRange";
constexpr auto bookmarkScanDwellMillisecondsSetting =
    "bookmarkScanner/dwellMilliseconds";
constexpr auto bookmarkScanResumeDelayMillisecondsSetting =
    "bookmarkScanner/resumeDelayMilliseconds";
constexpr int currentPassbandScanTypeIndex = 0;
constexpr int wideRangeScanTypeIndex = 1;
constexpr int scannerTunerSettlingMilliseconds = 100;
constexpr quint64 scannerCaptureEdgeGuardDivisor = 100;

QString scanTypeName(int scanTypeIndex)
{
    if (scanTypeIndex == wideRangeScanTypeIndex) {
        return QString::fromLatin1(wideRangeScanType);
    }
    return QString::fromLatin1(currentPassbandScanType);
}
constexpr auto dsdFmeBinaryPathSetting = "externalDecoder/dsdFmeBinaryPath";
constexpr double defaultBookmarksPanelWidth = 280.0;
constexpr double defaultScanPanelWidth = 320.0;
constexpr double defaultSettingsPanelWidth = 320.0;
constexpr double defaultConsolePanelWidth = 420.0;
constexpr double minimumBookmarksPanelWidth = 220.0;
constexpr double maximumBookmarksPanelWidth = 520.0;
constexpr int bookmarksPanelPersistenceDelayMilliseconds = 150;

double normalizedSpectrumWaterfallSplitRatio(double ratio) noexcept
{
    if (!std::isfinite(ratio) || ratio <= 0.0 || ratio >= 1.0) {
        return defaultSpectrumWaterfallSplitRatio;
    }
    return std::clamp(
        ratio,
        minimumSpectrumWaterfallSplitRatio,
        maximumSpectrumWaterfallSplitRatio);
}

double normalizedBookmarksPanelWidth(double width) noexcept
{
    if (!std::isfinite(width)) {
        return defaultBookmarksPanelWidth;
    }
    return std::clamp(
        width, minimumBookmarksPanelWidth, maximumBookmarksPanelWidth);
}

double normalizedSettingsPanelWidth(double width) noexcept
{
    if (!std::isfinite(width)) {
        return defaultSettingsPanelWidth;
    }
    return std::clamp(
        width, minimumBookmarksPanelWidth, maximumBookmarksPanelWidth);
}

double normalizedScanPanelWidth(double width) noexcept
{
    if (!std::isfinite(width)) {
        return defaultScanPanelWidth;
    }
    return std::clamp(
        width, minimumBookmarksPanelWidth, maximumBookmarksPanelWidth);
}

double normalizedConsolePanelWidth(double width) noexcept
{
    if (!std::isfinite(width)) {
        return defaultConsolePanelWidth;
    }
    return std::clamp(
        width, minimumBookmarksPanelWidth, maximumBookmarksPanelWidth);
}

QString normalizedSidebarMode(const QString& mode)
{
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QLatin1String("bookmarks") ||
        normalized == QLatin1String("scan") ||
        normalized == QLatin1String("settings") ||
        normalized == QLatin1String("console")) {
        return normalized;
    }
    return QStringLiteral("none");
}

QString normalizedDsdFmeBinaryPath(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || trimmed == QLatin1String("~")) {
        return trimmed;
    }
    if (trimmed.startsWith(QLatin1String("~/"))) {
        return QStringLiteral("~/") + QDir::cleanPath(trimmed.mid(2));
    }
    return QDir::cleanPath(trimmed);
}

std::optional<quint64> persistedUnsignedInteger(const QVariant& storedValue)
{
    if (!storedValue.isValid()) {
        return std::nullopt;
    }
    const QString text = storedValue.toString().trimmed();
    bool valid = false;
    const quint64 value = text.toULongLong(&valid, 10);
    return valid ? std::optional<quint64>(value) : std::nullopt;
}

std::optional<int> persistedInteger(const QVariant& storedValue)
{
    if (!storedValue.isValid()) {
        return std::nullopt;
    }
    const QString text = storedValue.toString().trimmed();
    bool valid = false;
    const qlonglong value = text.toLongLong(&valid, 10);
    if (!valid || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<quint64> persistedUnsignedInteger(
    const QSettings& settings,
    const char* key)
{
    const QString setting = QString::fromLatin1(key);
    return settings.contains(setting)
               ? persistedUnsignedInteger(settings.value(setting))
               : std::nullopt;
}

std::optional<int> persistedInteger(const QSettings& settings, const char* key)
{
    const QString setting = QString::fromLatin1(key);
    return settings.contains(setting) ? persistedInteger(settings.value(setting))
                                      : std::nullopt;
}

QString expandedDsdFmeBinaryPath(const QString& userPath)
{
    QString expanded = userPath;
    if (expanded == QLatin1String("~")) {
        expanded = QDir::homePath();
    } else if (expanded.startsWith(QLatin1String("~/"))) {
        expanded = QDir::homePath() + expanded.mid(1);
    }
    if (QDir::isRelativePath(expanded)) {
        expanded = QDir::current().absoluteFilePath(expanded);
    }
    return QDir::cleanPath(expanded);
}

QString dsdFmeBinaryStatusForPath(const QString& userPath, bool* valid)
{
    *valid = false;
    if (userPath.isEmpty()) {
        return QStringLiteral("No DSD-FME binary configured");
    }
    const QFileInfo info(expandedDsdFmeBinaryPath(userPath));
    if (!info.exists()) {
        return QStringLiteral("File not found");
    }
    if (!info.isFile()) {
        return QStringLiteral("Not a regular file");
    }
    if (!info.isExecutable()) {
        return QStringLiteral("Not executable");
    }
    *valid = true;
    return QStringLiteral("Valid executable");
}

std::pair<qint64, qint64> bookmarkFilterEdges(
    sdr::radio::DemodulationMode mode, quint64 filterWidth) noexcept
{
    const qint64 width = static_cast<qint64>(std::min<quint64>(
        filterWidth,
        static_cast<quint64>(std::numeric_limits<qint64>::max())));
    if (mode == sdr::radio::DemodulationMode::Usb) {
        return {0, width};
    }
    if (mode == sdr::radio::DemodulationMode::Lsb) {
        return {-width, 0};
    }
    const qint64 lower = width / 2;
    return {-lower, width - lower};
}

std::optional<quint64> bookmarkFilterWidth(
    sdr::radio::DemodulationMode mode, qint64 lowHz, qint64 highHz) noexcept
{
    if (mode == sdr::radio::DemodulationMode::Usb) {
        return lowHz == 0 && highHz > 0
                   ? std::optional<quint64>(static_cast<quint64>(highHz))
                   : std::nullopt;
    }
    if (mode == sdr::radio::DemodulationMode::Lsb) {
        return highHz == 0 && lowHz < 0 &&
                       lowHz != std::numeric_limits<qint64>::min()
                   ? std::optional<quint64>(static_cast<quint64>(-lowHz))
                   : std::nullopt;
    }
    if (lowHz > 0 || highHz <= 0 ||
        lowHz == std::numeric_limits<qint64>::min()) {
        return std::nullopt;
    }
    const quint64 lower = static_cast<quint64>(-lowHz);
    const quint64 upper = static_cast<quint64>(highHz);
    if (lower > std::numeric_limits<quint64>::max() - upper ||
        (lower > upper ? lower - upper : upper - lower) > 1) {
        return std::nullopt;
    }
    return lower + upper;
}

QString modeName(sdr::radio::DemodulationMode mode)
{
    using sdr::radio::DemodulationMode;
    switch (mode) {
    case DemodulationMode::Am:
        return QStringLiteral("AM");
    case DemodulationMode::Nfm:
        return QStringLiteral("NFM");
    case DemodulationMode::Wfm:
        return QStringLiteral("WFM");
    case DemodulationMode::Usb:
        return QStringLiteral("USB");
    case DemodulationMode::Lsb:
        return QStringLiteral("LSB");
    case DemodulationMode::DigitalDecoderOutput:
        return QStringLiteral("DMR/P25");
    }
    return QStringLiteral("Unknown");
}

QString formatFilterWidth(quint64 width)
{
    return QStringLiteral("%1 kHz").arg(
        static_cast<double>(width) / 1'000.0, 0, 'f', width % 1'000 == 0 ? 0 : 2);
}

std::vector<quint64> filterPresets(sdr::radio::DemodulationMode mode)
{
    using sdr::radio::DemodulationMode;
    switch (mode) {
    case DemodulationMode::Am: return {5'000, 6'000, 9'000, 10'000, 12'000, 15'000};
    case DemodulationMode::Nfm: return {8'330, 10'000, 12'500, 15'000, 25'000};
    case DemodulationMode::Wfm: return {100'000, 150'000, 180'000, 200'000, 250'000};
    case DemodulationMode::Usb:
    case DemodulationMode::Lsb: return {1'800, 2'100, 2'400, 2'700, 3'000, 4'000};
    case DemodulationMode::DigitalDecoderOutput:
        return {8'330, 10'000, 12'500, 15'000, 25'000};
    }
    return {};
}

quint64 filterWheelStep(sdr::radio::DemodulationMode mode) noexcept
{
    using sdr::radio::DemodulationMode;
    switch (mode) {
    case DemodulationMode::Am:
    case DemodulationMode::Nfm:
        return 500;
    case DemodulationMode::Wfm:
        return 10'000;
    case DemodulationMode::Usb:
    case DemodulationMode::Lsb:
        return 100;
    case DemodulationMode::DigitalDecoderOutput:
        return 500;
    }
    return 500;
}

sdr::app::FrequencyViewport::PassbandAlignment passbandAlignment(
    sdr::radio::DemodulationMode mode) noexcept
{
    using Alignment = sdr::app::FrequencyViewport::PassbandAlignment;
    if (mode == sdr::radio::DemodulationMode::Usb) {
        return Alignment::Upper;
    }
    if (mode == sdr::radio::DemodulationMode::Lsb) {
        return Alignment::Lower;
    }
    return Alignment::Centered;
}

template <typename Integer>
void accumulateWheelDelta(Integer& pendingDelta, Integer wheelDelta) noexcept
{
    if (wheelDelta > 0 &&
        pendingDelta > std::numeric_limits<Integer>::max() - wheelDelta) {
        pendingDelta = std::numeric_limits<Integer>::max();
    } else if (
        wheelDelta < 0 &&
        pendingDelta < std::numeric_limits<Integer>::min() - wheelDelta) {
        pendingDelta = std::numeric_limits<Integer>::min();
    } else {
        pendingDelta += wheelDelta;
    }
}

quint64 monotonicWheelTimestampNanoseconds() noexcept
{
    return static_cast<quint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

int scaledPixelWheelDelta(int pixelDelta) noexcept
{
    const auto scaled = static_cast<qint64>(pixelDelta) * 8;
    return static_cast<int>(std::clamp(
        scaled,
        static_cast<qint64>(std::numeric_limits<int>::min()),
        static_cast<qint64>(std::numeric_limits<int>::max())));
}

quint64 shiftedFrequency(quint64 frequency, qint64 shift) noexcept
{
    if (shift >= 0) {
        const auto positiveShift = static_cast<quint64>(shift);
        return frequency > std::numeric_limits<quint64>::max() - positiveShift
                   ? std::numeric_limits<quint64>::max()
                   : frequency + positiveShift;
    }
    const quint64 magnitude = shift == std::numeric_limits<qint64>::min()
                                  ? quint64{1} << 63U
                                  : static_cast<quint64>(-shift);
    return frequency < magnitude ? 0 : frequency - magnitude;
}

quint64 filterFrequencyForState(
    const sdr::radio::ReceiverState& state, bool upperEdge) noexcept
{
    const sdr::radio::FrequencyAxisMapper mapper(
        state.centerFrequency, state.sampleRate, {0.0, 1.0});
    const auto passband = mapper.visibleRange();
    std::uint64_t lowerOffset = state.filterWidth / 2;
    std::uint64_t upperOffset = state.filterWidth - lowerOffset;
    if (state.demodulationMode == sdr::radio::DemodulationMode::Usb) {
        lowerOffset = 0;
        upperOffset = state.filterWidth;
    } else if (state.demodulationMode == sdr::radio::DemodulationMode::Lsb) {
        lowerOffset = state.filterWidth;
        upperOffset = 0;
    }
    const std::uint64_t unboundedLower =
        state.listeningFrequency < lowerOffset
            ? 0
            : state.listeningFrequency - lowerOffset;
    const std::uint64_t unboundedUpper =
        state.listeningFrequency >
                std::numeric_limits<std::uint64_t>::max() - upperOffset
            ? std::numeric_limits<std::uint64_t>::max()
            : state.listeningFrequency + upperOffset;
    return upperEdge ? std::min(passband.maximum, unboundedUpper)
                     : std::max(passband.minimum, unboundedLower);
}

}  // namespace

ApplicationModel::ApplicationModel(QObject* parent)
    : ApplicationModel(std::make_unique<sdr::radio::MockReceiverBackend>(), parent)
{
}

ApplicationModel::ApplicationModel(
    std::unique_ptr<sdr::radio::ReceiverBackend> receiver, QObject* parent)
    : QObject(parent)
    , m_receiver(std::move(receiver))
{
    Q_ASSERT(m_receiver);
    m_spectrumFftSize = static_cast<quint64>(
        m_receiver->requestedSpectrumFftSize());
    m_effectiveSpectrumFftSize = static_cast<quint64>(
        m_receiver->spectrumFftSize());
    m_spectrumHertzPerBin = static_cast<double>(effectiveSampleRate()) /
                            static_cast<double>(m_effectiveSpectrumFftSize);
    m_frequencyViewport = sdr::app::FrequencyViewport(
        centerFrequency(),
        effectiveSampleRate(),
        m_effectiveSpectrumFftSize,
        filterWidth());
    (void)m_frequencyViewport.configureDetail(
        m_effectiveSpectrumFftSize,
        filterWidth(),
        listeningFrequency(),
        false,
        passbandAlignment(receiverState().demodulationMode));
    resetScanBoundsToCaptureRange();
    restorePersistedScanSettings();
    restorePersistedScanPresets();
    restorePersistedDisplaySettings();
    m_applicationLog.post(
        sdr::app::ApplicationLogModel::Info,
        QStringLiteral("Application"),
        QStringLiteral("Application model initialized"));
    connect(
        &m_bookmarkModel,
        &sdr::app::BookmarkTreeModel::lastErrorChanged,
        this,
        [this] {
            if (!m_bookmarkModel.lastError().isEmpty()) {
                m_applicationLog.post(
                    sdr::app::ApplicationLogModel::Error,
                    QStringLiteral("Bookmarks"),
                    m_bookmarkModel.lastError());
            }
        });
    initializeBookmarkScannerBindings();
    m_spectrumTimer.setInterval(displayPollIntervalMilliseconds);
    m_spectrumTimer.setTimerType(Qt::CoarseTimer);
    connect(
        &m_spectrumTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::publishLatestSpectrumFrame);
    m_spectrumTimer.start();
    initializeWheelTuningCoalescing();
    m_scanDwellTimer.setSingleShot(true);
    m_scanDwellTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scanDwellTimer, &QTimer::timeout, this, &ApplicationModel::scannerDwellElapsed);
    m_scanResumeTimer.setSingleShot(true);
    m_scanResumeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scanResumeTimer, &QTimer::timeout, this, &ApplicationModel::scannerResumeDelayElapsed);
    m_scanSettlingTimer.setSingleShot(true);
    m_scanSettlingTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scanSettlingTimer, &QTimer::timeout, this, &ApplicationModel::scannerSettlingElapsed);
    m_scanActivityTimer.setInterval(displayPollIntervalMilliseconds);
    connect(&m_scanActivityTimer, &QTimer::timeout, this, &ApplicationModel::updateScannerSquelchActivity);
    m_scanActivityTimer.start();
}

ApplicationModel::ApplicationModel(
    sdr::app::ReceiverRuntime& runtime,
    QObject* parent,
    bool verboseDiagnostics)
    : QObject(parent)
    , m_runtime(&runtime)
    , m_deviceState(QStringLiteral("Receiver runtime is initializing"))
    , m_backendDescription(QStringLiteral("No receiver backend"))
    , m_deviceCapabilitySummary(QStringLiteral("No device capabilities"))
    , m_minimumGainDb(0.0)
    , m_maximumGainDb(0.0)
    , m_gainSupported(false)
    , m_backendReady(false)
    , m_mockMode(runtime.startupMode() == sdr::app::ReceiverRuntime::StartupMode::Mock)
    , m_verboseDiagnostics(verboseDiagnostics)
    , m_statusText(QStringLiteral("Receiver runtime is initializing"))
{
    restorePersistedRuntimeControls();
    m_frequencyViewport = sdr::app::FrequencyViewport(
        centerFrequency(),
        effectiveSampleRate(),
        m_effectiveSpectrumFftSize,
        filterWidth());
    (void)m_frequencyViewport.configureDetail(
        m_effectiveSpectrumFftSize,
        filterWidth(),
        listeningFrequency(),
        false,
        passbandAlignment(receiverState().demodulationMode));
    resetScanBoundsToCaptureRange();
    restorePersistedScanSettings();
    restorePersistedScanPresets();
    restorePersistedDisplaySettings();
    m_applicationLog.post(
        sdr::app::ApplicationLogModel::Info,
        QStringLiteral("Application"),
        QStringLiteral("Application model initialized"));
    connect(
        &m_bookmarkModel,
        &sdr::app::BookmarkTreeModel::lastErrorChanged,
        this,
        [this] {
            if (!m_bookmarkModel.lastError().isEmpty()) {
                m_applicationLog.post(
                    sdr::app::ApplicationLogModel::Error,
                    QStringLiteral("Bookmarks"),
                    m_bookmarkModel.lastError());
            }
        });
    initializeBookmarkScannerBindings();
    runtime.setApplicationLogHandler(
        [log = QPointer<sdr::app::ApplicationLogModel>(&m_applicationLog)](
            int severity,
            const QString& source,
            const QString& message) {
            if (!log) {
                return;
            }
            log->post(
                static_cast<sdr::app::ApplicationLogModel::Severity>(
                    std::clamp(severity, 0, 3)),
                source,
                message);
        });
    runtime.setDsdFmeBinaryPath(m_dsdFmeBinaryPath);
    initializeWheelTuningCoalescing();
    m_scanDwellTimer.setSingleShot(true);
    m_scanDwellTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scanDwellTimer, &QTimer::timeout, this, &ApplicationModel::scannerDwellElapsed);
    m_scanResumeTimer.setSingleShot(true);
    m_scanResumeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scanResumeTimer, &QTimer::timeout, this, &ApplicationModel::scannerResumeDelayElapsed);
    m_scanSettlingTimer.setSingleShot(true);
    m_scanSettlingTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scanSettlingTimer, &QTimer::timeout, this, &ApplicationModel::scannerSettlingElapsed);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::snapshotChanged,
        this,
        &ApplicationModel::applyRuntimeSnapshot);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::scannerListeningFrequencyChanged,
        this,
        &ApplicationModel::applyScannerListeningFrequency);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::scannerCenterFrequencyChanged,
        this,
        &ApplicationModel::applyScannerCenterFrequency);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::spectrumFrameReady,
        this,
        &ApplicationModel::receiveRuntimeSpectrumFrame);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::waterfallFrameReady,
        this,
        &ApplicationModel::receiveRuntimeWaterfallFrame);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::centerFrequencyRequestCompleted,
        this,
        &ApplicationModel::finishCenterFrequencyRequest);
    connect(
        &runtime,
        &sdr::app::ReceiverRuntime::operationPending,
        this,
        [this](const QString& description) {
            if (!m_runtimeBusy) {
                m_runtimeBusy = true;
                emit runtimeBusyChanged();
            }
            setStatusText(description);
        });
}

void ApplicationModel::initializeBookmarkScannerBindings()
{
    const auto changed = [this] { emit bookmarkScannerChanged(); };
    connect(&m_bookmarkModel, &QAbstractItemModel::dataChanged, this, changed);
    connect(&m_bookmarkModel, &QAbstractItemModel::rowsInserted, this, changed);
    connect(&m_bookmarkModel, &QAbstractItemModel::rowsRemoved, this, changed);
    connect(&m_bookmarkModel, &QAbstractItemModel::rowsMoved, this, changed);
    connect(&m_bookmarkModel, &QAbstractItemModel::modelReset, this, changed);
    connect(this, &ApplicationModel::receiverRunningChanged,
            this, &ApplicationModel::bookmarkScannerChanged);
    connect(this, &ApplicationModel::deviceCapabilitiesChanged,
            this, &ApplicationModel::bookmarkScannerChanged);
    connect(this, &ApplicationModel::effectiveSampleRateChanged,
            this, &ApplicationModel::bookmarkScannerChanged);
    connect(this, &ApplicationModel::scannerChanged,
            this, &ApplicationModel::bookmarkScannerChanged);
}

void ApplicationModel::restorePersistedRuntimeControls()
{
    QSettings settings;
    const auto controls = sdr::app::loadReceiverControlSettings(
        settings, m_runtimeState.sampleRate);
    m_runtimeState = controls.receiverState(m_runtimeState.sampleRate);
    m_requestedGainDb = controls.requestedGainDb.value_or(20.0);
}

void ApplicationModel::restorePersistedDisplaySettings()
{
    QSettings settings;
    bool valid = false;
    const double storedRatio = settings.value(
        spectrumWaterfallSplitRatioSetting,
        defaultSpectrumWaterfallSplitRatio).toDouble(&valid);
    m_spectrumWaterfallSplitRatio =
        valid ? normalizedSpectrumWaterfallSplitRatio(storedRatio)
              : defaultSpectrumWaterfallSplitRatio;
    if (!valid || !qFuzzyCompare(
                      storedRatio + 1.0,
                      m_spectrumWaterfallSplitRatio + 1.0)) {
        settings.setValue(
            spectrumWaterfallSplitRatioSetting,
            m_spectrumWaterfallSplitRatio);
    }
    m_splitRatioPersistenceTimer.setInterval(
        splitRatioPersistenceDelayMilliseconds);
    m_splitRatioPersistenceTimer.setSingleShot(true);
    connect(
        &m_splitRatioPersistenceTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::persistSpectrumWaterfallSplitRatio);

    if (settings.contains(sidebarModeSetting)) {
        const QString storedMode = settings.value(sidebarModeSetting).toString();
        m_sidebarMode = normalizedSidebarMode(storedMode);
        if (storedMode.trimmed().toLower() != m_sidebarMode) {
            settings.setValue(sidebarModeSetting, m_sidebarMode);
        }
    } else {
        const auto legacyPanelOpen = sdr::app::strictSettingsBoolean(
            settings.value(legacyBookmarksPanelOpenSetting));
        m_sidebarMode = legacyPanelOpen.value_or(false)
                            ? QStringLiteral("bookmarks")
                            : QStringLiteral("none");
        if (legacyPanelOpen.has_value()) {
            settings.setValue(sidebarModeSetting, m_sidebarMode);
        }
    }
    valid = false;
    const double storedPanelWidth = settings.value(
        bookmarksPanelWidthSetting,
        defaultBookmarksPanelWidth).toDouble(&valid);
    m_bookmarksPanelWidth = valid
                                ? normalizedBookmarksPanelWidth(storedPanelWidth)
                                : defaultBookmarksPanelWidth;
    if (!valid || !qFuzzyCompare(
                      storedPanelWidth + 1.0,
                      m_bookmarksPanelWidth + 1.0)) {
        settings.setValue(bookmarksPanelWidthSetting, m_bookmarksPanelWidth);
    }
    m_bookmarksPanelWidthPersistenceTimer.setInterval(
        bookmarksPanelPersistenceDelayMilliseconds);
    m_bookmarksPanelWidthPersistenceTimer.setSingleShot(true);
    connect(
        &m_bookmarksPanelWidthPersistenceTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::persistBookmarksPanelWidth);

    valid = false;
    const double storedScanPanelWidth = settings.value(
        scanPanelWidthSetting,
        defaultScanPanelWidth).toDouble(&valid);
    m_scanPanelWidth = valid
                           ? normalizedScanPanelWidth(storedScanPanelWidth)
                           : defaultScanPanelWidth;
    if (!valid || !qFuzzyCompare(
                      storedScanPanelWidth + 1.0,
                      m_scanPanelWidth + 1.0)) {
        settings.setValue(scanPanelWidthSetting, m_scanPanelWidth);
    }
    m_scanPanelWidthPersistenceTimer.setInterval(
        bookmarksPanelPersistenceDelayMilliseconds);
    m_scanPanelWidthPersistenceTimer.setSingleShot(true);
    connect(
        &m_scanPanelWidthPersistenceTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::persistScanPanelWidth);

    valid = false;
    const double storedSettingsPanelWidth = settings.value(
        settingsPanelWidthSetting,
        defaultSettingsPanelWidth).toDouble(&valid);
    m_settingsPanelWidth = valid
                               ? normalizedSettingsPanelWidth(storedSettingsPanelWidth)
                               : defaultSettingsPanelWidth;
    if (!valid || !qFuzzyCompare(
                      storedSettingsPanelWidth + 1.0,
                      m_settingsPanelWidth + 1.0)) {
        settings.setValue(settingsPanelWidthSetting, m_settingsPanelWidth);
    }
    m_settingsPanelWidthPersistenceTimer.setInterval(
        bookmarksPanelPersistenceDelayMilliseconds);
    m_settingsPanelWidthPersistenceTimer.setSingleShot(true);
    connect(
        &m_settingsPanelWidthPersistenceTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::persistSettingsPanelWidth);

    valid = false;
    const double storedConsolePanelWidth = settings.value(
        consolePanelWidthSetting,
        defaultConsolePanelWidth).toDouble(&valid);
    m_consolePanelWidth = valid
                              ? normalizedConsolePanelWidth(storedConsolePanelWidth)
                              : defaultConsolePanelWidth;
    if (!valid || !qFuzzyCompare(
                      storedConsolePanelWidth + 1.0,
                      m_consolePanelWidth + 1.0)) {
        settings.setValue(consolePanelWidthSetting, m_consolePanelWidth);
    }
    m_consolePanelWidthPersistenceTimer.setInterval(
        bookmarksPanelPersistenceDelayMilliseconds);
    m_consolePanelWidthPersistenceTimer.setSingleShot(true);
    connect(
        &m_consolePanelWidthPersistenceTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::persistConsolePanelWidth);

    m_dsdFmeBinaryPath = normalizedDsdFmeBinaryPath(
        settings.value(dsdFmeBinaryPathSetting).toString());
    if (settings.value(dsdFmeBinaryPathSetting).toString() !=
        m_dsdFmeBinaryPath) {
        settings.setValue(dsdFmeBinaryPathSetting, m_dsdFmeBinaryPath);
    }
    revalidateDsdFmeBinaryPath();
}

void ApplicationModel::restorePersistedScanSettings()
{
    QSettings settings;
    const QString storedScanType = settings.value(
        scanTypeSetting, QString::fromLatin1(currentPassbandScanType)).toString();
    m_scanTypeIndex = storedScanType == QLatin1String(wideRangeScanType)
                          ? wideRangeScanTypeIndex
                          : currentPassbandScanTypeIndex;
    const auto passband = m_frequencyViewport.captureRange();
    const auto storedLower =
        persistedUnsignedInteger(settings, scanLowerFrequencySetting);
    const auto storedUpper =
        persistedUnsignedInteger(settings, scanUpperFrequencySetting);
    const auto storedStep =
        persistedUnsignedInteger(settings, scanStepSizeSetting);
    const auto storedDwell =
        persistedInteger(settings, scanDwellMillisecondsSetting);
    const auto storedResumeDelay =
        persistedInteger(settings, scanResumeDelayMillisecondsSetting);

    m_scanLowerFrequency = storedLower.value_or(passband.minimum);
    m_scanUpperFrequency = storedUpper.value_or(passband.maximum);
    m_scanStepSize = storedStep.value_or(12'500);
    m_scanDwellMilliseconds = storedDwell.value_or(250);
    m_scanResumeDelayMilliseconds = storedResumeDelay.value_or(1'000);
    const auto storedBookmarkDwell = persistedInteger(
        settings, bookmarkScanDwellMillisecondsSetting);
    const auto storedBookmarkResumeDelay = persistedInteger(
        settings, bookmarkScanResumeDelayMillisecondsSetting);
    m_bookmarkScanDwellMilliseconds =
        storedBookmarkDwell.has_value() && *storedBookmarkDwell > 0
            ? *storedBookmarkDwell : 250;
    m_bookmarkScanResumeDelayMilliseconds =
        storedBookmarkResumeDelay.has_value() && *storedBookmarkResumeDelay >= 0
            ? *storedBookmarkResumeDelay : 1'000;

    bool fallbackToCaptureDefaults = false;
    if (!storedStep.has_value() || m_scanStepSize == 0) {
        m_scanStepSize = 12'500;
    }
    if (!storedDwell.has_value() || m_scanDwellMilliseconds <= 0) {
        m_scanDwellMilliseconds = 250;
    }
    if (!storedResumeDelay.has_value() || m_scanResumeDelayMilliseconds < 0) {
        m_scanResumeDelayMilliseconds = 1'000;
    }
    if (m_scanLowerFrequency > m_scanUpperFrequency) {
        m_scanLowerFrequency = passband.minimum;
        m_scanUpperFrequency = passband.maximum;
        fallbackToCaptureDefaults = true;
    }
    m_scanBoundsFollowCapture =
        fallbackToCaptureDefaults || (!storedLower.has_value() &&
                                      !storedUpper.has_value());
    static_cast<void>(updateScanValidation());
}

void ApplicationModel::restorePersistedScanPresets()
{
    QSettings settings;
    const QVariantList storedPresets = settings.value(scanPresetsSetting).toList();
    QSet<QString> knownIds;
    QSet<QString> knownNames;
    bool ignoredInvalidPreset = false;

    for (const QVariant& storedPreset : storedPresets) {
        const QVariantMap fields = storedPreset.toMap();
        const QUuid uuid(fields.value(QStringLiteral("id")).toString());
        const QString name = fields.value(QStringLiteral("name")).toString().trimmed();
        const QString storedScanType =
            fields.value(QStringLiteral("scanType")).toString();
        const QString scanType = storedScanType.isEmpty()
                                     ? QString::fromLatin1(currentPassbandScanType)
                                     : storedScanType;
        const auto lower = persistedUnsignedInteger(
            fields.value(QStringLiteral("lowerFrequencyHz")));
        const auto upper = persistedUnsignedInteger(
            fields.value(QStringLiteral("upperFrequencyHz")));
        const auto step = persistedUnsignedInteger(
            fields.value(QStringLiteral("stepSizeHz")));
        const auto dwell = persistedInteger(
            fields.value(QStringLiteral("dwellMilliseconds")));
        const auto resumeDelay = persistedInteger(
            fields.value(QStringLiteral("resumeDelayMilliseconds")));
        const QString id = uuid.toString(QUuid::WithoutBraces);
        const QString foldedName = name.toCaseFolded();
        if (uuid.isNull() || name.isEmpty() ||
            (scanType != QLatin1String(currentPassbandScanType) &&
             scanType != QLatin1String(wideRangeScanType)) ||
            !lower.has_value() || !upper.has_value() || !step.has_value() ||
            !dwell.has_value() || !resumeDelay.has_value() ||
            *lower > *upper || *step == 0 || *dwell <= 0 ||
            *resumeDelay < 0 || knownIds.contains(id) ||
            knownNames.contains(foldedName)) {
            ignoredInvalidPreset = true;
            continue;
        }
        knownIds.insert(id);
        knownNames.insert(foldedName);
        m_scanPresets.push_back({
            id,
            name,
            scanType,
            {*lower, *upper, *step, *dwell, *resumeDelay},
        });
    }

    if (ignoredInvalidPreset) {
        setScanPresetStatusMessage(
            QStringLiteral("Ignored invalid saved scanner preset data"));
    }
}

bool ApplicationModel::persistScanPresets()
{
    QVariantList storedPresets;
    storedPresets.reserve(static_cast<qsizetype>(m_scanPresets.size()));
    for (const ScanPreset& preset : m_scanPresets) {
        QVariantMap fields;
        fields.insert(QStringLiteral("id"), preset.id);
        fields.insert(QStringLiteral("name"), preset.name);
        fields.insert(QStringLiteral("scanType"), preset.scanType);
        fields.insert(
            QStringLiteral("lowerFrequencyHz"),
            QVariant::fromValue<qulonglong>(preset.settings.lowerFrequency));
        fields.insert(
            QStringLiteral("upperFrequencyHz"),
            QVariant::fromValue<qulonglong>(preset.settings.upperFrequency));
        fields.insert(
            QStringLiteral("stepSizeHz"),
            QVariant::fromValue<qulonglong>(preset.settings.stepSize));
        fields.insert(
            QStringLiteral("dwellMilliseconds"),
            preset.settings.dwellMilliseconds);
        fields.insert(
            QStringLiteral("resumeDelayMilliseconds"),
            preset.settings.resumeDelayMilliseconds);
        storedPresets.append(fields);
    }
    QSettings settings;
    settings.setValue(scanPresetsSetting, storedPresets);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setScanPresetStatusMessage(
            QStringLiteral("Unable to save scanner presets"));
        return false;
    }
    return true;
}

void ApplicationModel::setScanPresetStatusMessage(QString message)
{
    if (m_scanPresetStatusMessage == message) {
        return;
    }
    m_scanPresetStatusMessage = std::move(message);
    emit scanPresetStatusChanged();
}

int ApplicationModel::scanPresetIndex(const QString& presetId) const noexcept
{
    const auto iterator = std::find_if(
        m_scanPresets.cbegin(),
        m_scanPresets.cend(),
        [&presetId](const ScanPreset& preset) { return preset.id == presetId; });
    return iterator == m_scanPresets.cend()
               ? -1
               : static_cast<int>(std::distance(m_scanPresets.cbegin(), iterator));
}

bool ApplicationModel::scanPresetNameAvailable(
    const QString& name,
    const QString& exceptPresetId) const
{
    const QString foldedName = name.trimmed().toCaseFolded();
    return !std::any_of(
        m_scanPresets.cbegin(),
        m_scanPresets.cend(),
        [&foldedName, &exceptPresetId](const ScanPreset& preset) {
            return preset.id != exceptPresetId &&
                   preset.name.toCaseFolded() == foldedName;
        });
}

void ApplicationModel::applyScanPresetConfiguration(
    const QString& scanType,
    const sdr::app::CurrentPassbandScanSettings& settings)
{
    m_scanTypeIndex = scanType == QLatin1String(wideRangeScanType)
                          ? wideRangeScanTypeIndex
                          : currentPassbandScanTypeIndex;
    m_scanLowerFrequency = settings.lowerFrequency;
    m_scanUpperFrequency = settings.upperFrequency;
    m_scanStepSize = settings.stepSize;
    m_scanDwellMilliseconds = settings.dwellMilliseconds;
    m_scanResumeDelayMilliseconds = settings.resumeDelayMilliseconds;
    m_scanBoundsFollowCapture = false;
    m_wideRangePlan.reset();
    m_wideRangePlanDirty = true;

    QSettings persistedSettings;
    persistedSettings.setValue(scanTypeSetting, scanTypeName(m_scanTypeIndex));
    persistedSettings.setValue(scanLowerFrequencySetting, m_scanLowerFrequency);
    persistedSettings.setValue(scanUpperFrequencySetting, m_scanUpperFrequency);
    persistedSettings.setValue(scanStepSizeSetting, m_scanStepSize);
    persistedSettings.setValue(
        scanDwellMillisecondsSetting, m_scanDwellMilliseconds);
    persistedSettings.setValue(
        scanResumeDelayMillisecondsSetting, m_scanResumeDelayMilliseconds);
    static_cast<void>(updateScanValidation());
    emit scannerChanged();
}

void ApplicationModel::persistSpectrumWaterfallSplitRatio()
{
    QSettings settings;
    settings.setValue(
        spectrumWaterfallSplitRatioSetting,
        m_spectrumWaterfallSplitRatio);
}

void ApplicationModel::persistBookmarksPanelWidth()
{
    QSettings().setValue(bookmarksPanelWidthSetting, m_bookmarksPanelWidth);
}

void ApplicationModel::persistScanPanelWidth()
{
    QSettings().setValue(scanPanelWidthSetting, m_scanPanelWidth);
}

void ApplicationModel::persistSettingsPanelWidth()
{
    QSettings().setValue(settingsPanelWidthSetting, m_settingsPanelWidth);
}

void ApplicationModel::persistConsolePanelWidth()
{
    QSettings().setValue(consolePanelWidthSetting, m_consolePanelWidth);
}

quint64 ApplicationModel::centerFrequency() const noexcept
{
    if (m_runtime && m_pendingCenterWheelFrequency.has_value()) {
        return *m_pendingCenterWheelFrequency;
    }
    return receiverState().centerFrequency;
}

quint64 ApplicationModel::listeningFrequency() const noexcept
{
    if (m_runtime && m_pendingCenterWheelFrequency.has_value()) {
        return *m_pendingCenterWheelFrequency;
    }
    return receiverState().listeningFrequency;
}

QString ApplicationModel::centerFrequencyDigits() const
{
    return QString::number(m_centerFrequencyDigitEditPending.value_or(
                               centerFrequency()))
        .rightJustified(
        sdr::app::FrequencyDigitController::digitCount, QLatin1Char('0'));
}

bool ApplicationModel::centerFrequencyDigitEditActive() const noexcept
{
    return m_centerFrequencyDigitEditPending.has_value();
}

int ApplicationModel::centerFrequencyDigitEditIndex() const noexcept
{
    return m_centerFrequencyDigitEditIndex;
}

int ApplicationModel::centerFrequencyDigitEditStartIndex() const noexcept
{
    return m_centerFrequencyDigitEditStartIndex;
}

double ApplicationModel::listeningPosition() const noexcept
{
    return m_frequencyViewport.normalizedPosition(listeningFrequency());
}

double ApplicationModel::filterLowerPosition() const noexcept
{
    return m_frequencyViewport
        .axis({0.0, 1.0})
        .positionForFrequency(static_cast<double>(filterLowerFrequency()))
        .value_or(0.0);
}

double ApplicationModel::filterUpperPosition() const noexcept
{
    return m_frequencyViewport
        .axis({0.0, 1.0})
        .positionForFrequency(static_cast<double>(filterUpperFrequency()))
        .value_or(1.0);
}

quint64 ApplicationModel::filterLowerFrequency() const noexcept
{
    return filterFrequencyForState(displayState(), false);
}

quint64 ApplicationModel::filterUpperFrequency() const noexcept
{
    return filterFrequencyForState(displayState(), true);
}

quint64 ApplicationModel::visibleLowerFrequency() const noexcept
{
    return m_frequencyViewport.visibleRange().minimum;
}

quint64 ApplicationModel::visibleUpperFrequency() const noexcept
{
    return m_frequencyViewport.visibleRange().maximum;
}

quint64 ApplicationModel::visibleCenterFrequency() const noexcept
{
    return m_frequencyViewport.visibleCenter();
}

quint64 ApplicationModel::visibleSpan() const noexcept
{
    return m_frequencyViewport.visibleSpan();
}

double ApplicationModel::displayZoomFactor() const noexcept
{
    return m_frequencyViewport.zoomFactor();
}

quint64 ApplicationModel::displayZoomPercentage() const noexcept
{
    const quint64 fullSpan = m_frequencyViewport.captureSpan();
    const quint64 currentSpan = m_frequencyViewport.visibleSpan();
    if (fullSpan == 0 || currentSpan == 0) {
        return 100;
    }
    return static_cast<quint64>(std::max(
        100.0,
        std::round(
            static_cast<double>(fullSpan) /
            static_cast<double>(currentSpan) * 100.0)));
}

quint64 ApplicationModel::sampleRate() const noexcept
{
    return receiverState().sampleRate;
}

quint64 ApplicationModel::effectiveSampleRate() const noexcept
{
    return m_runtime ? m_runtimeEffectiveSampleRate : m_receiver->effectiveSampleRate();
}

QStringList ApplicationModel::captureBandwidthOptions() const
{
    return m_captureBandwidthOptions;
}

bool ApplicationModel::customCaptureBandwidthSupported() const noexcept
{
    return m_customCaptureBandwidthSupported;
}

quint64 ApplicationModel::spectrumFftSize() const noexcept
{
    return m_spectrumFftSize;
}

quint64 ApplicationModel::effectiveSpectrumFftSize() const noexcept
{
    return m_effectiveSpectrumFftSize;
}

QStringList ApplicationModel::spectrumFftSizeOptions() const
{
    return {
        QStringLiteral("1024"),
        QStringLiteral("2048"),
        QStringLiteral("4096"),
        QStringLiteral("8192"),
        QStringLiteral("16384"),
        QStringLiteral("32768"),
        QStringLiteral("65536"),
        QStringLiteral("131072"),
        QStringLiteral("262144"),
    };
}

double ApplicationModel::spectrumHertzPerBin() const noexcept
{
    return m_spectrumHertzPerBin;
}

double ApplicationModel::effectiveWaterfallRowsPerSecond() const noexcept
{
    return m_effectiveWaterfallRowsPerSecond;
}

quint32 ApplicationModel::visibleWaterfallHistorySeconds() const noexcept
{
    return m_visibleWaterfallHistorySeconds;
}

QStringList ApplicationModel::visibleWaterfallHistoryOptions() const
{
    return {
        QStringLiteral("5 s"),
        QStringLiteral("10 s"),
        QStringLiteral("15 s"),
        QStringLiteral("30 s"),
        QStringLiteral("60 s"),
        QStringLiteral("Custom…"),
    };
}

quint64 ApplicationModel::waterfallHistoryMemoryBudgetBytes() const noexcept
{
    return waterfallHistoryMemoryBudget;
}

double ApplicationModel::spectrumWaterfallSplitRatio() const noexcept
{
    return m_spectrumWaterfallSplitRatio;
}

QAbstractItemModel* ApplicationModel::bookmarkModel() noexcept
{
    return &m_bookmarkModel;
}

bool ApplicationModel::bookmarkUpdateAvailable() const noexcept
{
    return !m_loadedBookmarkUuid.isEmpty() &&
           m_bookmarkModel.visibleRowForUuid(m_loadedBookmarkUuid) >= 0;
}

sdr::app::ApplicationLogModel* ApplicationModel::applicationLog() noexcept
{
    return &m_applicationLog;
}

QString ApplicationModel::sidebarMode() const
{
    return m_sidebarMode;
}

bool ApplicationModel::bookmarksPanelOpen() const noexcept
{
    return m_sidebarMode == QLatin1String("bookmarks");
}

bool ApplicationModel::scanPanelOpen() const noexcept
{
    return m_sidebarMode == QLatin1String("scan");
}

bool ApplicationModel::settingsPanelOpen() const noexcept
{
    return m_sidebarMode == QLatin1String("settings");
}

bool ApplicationModel::consolePanelOpen() const noexcept
{
    return m_sidebarMode == QLatin1String("console");
}

double ApplicationModel::bookmarksPanelWidth() const noexcept
{
    return m_bookmarksPanelWidth;
}

double ApplicationModel::scanPanelWidth() const noexcept
{
    return m_scanPanelWidth;
}

int ApplicationModel::scanTypeIndex() const noexcept
{
    return m_scanTypeIndex;
}

quint64 ApplicationModel::scanLowerFrequency() const noexcept
{
    return m_scanLowerFrequency;
}

quint64 ApplicationModel::scanUpperFrequency() const noexcept
{
    return m_scanUpperFrequency;
}

quint64 ApplicationModel::scanStepSize() const noexcept
{
    return m_scanStepSize;
}

int ApplicationModel::scanDwellMilliseconds() const noexcept
{
    return m_scanDwellMilliseconds;
}

int ApplicationModel::scanResumeDelayMilliseconds() const noexcept
{
    return m_scanResumeDelayMilliseconds;
}

quint64 ApplicationModel::scanCurrentFrequency() const noexcept
{
    return m_lastNotifiedScanCurrentFrequency.value_or(0);
}

int ApplicationModel::bookmarkScanDwellMilliseconds() const noexcept
{
    return m_bookmarkScanDwellMilliseconds;
}

int ApplicationModel::bookmarkScanResumeDelayMilliseconds() const noexcept
{
    return m_bookmarkScanResumeDelayMilliseconds;
}

QString ApplicationModel::bookmarkScanCurrentName() const
{
    if (const auto bookmark = bookmarkScanEntry()) return bookmark->bookmark.name;
    return {};
}

QString ApplicationModel::bookmarkScanPosition() const
{
    if (!bookmarkScanActive() || m_bookmarkScanBookmarks.empty()) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1 of %2")
        .arg(static_cast<qulonglong>(m_scanner.currentFrequency() + 1))
        .arg(static_cast<qulonglong>(m_bookmarkScanBookmarks.size()));
}

QString ApplicationModel::scanCaptureBlockProgress() const
{
    if (m_scanTypeIndex != wideRangeScanTypeIndex ||
        !m_wideRangePlan.has_value() || m_wideRangePlan->blocks.empty()) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1 of %2")
        .arg(static_cast<qulonglong>(m_wideRangeBlockIndex + 1))
        .arg(static_cast<qulonglong>(m_wideRangePlan->blocks.size()));
}

QString ApplicationModel::scanState() const
{
    if (m_bookmarkScanSession) return QStringLiteral("Stopped");
    return activeScannerState();
}

QString ApplicationModel::activeScannerState() const
{
    if (scannerRetuning()) {
        return QStringLiteral("Retuning");
    }
    switch (m_scanner.state()) {
    case sdr::app::CurrentPassbandScanState::Stopped:
        return QStringLiteral("Stopped");
    case sdr::app::CurrentPassbandScanState::Running:
        return QStringLiteral("Running");
    case sdr::app::CurrentPassbandScanState::Paused:
        return QStringLiteral("Paused");
    case sdr::app::CurrentPassbandScanState::Holding:
        return QStringLiteral("Holding on squelch activity");
    }
    return QStringLiteral("Stopped");
}

QString ApplicationModel::bookmarkScanState() const
{
    return m_bookmarkScanSession ? activeScannerState()
                                 : QStringLiteral("Stopped");
}

QString ApplicationModel::scanStatusMessage() const
{
    return m_bookmarkScanSession ? m_scanStatusBeforeBookmarkScan : m_scanStatus;
}

QString ApplicationModel::bookmarkScanStatusMessage() const
{
    return m_bookmarkScanSession ? m_scanStatus : m_bookmarkScanStatus;
}

QString ApplicationModel::scanValidationError() const
{
    return m_scanValidationError;
}

bool ApplicationModel::scanCanStart() const noexcept
{
    return !m_bookmarkScanSession && !scannerOwnsTuning() && receiverRunning() &&
           m_scanValidationError.isEmpty();
}

bool ApplicationModel::scanCanPauseResume() const noexcept
{
    return !m_bookmarkScanSession && !scannerRetuning() &&
           m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped;
}

bool ApplicationModel::scanPaused() const noexcept
{
    return !m_bookmarkScanSession &&
           m_scanner.state() == sdr::app::CurrentPassbandScanState::Paused;
}

bool ApplicationModel::scanCanSkip() const noexcept
{
    return !m_bookmarkScanSession && !scannerRetuning() &&
           m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped;
}

bool ApplicationModel::scanCanStop() const noexcept
{
    return !m_bookmarkScanSession && scannerOwnsTuning();
}

bool ApplicationModel::bookmarkScanCanStart() const
{
    return !scannerOwnsTuning() && receiverRunning() &&
           bookmarkScanValidationError(m_bookmarkModel.scannerBookmarks()).isEmpty();
}

bool ApplicationModel::bookmarkScanCanPauseResume() const noexcept
{
    return m_bookmarkScanSession && !scannerRetuning() &&
           m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped;
}

bool ApplicationModel::bookmarkScanPaused() const noexcept
{
    return m_bookmarkScanSession &&
           m_scanner.state() == sdr::app::CurrentPassbandScanState::Paused;
}

bool ApplicationModel::bookmarkScanCanSkip() const noexcept
{
    return bookmarkScanCanPauseResume();
}

bool ApplicationModel::bookmarkScanCanStop() const noexcept
{
    return m_bookmarkScanSession && scannerOwnsTuning();
}

bool ApplicationModel::scannerOwnsTuning() const noexcept
{
    return m_pendingScanStart.has_value() ||
           m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped;
}

QVariantList ApplicationModel::scanPresets() const
{
    QVariantList presets;
    presets.reserve(static_cast<qsizetype>(m_scanPresets.size()));
    for (const ScanPreset& preset : m_scanPresets) {
        QVariantMap fields;
        fields.insert(QStringLiteral("presetId"), preset.id);
        fields.insert(QStringLiteral("name"), preset.name);
        fields.insert(QStringLiteral("scanType"), preset.scanType);
        fields.insert(
            QStringLiteral("lowerFrequencyHz"),
            QVariant::fromValue<qulonglong>(preset.settings.lowerFrequency));
        fields.insert(
            QStringLiteral("upperFrequencyHz"),
            QVariant::fromValue<qulonglong>(preset.settings.upperFrequency));
        fields.insert(
            QStringLiteral("stepSizeHz"),
            QVariant::fromValue<qulonglong>(preset.settings.stepSize));
        fields.insert(
            QStringLiteral("dwellMilliseconds"),
            preset.settings.dwellMilliseconds);
        fields.insert(
            QStringLiteral("resumeDelayMilliseconds"),
            preset.settings.resumeDelayMilliseconds);
        presets.append(fields);
    }
    return presets;
}

QString ApplicationModel::selectedScanPresetId() const
{
    return m_selectedScanPresetId;
}

QString ApplicationModel::selectedScanPresetName() const
{
    const int index = scanPresetIndex(m_selectedScanPresetId);
    return index < 0
               ? QString()
               : m_scanPresets.at(static_cast<std::size_t>(index)).name;
}

QString ApplicationModel::scanPresetStatusMessage() const
{
    return m_scanPresetStatusMessage;
}

double ApplicationModel::settingsPanelWidth() const noexcept
{
    return m_settingsPanelWidth;
}

double ApplicationModel::consolePanelWidth() const noexcept
{
    return m_consolePanelWidth;
}

QString ApplicationModel::dsdFmeBinaryPath() const
{
    return m_dsdFmeBinaryPath;
}

QString ApplicationModel::dsdFmeBinaryStatus() const
{
    return m_dsdFmeBinaryStatus;
}

bool ApplicationModel::dsdFmeBinaryValid() const noexcept
{
    return m_dsdFmeBinaryValid;
}

QVariantList ApplicationModel::bookmarkDemodulators() const
{
    QVariantList options;
    for (const auto& descriptor :
         sdr::radio::DemodulatorRegistry::availableDemodulators()) {
        options.push_back(QVariantMap{
            {QStringLiteral("id"), QString::fromLatin1(
                 descriptor.id.data(),
                 static_cast<qsizetype>(descriptor.id.size()))},
            {QStringLiteral("name"), QString::fromLatin1(
                 descriptor.displayName.data(),
                 static_cast<qsizetype>(descriptor.displayName.size()))},
        });
    }
    return options;
}

quint64 ApplicationModel::filterWidth() const noexcept
{
    return receiverState().filterWidth;
}

quint64 ApplicationModel::minimumFilterWidth() const noexcept
{
    return sdr::radio::filterWidthRange(
               receiverState().demodulationMode,
               receiverState().sampleRate)
        .minimum;
}

quint64 ApplicationModel::maximumFilterWidth() const noexcept
{
    return sdr::radio::filterWidthRange(
               receiverState().demodulationMode,
               receiverState().sampleRate)
        .maximum;
}

QStringList ApplicationModel::filterWidthOptions() const
{
    const auto range = sdr::radio::filterWidthRange(
        receiverState().demodulationMode, receiverState().sampleRate);
    QStringList options;
    for (const auto width : filterPresets(receiverState().demodulationMode)) {
        if (range.contains(width)) {
            options.append(formatFilterWidth(width));
        }
    }
    options.append(QStringLiteral("Custom…"));
    return options;
}

double ApplicationModel::gain() const noexcept
{
    return receiverState().gainDb;
}

double ApplicationModel::requestedGain() const noexcept
{
    return m_requestedGainDb;
}

double ApplicationModel::ppmCorrection() const noexcept
{
    return receiverState().ppmCorrection;
}

bool ApplicationModel::ppmCorrectionSupported() const noexcept
{
    return receiverCapabilities().ppmCorrectionSupported;
}

bool ApplicationModel::automaticPpmCalibrationSupported() const noexcept
{
    return m_automaticPpmCalibrationSupported;
}

bool ApplicationModel::ppmCalibrationRunning() const noexcept
{
    return m_ppmCalibrationRunning;
}

QString ApplicationModel::ppmCalibrationStatus() const
{
    return m_ppmCalibrationStatus;
}

int ApplicationModel::ppmCalibrationProgressPercent() const noexcept
{
    return m_ppmCalibrationProgressPercent;
}

int ApplicationModel::demodulationModeIndex() const noexcept
{
    return static_cast<int>(receiverState().demodulationMode);
}

QString ApplicationModel::demodulationModeName() const
{
    return modeName(receiverState().demodulationMode);
}

QStringList ApplicationModel::demodulationModes() const
{
    return {
        QStringLiteral("AM"),
        QStringLiteral("NFM"),
        QStringLiteral("WFM"),
        QStringLiteral("USB"),
        QStringLiteral("LSB"),
        QStringLiteral("DMR/P25"),
    };
}

double ApplicationModel::squelchLevel() const noexcept
{
    return receiverState().squelchLevelDb;
}

bool ApplicationModel::automaticSquelchEnabled() const noexcept
{
    return receiverState().squelchMode == sdr::radio::SquelchMode::Automatic;
}

bool ApplicationModel::squelchDisabled() const noexcept
{
    return receiverState().squelchMode == sdr::radio::SquelchMode::Disabled;
}

QString ApplicationModel::squelchStateText() const
{
    using sdr::radio::SquelchMode;
    switch (receiverState().squelchMode) {
    case SquelchMode::Disabled:
        return QStringLiteral("Disabled (open)");
    case SquelchMode::Manual:
        return QStringLiteral("Manual");
    case SquelchMode::Automatic:
        return QStringLiteral("Automatic · %1 dB")
            .arg(receiverState().squelchLevelDb, 0, 'f', 0);
    }
    return QStringLiteral("Unknown");
}

bool ApplicationModel::receiverRunning() const noexcept
{
    return receiverState().running;
}

QString ApplicationModel::statusText() const
{
    return m_statusText;
}

QString ApplicationModel::deviceState() const
{
    return m_deviceState;
}

QString ApplicationModel::backendDescription() const
{
    return m_backendDescription;
}

QStringList ApplicationModel::deviceDisplayNames() const
{
    return m_deviceDisplayNames;
}

int ApplicationModel::selectedDeviceIndex() const noexcept
{
    return static_cast<int>(
        m_deviceIdentifiers.indexOf(m_selectedDeviceIdentifier));
}

QString ApplicationModel::deviceCapabilitySummary() const
{
    return m_deviceCapabilitySummary;
}

bool ApplicationModel::backendReady() const noexcept
{
    return m_backendReady;
}

bool ApplicationModel::deviceDiscoveryAvailable() const noexcept
{
    return m_deviceDiscoveryAvailable;
}

bool ApplicationModel::runtimeBusy() const noexcept
{
    return m_runtimeBusy;
}

bool ApplicationModel::mockMode() const noexcept
{
    return m_mockMode;
}

bool ApplicationModel::verboseDiagnosticsEnabled() const noexcept
{
    return m_verboseDiagnostics;
}

bool ApplicationModel::gainSupported() const noexcept
{
    return m_gainSupported;
}

double ApplicationModel::minimumGain() const noexcept
{
    return m_minimumGainDb;
}

double ApplicationModel::maximumGain() const noexcept
{
    return m_maximumGainDb;
}

double ApplicationModel::gainStep() const noexcept
{
    return m_gainStepDb;
}

QStringList ApplicationModel::audioDeviceDisplayNames() const
{
    return m_audioDeviceDisplayNames;
}

int ApplicationModel::selectedAudioDeviceIndex() const noexcept
{
    return static_cast<int>(m_audioDeviceIdentifiers.indexOf(
        m_selectedAudioDeviceIdentifier));
}

QString ApplicationModel::audioStatusText() const
{
    return m_audioStatusText;
}

QString ApplicationModel::dsdFmeStatusText() const
{
    return m_dsdFmeStatusText;
}

int ApplicationModel::audioVolume() const noexcept
{
    return m_audioVolumePercent;
}

bool ApplicationModel::audioMuted() const noexcept
{
    return m_audioMuted;
}

bool ApplicationModel::audioReady() const noexcept
{
    return m_audioReady;
}

bool ApplicationModel::audioRunning() const noexcept
{
    return m_audioRunning;
}

quint64 ApplicationModel::audioOverflowEvents() const noexcept
{
    return m_audioOverflowEvents;
}

quint64 ApplicationModel::audioUnderrunEvents() const noexcept
{
    return m_audioUnderrunEvents;
}

const std::vector<sdr::radio::FrequencyRange>&
ApplicationModel::deviceSampleRateRanges() const noexcept
{
    return m_deviceSampleRateRanges;
}

quint64 ApplicationModel::tuningWheelStep() const noexcept
{
    return m_tuningWheelStep;
}

sdr::radio::FrequencyAxisMapper ApplicationModel::frequencyViewportAxis(
    sdr::radio::FrequencyPlot plot) const noexcept
{
    return m_frequencyViewport.axis(plot);
}

void ApplicationModel::setDeviceFrequencyRanges(
    std::vector<sdr::radio::FrequencyRange> frequencyRanges)
{
    m_deviceFrequencyRanges = std::move(frequencyRanges);
    const auto allowedRanges = effectiveCenterFrequencyRanges();
    const auto edit = sdr::app::FrequencyDigitController::constrain(
        centerFrequency(), allowedRanges);
    if (!edit.succeeded()) {
        setStatusText(QString::fromStdString(edit.message));
        return;
    }
    if (edit.adjustedToLimit) {
        if (scannerOwnsTuning()) {
            stopScanner(QStringLiteral(
                "Scanner stopped: device frequency limits changed"));
        }
        applyCenterFrequencyEdit(edit);
        return;
    }
    if (m_frequencyViewport.configureCapture(
            centerFrequency(),
            effectiveSampleRate(),
            listeningFrequency(),
            advertisedRfRangeForCenter(centerFrequency()))) {
        emitFrequencyViewportChanges();
    }
    validateActiveScanRange();
    setStatusText(QStringLiteral("Selected device frequency limits applied"));
}

void ApplicationModel::clearDeviceFrequencyRanges()
{
    if (!m_deviceFrequencyRanges.has_value()) {
        return;
    }
    m_deviceFrequencyRanges.reset();
    if (m_frequencyViewport.configureCapture(
            centerFrequency(),
            effectiveSampleRate(),
            listeningFrequency(),
            advertisedRfRangeForCenter(centerFrequency()))) {
        emitFrequencyViewportChanges();
    }
    validateActiveScanRange();
    setStatusText(QStringLiteral("Device-specific frequency limits cleared"));
}

void ApplicationModel::refreshDevices()
{
    if (!m_runtime) {
        setStatusText(QStringLiteral("Device discovery is unavailable in direct mock mode"));
        return;
    }
    m_runtime->refreshDevices();
}

void ApplicationModel::selectDeviceIndex(int index)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    if (!m_runtime || index < 0 || index >= m_deviceIdentifiers.size()) {
        setStatusText(QStringLiteral("Select an available SDR device"));
        return;
    }
    m_runtime->selectDevice(m_deviceIdentifiers.at(index));
}

void ApplicationModel::clearDeviceSelection()
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    if (!m_runtime) {
        setStatusText(QStringLiteral("No hardware device is selected in mock mode"));
        return;
    }
    m_runtime->clearDeviceSelection();
}

void ApplicationModel::selectAudioDeviceIndex(int index)
{
    if (!m_runtime || index < 0 || index >= m_audioDeviceIdentifiers.size()) {
        return;
    }
    m_runtime->selectAudioDevice(m_audioDeviceIdentifiers.at(index));
}

void ApplicationModel::setAudioVolume(int volumePercent)
{
    if (m_runtime) {
        m_runtime->setAudioVolume(volumePercent);
    }
}

void ApplicationModel::setAudioMuted(bool muted)
{
    if (m_runtime) {
        m_runtime->setAudioMuted(muted);
    }
}

void ApplicationModel::startReception()
{
    if (m_runtime) {
        m_runtime->startReception();
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->startReception());
}

void ApplicationModel::stopReception()
{
    if (m_runtime) {
        m_runtime->stopReception();
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->stopReception());
}

void ApplicationModel::setCenterFrequencyText(const QString& frequencyText)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    bool conversionSucceeded = false;
    const quint64 requestedFrequency =
        frequencyText.trimmed().toULongLong(&conversionSucceeded);

    if (!conversionSucceeded) {
        setStatusText(QStringLiteral("Center frequency must contain decimal digits only"));
        return;
    }

    applyCenterFrequencyEdit(sdr::app::FrequencyDigitController::constrain(
        requestedFrequency, effectiveCenterFrequencyRanges()));
}

void ApplicationModel::setListeningFrequency(quint64 frequency)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    if (m_pendingTuningWheelAction == PendingTuningWheelAction::Listening) {
        m_pendingTuningWheelAction = PendingTuningWheelAction::None;
        m_pendingWheelTuningShift = 0;
        m_wheelTuningTimer.stop();
    }
    m_pendingListeningWheelFrequency.reset();
    if (!m_frequencyViewport.captureRange().contains(frequency)) {
        setStatusText(
            QStringLiteral("Listening frequency is outside the visible RF capture range"));
        return;
    }
    if (m_runtime) {
        m_runtime->setListeningFrequency(frequency);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->setListeningFrequency(frequency));
}

void ApplicationModel::adjustCenterFrequencyDigit(int digitIndex, int direction)
{
    if (rejectManualTuningWhileScanning() || centerFrequencyDigitEditActive()) {
        return;
    }
    applyCenterFrequencyEdit(sdr::app::FrequencyDigitController::adjustDigit(
        centerFrequency(),
        digitIndex,
        direction,
        effectiveCenterFrequencyRanges()));
}

void ApplicationModel::zeroCenterFrequencyFromDigit(int digitIndex)
{
    clearCenterFrequencyDigitEdit();
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    applyCenterFrequencyEdit(sdr::app::FrequencyDigitController::zeroFromDigit(
        centerFrequency(), digitIndex, effectiveCenterFrequencyRanges()));
}

bool ApplicationModel::beginCenterFrequencyDigitEdit(int digitIndex)
{
    if (rejectManualTuningWhileScanning()) {
        return false;
    }
    if (!sdr::app::FrequencyDigitController::placeValue(digitIndex).has_value()) {
        setStatusText(QStringLiteral("Frequency digit index is outside the displayed range"));
        return false;
    }
    if (centerFrequencyDigitEditActive()) {
        if (digitIndex < m_centerFrequencyDigitEditStartIndex) {
            return false;
        }
        m_centerFrequencyDigitEditIndex = digitIndex;
        emit centerFrequencyDigitEditChanged();
        return true;
    }

    cancelPendingManualTuning();
    m_centerFrequencyDigitEditOriginal = centerFrequency();
    m_centerFrequencyDigitEditPending = *m_centerFrequencyDigitEditOriginal;
    m_centerFrequencyDigitEditIndex = digitIndex;
    m_centerFrequencyDigitEditStartIndex = digitIndex;
    emit centerFrequencyDigitsChanged();
    emit centerFrequencyDigitEditChanged();
    return true;
}

void ApplicationModel::replaceCenterFrequencyDigitInEdit(int replacementDigit)
{
    if (rejectManualTuningWhileScanning() ||
        !m_centerFrequencyDigitEditPending.has_value() ||
        m_centerFrequencyDigitEditIndex < 0 ||
        m_centerFrequencyDigitEditIndex >=
            sdr::app::FrequencyDigitController::digitCount) {
        return;
    }

    const auto replacement = sdr::app::FrequencyDigitController::replaceDigit(
        *m_centerFrequencyDigitEditPending,
        m_centerFrequencyDigitEditIndex,
        replacementDigit);
    if (!replacement.succeeded()) {
        setStatusText(QString::fromStdString(replacement.message));
        return;
    }
    m_centerFrequencyDigitEditPending = replacement.frequency;
    ++m_centerFrequencyDigitEditIndex;
    emit centerFrequencyDigitsChanged();
    emit centerFrequencyDigitEditChanged();
}

void ApplicationModel::replaceHoveredCenterFrequencyDigit(
    int digitIndex, int replacementDigit)
{
    if (rejectManualTuningWhileScanning() || centerFrequencyDigitEditActive()) {
        return;
    }
    applyExactCenterFrequencyEdit(
        sdr::app::FrequencyDigitController::replaceDigit(
            centerFrequency(), digitIndex, replacementDigit));
}

bool ApplicationModel::commitCenterFrequencyDigitEdit()
{
    if (rejectManualTuningWhileScanning() ||
        !m_centerFrequencyDigitEditPending.has_value()) {
        return false;
    }
    if (m_centerFrequencyDigitEditIndex <
        sdr::app::FrequencyDigitController::digitCount) {
        setStatusText(QStringLiteral("Replace the remaining center-frequency digits before applying"));
        return false;
    }

    const auto constrained = sdr::app::FrequencyDigitController::constrain(
        *m_centerFrequencyDigitEditPending, effectiveCenterFrequencyRanges());
    if (!constrained.succeeded() || constrained.adjustedToLimit) {
        setStatusText(QStringLiteral(
            "Center frequency is outside the available receiver and device limits"));
        return false;
    }
    applyCenterFrequencyEdit({
        sdr::app::FrequencyEditError::None,
        constrained.frequency,
        false,
        "Center frequency is within the available range",
    });
    return !centerFrequencyDigitEditActive();
}

void ApplicationModel::cancelCenterFrequencyDigitEdit()
{
    if (!m_centerFrequencyDigitEditPending.has_value()) {
        return;
    }
    const quint64 originalFrequency = *m_centerFrequencyDigitEditOriginal;
    clearCenterFrequencyDigitEdit();
    if (!scannerOwnsTuning() && centerFrequency() != originalFrequency) {
        applyExactCenterFrequencyEdit(
            sdr::app::FrequencyDigitController::constrain(
                originalFrequency, effectiveCenterFrequencyRanges()));
    }
}

void ApplicationModel::handleFrequencyWheel(
    bool waterfall, int wheelDelta, int modifierKeys)
{
    handleFrequencyWheelWithDeltas(
        waterfall, wheelDelta, 0, modifierKeys);
}

void ApplicationModel::handleFrequencyWheelWithDeltas(
    bool waterfall, int angleDelta, int pixelDelta, int modifierKeys)
{
    const int actionDelta = angleDelta != 0
                                ? angleDelta
                                : scaledPixelWheelDelta(pixelDelta);
    if (actionDelta == 0) {
        return;
    }
    const auto modifiers = Qt::KeyboardModifiers(modifierKeys);
    if (modifiers.testFlag(Qt::ControlModifier)) {
        resetCenterWheelAcceleration();
        discardPendingViewportWheelAction();
        requestFilterWidthAdjustment(actionDelta);
        return;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        resetCenterWheelAcceleration();
        discardPendingViewportWheelAction();
        shiftListeningFromWheel(actionDelta);
        return;
    }
    if (waterfall) {
        resetCenterWheelAcceleration();
        requestWaterfallZoom(actionDelta);
    } else {
        discardPendingViewportWheelAction();
        handleSpectrumWheelDeltas(
            angleDelta,
            pixelDelta,
            wheelTimestampNanoseconds());
    }
}

void ApplicationModel::shiftCenterFromSpectrum(int wheelDelta)
{
    shiftCenterFromSpectrumWithMultiplier(wheelDelta, 1);
}

void ApplicationModel::handleSpectrumWheelDeltas(
    int angleDelta, int pixelDelta, quint64 timestampNanoseconds)
{
    const int rawDelta = angleDelta != 0 ? angleDelta : pixelDelta;
    if (rawDelta == 0) {
        return;
    }
    const int direction = rawDelta > 0 ? 1 : -1;
    if (m_centerWheelDirection != 0 &&
        m_centerWheelDirection != direction) {
        m_centerWheelDeltaRemainder = 0;
        m_centerPixelDeltaRemainder = 0;
    }
    updateCenterWheelAcceleration(direction, timestampNanoseconds);

    if (angleDelta != 0) {
        m_centerPixelDeltaRemainder = 0;
        shiftCenterFromSpectrumWithMultiplier(
            static_cast<qint64>(angleDelta),
            m_centerWheelAccelerationMultiplier);
        return;
    }

    const qint64 boundedPixelDelta = std::clamp(
        static_cast<qint64>(pixelDelta),
        -maximumTouchpadPixelsPerEvent,
        maximumTouchpadPixelsPerEvent);
    accumulateWheelDelta(m_centerPixelDeltaRemainder, boundedPixelDelta);
    const qint64 wheelSteps = m_centerPixelDeltaRemainder /
                              touchpadPixelsPerWheelStep;
    if (wheelSteps == 0) {
        return;
    }
    m_centerPixelDeltaRemainder -=
        wheelSteps * touchpadPixelsPerWheelStep;
    shiftCenterFromSpectrumWithMultiplier(
        wheelSteps * static_cast<qint64>(wheelDeltaPerStep),
        m_centerWheelAccelerationMultiplier);
}

void ApplicationModel::shiftCenterFromSpectrumWithMultiplier(
    qint64 wheelDelta, int accelerationMultiplier)
{
    if (wheelDelta == 0 || rejectManualTuningWhileScanning()) {
        return;
    }

    accumulateWheelDelta(m_centerWheelDeltaRemainder, wheelDelta);
    const qint64 wheelSteps = m_centerWheelDeltaRemainder /
                              static_cast<qint64>(wheelDeltaPerStep);
    if (wheelSteps == 0) {
        return;
    }
    m_centerWheelDeltaRemainder -=
        wheelSteps * static_cast<qint64>(wheelDeltaPerStep);
    clearCenterFrequencyDigitEdit();
    const int boundedMultiplier = std::clamp(accelerationMultiplier, 1, 10);
    const long double requestedShift =
        static_cast<long double>(wheelSteps) *
        static_cast<long double>(boundedMultiplier) *
        static_cast<long double>(m_tuningWheelStep);
    const qint64 boundedStep = static_cast<qint64>(std::clamp(
        requestedShift,
        static_cast<long double>(std::numeric_limits<qint64>::min()),
        static_cast<long double>(std::numeric_limits<qint64>::max())));
    if (m_runtime) {
        queueTuningWheelAction(
            PendingTuningWheelAction::Center, boundedStep);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(
        previousState, m_receiver->shiftCenterFrequency(boundedStep));
}

void ApplicationModel::resetCenterWheelAcceleration()
{
    m_centerWheelDeltaRemainder = 0;
    m_centerPixelDeltaRemainder = 0;
    m_centerWheelDirection = 0;
    m_centerWheelAccelerationMultiplier = 1;
    m_centerWheelLastEventNanoseconds.reset();
}

void ApplicationModel::updateCenterWheelAcceleration(
    int direction, quint64 timestampNanoseconds)
{
    if (m_centerWheelDirection == 0 ||
        m_centerWheelDirection != direction ||
        !m_centerWheelLastEventNanoseconds.has_value()) {
        m_centerWheelDirection = direction;
        m_centerWheelAccelerationMultiplier = 1;
        m_centerWheelLastEventNanoseconds = timestampNanoseconds;
        return;
    }

    const quint64 previousTimestamp =
        *m_centerWheelLastEventNanoseconds;
    const quint64 elapsed = timestampNanoseconds >= previousTimestamp
                                ? timestampNanoseconds - previousTimestamp
                                : wheelAccelerationResetNanoseconds;
    if (elapsed > wheelAccelerationResetNanoseconds) {
        m_centerWheelAccelerationMultiplier = 1;
    } else if (m_centerWheelAccelerationMultiplier == 1) {
        m_centerWheelAccelerationMultiplier = 2;
    } else if (
        m_centerWheelAccelerationMultiplier == 2 &&
        elapsed <= wheelAccelerationFastNanoseconds) {
        m_centerWheelAccelerationMultiplier = 5;
    } else if (
        m_centerWheelAccelerationMultiplier == 5 &&
        elapsed <= wheelAccelerationVeryFastNanoseconds) {
        m_centerWheelAccelerationMultiplier = 10;
    }
    m_centerWheelLastEventNanoseconds = timestampNanoseconds;
}

quint64 ApplicationModel::wheelTimestampNanoseconds() const noexcept
{
    return m_wheelClockForTests ? m_wheelClockForTests() :
                                  monotonicWheelTimestampNanoseconds();
}

void ApplicationModel::setWheelClockForTests(
    std::function<quint64()> clock)
{
    m_wheelClockForTests = std::move(clock);
    resetCenterWheelAcceleration();
}

void ApplicationModel::shiftListeningFromWheel(int wheelDelta)
{
    if (wheelDelta == 0 || rejectManualTuningWhileScanning()) {
        return;
    }
    const qint64 requestedStep = wheelDelta > 0
                                     ? static_cast<qint64>(m_tuningWheelStep)
                                     : -static_cast<qint64>(m_tuningWheelStep);
    if (m_runtime) {
        queueTuningWheelAction(
            PendingTuningWheelAction::Listening, requestedStep);
        return;
    }

    const auto captureRange = m_frequencyViewport.captureRange();
    const auto requested = static_cast<qint64>(listeningFrequency()) +
                           requestedStep;
    const quint64 adjusted = static_cast<quint64>(std::clamp(
        requested,
        static_cast<qint64>(captureRange.minimum),
        static_cast<qint64>(captureRange.maximum)));
    const auto previousState = m_receiver->state();
    const auto operation = m_receiver->setListeningFrequency(adjusted);
    const bool recentered =
        operation.succeeded() && operation.stateChanged &&
        m_frequencyViewport.centerOn(adjusted);
    applyOperation(previousState, operation);
    if (recentered) {
        emitFrequencyViewportChanges();
    }
}

void ApplicationModel::requestWaterfallZoom(int wheelDelta)
{
    if (wheelDelta == 0) {
        return;
    }
    ++m_waterfallZoomEvents;
    if (m_viewportWheelTimer.isActive() &&
        m_pendingViewportWheelAction == PendingViewportWheelAction::Zoom) {
        ++m_coalescedWaterfallZoomEvents;
    }
    queueViewportWheelAction(PendingViewportWheelAction::Zoom, wheelDelta);
}

void ApplicationModel::selectListeningFrequencyAt(
    double horizontalPosition, double displayWidth)
{
    const auto frequency =
        frequencyViewportAxis({0.0, displayWidth})
            .roundedFrequencyForPosition(horizontalPosition);
    if (!frequency.has_value()) {
        setStatusText(QStringLiteral("Cannot tune against an empty spectrum display"));
        return;
    }

    setListeningFrequency(*frequency);
}

void ApplicationModel::setTuningWheelStep(quint64 step)
{
    if (step == 0 || step > maximumTuningWheelStep) {
        setStatusText(QStringLiteral("Spectrum tuning step is outside the supported range"));
        return;
    }
    if (m_tuningWheelStep == step) {
        return;
    }

    m_centerWheelDeltaRemainder = 0;
    m_tuningWheelStep = step;
    emit tuningWheelStepChanged();
}

void ApplicationModel::setSampleRate(quint64 newSampleRate)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    if (m_runtime) {
        m_runtime->setSampleRate(newSampleRate);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->setSampleRate(newSampleRate));
}

void ApplicationModel::setCaptureBandwidthText(const QString& bandwidthText)
{
    const QString text = bandwidthText.trimmed();
    if (text.isEmpty()) {
        setStatusText(QStringLiteral("Enter a capture bandwidth in MS/s or samples per second"));
        return;
    }
    QString numericText = text;
    const bool megasamples = numericText.contains(
        QStringLiteral("m"), Qt::CaseInsensitive);
    numericText.remove(QStringLiteral("MS/s"), Qt::CaseInsensitive);
    numericText.remove(QStringLiteral("MHz"), Qt::CaseInsensitive);
    numericText.remove(QStringLiteral("M"), Qt::CaseInsensitive);
    numericText.remove(QStringLiteral("sps"), Qt::CaseInsensitive);
    numericText.remove(QStringLiteral("Hz"), Qt::CaseInsensitive);
    numericText = numericText.trimmed();
    bool valid = false;
    const double value = numericText.toDouble(&valid);
    if (!valid || !std::isfinite(value) || value <= 0.0) {
        setStatusText(QStringLiteral("Capture bandwidth must be a positive numeric value"));
        return;
    }
    const double samplesPerSecond = megasamples ? value * 1'000'000.0 : value;
    if (samplesPerSecond > static_cast<double>(std::numeric_limits<quint64>::max())) {
        setStatusText(QStringLiteral("Capture bandwidth is too large"));
        return;
    }
    setSampleRate(static_cast<quint64>(std::llround(samplesPerSecond)));
}

void ApplicationModel::setSpectrumFftSize(quint64 fftSize)
{
    if (!spectrumFftSizeOptions().contains(QString::number(fftSize))) {
        setStatusText(QStringLiteral("Unsupported spectrum FFT size"));
        return;
    }
    if (m_runtime) {
        m_runtime->setSpectrumFftSize(fftSize);
        return;
    }
    const auto result = m_receiver->setSpectrumFftSize(
        static_cast<std::size_t>(fftSize));
    if (!result.succeeded()) {
        setStatusText(QString::fromStdString(result.message));
        return;
    }
    m_spectrumFftSize = static_cast<quint64>(
        m_receiver->requestedSpectrumFftSize());
    m_effectiveSpectrumFftSize = static_cast<quint64>(
        m_receiver->spectrumFftSize());
    m_spectrumHertzPerBin = static_cast<double>(effectiveSampleRate()) /
                            static_cast<double>(m_effectiveSpectrumFftSize);
    if (m_frequencyViewport.configureDetail(
            m_effectiveSpectrumFftSize,
            filterWidth(),
            listeningFrequency(),
            true,
            passbandAlignment(receiverState().demodulationMode))) {
        emitFrequencyViewportChanges();
    }
    resetSpectrumFrame();
    emit spectrumFftSizeChanged();
    setStatusText(QString::fromStdString(result.message));
}

void ApplicationModel::setVisibleWaterfallHistorySeconds(quint32 seconds)
{
    if (seconds == 0 || seconds > 300) {
        setStatusText(QStringLiteral("Visible waterfall history must be between 1 and 300 seconds"));
        return;
    }
    if (m_runtime) {
        m_runtime->setVisibleWaterfallHistorySeconds(seconds);
        return;
    }
    if (m_visibleWaterfallHistorySeconds != seconds) {
        m_visibleWaterfallHistorySeconds = seconds;
        emit waterfallSettingsChanged();
    }
}

void ApplicationModel::setSpectrumWaterfallSplitRatio(double ratio)
{
    const double boundedRatio = normalizedSpectrumWaterfallSplitRatio(ratio);
    if (qFuzzyCompare(
            m_spectrumWaterfallSplitRatio + 1.0,
            boundedRatio + 1.0)) {
        return;
    }
    m_spectrumWaterfallSplitRatio = boundedRatio;
    m_splitRatioPersistenceTimer.start();
    emit spectrumWaterfallSplitRatioChanged();
}

void ApplicationModel::commitSpectrumWaterfallSplitRatio()
{
    m_splitRatioPersistenceTimer.stop();
    persistSpectrumWaterfallSplitRatio();
}

void ApplicationModel::setSidebarMode(const QString& mode)
{
    const QString normalized = normalizedSidebarMode(mode);
    if (m_sidebarMode == normalized) {
        return;
    }
    m_sidebarMode = normalized;
    QSettings().setValue(sidebarModeSetting, m_sidebarMode);
    emit sidebarModeChanged();
}

void ApplicationModel::setBookmarksPanelOpen(bool open)
{
    setSidebarMode(open ? QStringLiteral("bookmarks") : QStringLiteral("none"));
}

void ApplicationModel::setBookmarksPanelWidth(double width)
{
    const double boundedWidth = normalizedBookmarksPanelWidth(width);
    if (qFuzzyCompare(m_bookmarksPanelWidth + 1.0, boundedWidth + 1.0)) {
        return;
    }
    m_bookmarksPanelWidth = boundedWidth;
    m_bookmarksPanelWidthPersistenceTimer.start();
    emit bookmarksPanelWidthChanged();
}

void ApplicationModel::commitBookmarksPanelWidth()
{
    m_bookmarksPanelWidthPersistenceTimer.stop();
    persistBookmarksPanelWidth();
}

void ApplicationModel::setScanPanelWidth(double width)
{
    const double boundedWidth = normalizedScanPanelWidth(width);
    if (qFuzzyCompare(m_scanPanelWidth + 1.0, boundedWidth + 1.0)) {
        return;
    }
    m_scanPanelWidth = boundedWidth;
    m_scanPanelWidthPersistenceTimer.start();
    emit scanPanelWidthChanged();
}

void ApplicationModel::commitScanPanelWidth()
{
    m_scanPanelWidthPersistenceTimer.stop();
    persistScanPanelWidth();
}

void ApplicationModel::setScanTypeIndex(int scanTypeIndex)
{
    if (scanTypeIndex != currentPassbandScanTypeIndex &&
        scanTypeIndex != wideRangeScanTypeIndex) {
        return;
    }
    if (m_scanTypeIndex == scanTypeIndex) {
        return;
    }
    if (scannerOwnsTuning()) {
        setStatusText(QStringLiteral(
            "Stop scanning before changing the scan type"));
        return;
    }
    m_scanTypeIndex = scanTypeIndex;
    m_wideRangePlan.reset();
    m_wideRangePlanDirty = true;
    QSettings().setValue(scanTypeSetting, scanTypeName(scanTypeIndex));
    static_cast<void>(updateScanValidation());
    emit scannerChanged();
}

void ApplicationModel::setScanLowerFrequency(quint64 frequency)
{
    if (m_scanLowerFrequency == frequency) {
        return;
    }
    m_scanLowerFrequency = frequency;
    m_scanBoundsFollowCapture = false;
    m_wideRangePlanDirty = true;
    QSettings().setValue(scanLowerFrequencySetting, frequency);
    static_cast<void>(updateScanValidation());
    if (scanCanStop()) {
        stopScanner(QStringLiteral("Scanner stopped: scan settings changed"));
    } else {
        emit scannerChanged();
    }
}

void ApplicationModel::setScanUpperFrequency(quint64 frequency)
{
    if (m_scanUpperFrequency == frequency) {
        return;
    }
    m_scanUpperFrequency = frequency;
    m_scanBoundsFollowCapture = false;
    m_wideRangePlanDirty = true;
    QSettings().setValue(scanUpperFrequencySetting, frequency);
    static_cast<void>(updateScanValidation());
    if (scanCanStop()) {
        stopScanner(QStringLiteral("Scanner stopped: scan settings changed"));
    } else {
        emit scannerChanged();
    }
}

void ApplicationModel::setScanStepSize(quint64 stepSize)
{
    if (m_scanStepSize == stepSize) {
        return;
    }
    m_scanStepSize = stepSize;
    m_wideRangePlanDirty = true;
    QSettings().setValue(scanStepSizeSetting, stepSize);
    static_cast<void>(updateScanValidation());
    if (scanCanStop()) {
        stopScanner(QStringLiteral("Scanner stopped: scan settings changed"));
    } else {
        emit scannerChanged();
    }
}

void ApplicationModel::setScanDwellMilliseconds(int milliseconds)
{
    if (m_scanDwellMilliseconds == milliseconds) {
        return;
    }
    m_scanDwellMilliseconds = milliseconds;
    QSettings().setValue(scanDwellMillisecondsSetting, milliseconds);
    static_cast<void>(updateScanValidation());
    if (scanCanStop()) {
        stopScanner(QStringLiteral("Scanner stopped: scan settings changed"));
    } else {
        emit scannerChanged();
    }
}

void ApplicationModel::setScanResumeDelayMilliseconds(int milliseconds)
{
    if (m_scanResumeDelayMilliseconds == milliseconds) {
        return;
    }
    m_scanResumeDelayMilliseconds = milliseconds;
    QSettings().setValue(scanResumeDelayMillisecondsSetting, milliseconds);
    static_cast<void>(updateScanValidation());
    if (scanCanStop()) {
        stopScanner(QStringLiteral("Scanner stopped: scan settings changed"));
    } else {
        emit scannerChanged();
    }
}

void ApplicationModel::selectScanPreset(const QString& presetId)
{
    const int index = scanPresetIndex(presetId);
    const QString selectedId = index >= 0
                                   ? m_scanPresets.at(static_cast<std::size_t>(index)).id
                                   : QString();
    if (m_selectedScanPresetId == selectedId) {
        return;
    }
    m_selectedScanPresetId = selectedId;
    emit scanPresetSelectionChanged();
    if (index < 0 && !presetId.isEmpty()) {
        setScanPresetStatusMessage(
            QStringLiteral("Selected scanner preset is no longer available"));
    }
}

bool ApplicationModel::saveNewScanPreset(const QString& name)
{
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        setScanPresetStatusMessage(
            QStringLiteral("Enter a non-empty scanner preset name"));
        return false;
    }
    if (!scanPresetNameAvailable(trimmedName)) {
        setScanPresetStatusMessage(
            QStringLiteral("A scanner preset with that name already exists"));
        return false;
    }

    const auto previousPresets = m_scanPresets;
    const QString previousSelection = m_selectedScanPresetId;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_scanPresets.push_back({
        id,
        trimmedName,
        scanTypeName(m_scanTypeIndex),
        scanSettings(),
    });
    m_selectedScanPresetId = id;
    if (!persistScanPresets()) {
        m_scanPresets = previousPresets;
        m_selectedScanPresetId = previousSelection;
        return false;
    }
    emit scanPresetsChanged();
    emit scanPresetSelectionChanged();
    setScanPresetStatusMessage(
        QStringLiteral("Saved scanner preset “%1”").arg(trimmedName));
    return true;
}

bool ApplicationModel::loadSelectedScanPreset()
{
    if (scannerOwnsTuning()) {
        setScanPresetStatusMessage(
            QStringLiteral("Stop scanning before loading a preset"));
        return false;
    }
    const int index = scanPresetIndex(m_selectedScanPresetId);
    if (index < 0) {
        setScanPresetStatusMessage(QStringLiteral("Select a scanner preset to load"));
        return false;
    }
    const ScanPreset& preset = m_scanPresets.at(static_cast<std::size_t>(index));
    applyScanPresetConfiguration(preset.scanType, preset.settings);
    setScanPresetStatusMessage(
        QStringLiteral("Loaded scanner preset “%1”").arg(preset.name));
    return true;
}

bool ApplicationModel::updateSelectedScanPreset(const QString& name)
{
    const int index = scanPresetIndex(m_selectedScanPresetId);
    if (index < 0) {
        setScanPresetStatusMessage(
            QStringLiteral("Select a scanner preset to update"));
        return false;
    }
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        setScanPresetStatusMessage(
            QStringLiteral("Enter a non-empty scanner preset name"));
        return false;
    }
    const ScanPreset& selectedPreset =
        m_scanPresets.at(static_cast<std::size_t>(index));
    if (!scanPresetNameAvailable(trimmedName, selectedPreset.id)) {
        setScanPresetStatusMessage(
            QStringLiteral("A scanner preset with that name already exists"));
        return false;
    }

    const auto previousPresets = m_scanPresets;
    ScanPreset& preset = m_scanPresets.at(static_cast<std::size_t>(index));
    preset.name = trimmedName;
    preset.scanType = scanTypeName(m_scanTypeIndex);
    preset.settings = scanSettings();
    if (!persistScanPresets()) {
        m_scanPresets = previousPresets;
        return false;
    }
    emit scanPresetsChanged();
    emit scanPresetSelectionChanged();
    setScanPresetStatusMessage(
        QStringLiteral("Updated scanner preset “%1”").arg(trimmedName));
    return true;
}

bool ApplicationModel::deleteSelectedScanPreset()
{
    const int index = scanPresetIndex(m_selectedScanPresetId);
    if (index < 0) {
        setScanPresetStatusMessage(
            QStringLiteral("Select a scanner preset to delete"));
        return false;
    }

    const auto previousPresets = m_scanPresets;
    const QString previousSelection = m_selectedScanPresetId;
    const QString name = m_scanPresets.at(static_cast<std::size_t>(index)).name;
    m_scanPresets.erase(m_scanPresets.begin() + index);
    m_selectedScanPresetId.clear();
    if (!persistScanPresets()) {
        m_scanPresets = previousPresets;
        m_selectedScanPresetId = previousSelection;
        return false;
    }
    emit scanPresetsChanged();
    emit scanPresetSelectionChanged();
    setScanPresetStatusMessage(
        QStringLiteral("Deleted scanner preset “%1”").arg(name));
    return true;
}

bool ApplicationModel::moveScanPreset(
    const QString& presetId,
    const QString& targetPresetId,
    const QString& placement)
{
    const int sourceIndex = scanPresetIndex(presetId);
    const int targetIndex = scanPresetIndex(targetPresetId);
    if (sourceIndex < 0 || targetIndex < 0 || sourceIndex == targetIndex ||
        (placement != QLatin1String("before") &&
         placement != QLatin1String("after"))) {
        return false;
    }

    const auto previousPresets = m_scanPresets;
    ScanPreset movedPreset = m_scanPresets.at(static_cast<std::size_t>(sourceIndex));
    m_scanPresets.erase(m_scanPresets.begin() + sourceIndex);
    int adjustedTargetIndex = scanPresetIndex(targetPresetId);
    if (placement == QLatin1String("after")) {
        ++adjustedTargetIndex;
    }
    m_scanPresets.insert(
        m_scanPresets.begin() + adjustedTargetIndex,
        std::move(movedPreset));
    if (!persistScanPresets()) {
        m_scanPresets = previousPresets;
        return false;
    }
    emit scanPresetsChanged();
    setScanPresetStatusMessage(QStringLiteral("Reordered scanner presets"));
    return true;
}

void ApplicationModel::startScan()
{
    if (scannerOwnsTuning()) {
        return;
    }
    clearCenterFrequencyDigitEdit();
    static_cast<void>(updateScanValidation());
    if (!receiverRunning()) {
        m_scanStatus = QStringLiteral(
            "Scanner not started: start reception before scanning");
        emit scannerChanged();
        return;
    }
    if (!m_scanValidationError.isEmpty()) {
        m_scanStatus = QStringLiteral("Scanner not started: %1").arg(m_scanValidationError);
        emit scannerChanged();
        return;
    }

    cancelPendingManualTuning();
    if (m_scanTypeIndex == wideRangeScanTypeIndex) {
        if (!refreshWideRangePlan() || !m_wideRangePlan.has_value() ||
            m_wideRangePlan->blocks.empty() ||
            !m_scanner.start(
                scanSettings(),
                {m_scanLowerFrequency, m_scanUpperFrequency},
                m_scanLowerFrequency,
                false)) {
            m_scanStatus = QStringLiteral("Scanner not started: %1").arg(
                m_scanValidationError.isEmpty()
                    ? QStringLiteral("invalid wide-range plan")
                    : m_scanValidationError);
            m_scanner.stop();
            emit scannerChanged();
            return;
        }
        m_wideRangeBlockIndex = 0;
        notifyScanCurrentFrequencyChanged();
        requestScannerCenter(
            m_wideRangePlan->blocks.front().centerFrequency,
            m_scanLowerFrequency,
            0,
            true);
        return;
    }
    const quint64 midpoint = scanMidpoint();
    m_pendingScanStart = PendingScanStart{
        midpoint,
        listeningFrequency(),
        0,
        0,
        false,
        false,
        true,
    };
    m_scanStatus = QStringLiteral("Centering receiver for scanner range");
    emit scannerChanged();

    if (m_runtime) {
        m_runtime->setCenterFrequency(midpoint);
        return;
    }

    const auto previousState = m_receiver->state();
    const auto operation = m_receiver->setCenterFrequency(midpoint);
    applyOperation(previousState, operation);
    finishScannerCentering(midpoint, operation.succeeded());
}

void ApplicationModel::pauseOrResumeScan()
{
    if (!scanCanPauseResume()) {
        return;
    }
    pauseOrResumeActiveScan();
}

void ApplicationModel::pauseOrResumeActiveScan()
{
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Paused) {
        m_scanner.resume(scannerSquelchOpen());
        if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Holding) {
            m_scanStatus = QStringLiteral("Holding on squelch activity");
        } else {
            m_scanStatus = m_bookmarkScanSession
                               ? QStringLiteral("Scanning bookmarks")
                               : (m_scanTypeIndex == wideRangeScanTypeIndex
                                      ? QStringLiteral("Scanning wide range")
                                      : QStringLiteral("Scanning current capture passband"));
            scheduleScanDwell();
        }
    } else {
        m_scanDwellTimer.stop();
        m_scanResumeTimer.stop();
        if (m_runtime) {
            m_runtime->cancelScannerListeningFrequencyRequests();
        }
        m_scanner.pause();
        m_scanStatus = QStringLiteral("Scanner paused");
    }
    emit scannerChanged();
}

void ApplicationModel::skipScanFrequency()
{
    if (!scanCanSkip()) return;
    skipActiveScan();
}

void ApplicationModel::skipActiveScan()
{
    const auto frequency = m_scanner.skip();
    if (!frequency.has_value()) {
        return;
    }
    notifyScanCurrentFrequencyChanged();
    m_scanDwellTimer.stop();
    m_scanResumeTimer.stop();
    const WideTuneResult tuneResult =
        m_bookmarkScanSession
            ? tuneBookmarkScannerTo(static_cast<std::size_t>(*frequency))
            : (m_scanTypeIndex == wideRangeScanTypeIndex
            ? tuneWideScannerTo(*frequency)
            : (tuneScannerTo(*frequency), WideTuneResult::Tuned));
    if (tuneResult != WideTuneResult::Tuned) {
        return;
    }
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Paused) {
        m_scanStatus = QStringLiteral("Scanner paused after skipping frequency");
    } else {
        m_scanStatus = m_bookmarkScanSession
                           ? QStringLiteral("Scanning bookmarks")
                           : (m_scanTypeIndex == wideRangeScanTypeIndex
                                  ? QStringLiteral("Scanning wide range")
                                  : QStringLiteral("Scanning current capture passband"));
        scheduleScanDwell();
    }
    emit scannerChanged();
}

void ApplicationModel::stopScan()
{
    if (!scanCanStop()) return;
    stopActiveScan();
}

void ApplicationModel::stopActiveScan()
{
    if (m_pendingScanStart.has_value() &&
        (m_pendingScanStart->wideRange || m_pendingScanStart->bookmark) &&
        !m_scanSettlingTimer.isActive()) {
        m_stopScanAfterRetune = true;
        m_scanStatus = QStringLiteral(
            "Stopping scanner after the pending hardware retune");
        emit scannerChanged();
        return;
    }
    if (m_pendingWideListeningFrequency.has_value() ||
        m_pendingBookmarkListeningFrequency.has_value()) {
        m_stopScanAfterRetune = true;
        m_scanStatus = QStringLiteral(
            "Stopping scanner after the pending listening tune");
        emit scannerChanged();
        return;
    }
    if (scannerOwnsTuning()) {
        stopScanner(QStringLiteral("Scanner stopped"));
    }
}

void ApplicationModel::setBookmarkScanDwellMilliseconds(int milliseconds)
{
    if (milliseconds <= 0 || m_bookmarkScanDwellMilliseconds == milliseconds) {
        return;
    }
    m_bookmarkScanDwellMilliseconds = milliseconds;
    QSettings().setValue(bookmarkScanDwellMillisecondsSetting, milliseconds);
    if (m_bookmarkScanSession) {
        stopScanner(QStringLiteral("Scanner stopped: bookmark scan settings changed"));
    }
    emit bookmarkScannerChanged();
}

void ApplicationModel::setBookmarkScanResumeDelayMilliseconds(int milliseconds)
{
    if (milliseconds < 0 ||
        m_bookmarkScanResumeDelayMilliseconds == milliseconds) {
        return;
    }
    m_bookmarkScanResumeDelayMilliseconds = milliseconds;
    QSettings().setValue(
        bookmarkScanResumeDelayMillisecondsSetting, milliseconds);
    if (m_bookmarkScanSession) {
        stopScanner(QStringLiteral("Scanner stopped: bookmark scan settings changed"));
    }
    emit bookmarkScannerChanged();
}

void ApplicationModel::startBookmarkScan()
{
    if (scannerOwnsTuning()) return;
    auto bookmarks = m_bookmarkModel.scannerBookmarks();
    const QString error = bookmarkScanValidationError(bookmarks);
    if (!receiverRunning() || !error.isEmpty()) {
        m_bookmarkScanStatus = !receiverRunning()
                                   ? QStringLiteral("Scanner not started: start reception before scanning")
                                   : QStringLiteral("Scanner not started: %1").arg(error);
        emit bookmarkScannerChanged();
        return;
    }
    const bool legacySquelchEnabled = !squelchDisabled();
    const double legacySquelchLevel = legacySquelchEnabled
                                          ? receiverState().squelchLevelDb
                                          : receiverState().manualSquelchLevelDb;
    for (auto& entry : bookmarks) {
        if (!entry.bookmark.hasSavedSquelch) {
            entry.bookmark.squelchEnabled = legacySquelchEnabled;
            entry.bookmark.squelchThresholdDb = legacySquelchLevel;
            entry.bookmark.hasSavedSquelch = true;
        }
    }
    clearCenterFrequencyDigitEdit();
    cancelPendingManualTuning();
    m_scanStatusBeforeBookmarkScan = m_scanStatus;
    m_bookmarkScanSession = true;
    m_bookmarkScanBookmarks = std::move(bookmarks);
    const quint64 upper = static_cast<quint64>(m_bookmarkScanBookmarks.size() - 1);
    const sdr::app::CurrentPassbandScanSettings settings{
        0, upper, 1, m_bookmarkScanDwellMilliseconds,
        m_bookmarkScanResumeDelayMilliseconds};
    if (!m_scanner.start(settings, {0, upper}, 0, false)) {
        m_bookmarkScanSession = false;
        m_bookmarkScanBookmarks.clear();
        m_bookmarkScanStatus = QStringLiteral(
            "Scanner not started: invalid bookmark selection");
        emit bookmarkScannerChanged();
        return;
    }
    m_scanStatus = QStringLiteral("Starting bookmark scanner");
    notifyScanCurrentFrequencyChanged();
    static_cast<void>(tuneBookmarkScannerTo(0));
    emit bookmarkScannerChanged();
}

void ApplicationModel::pauseOrResumeBookmarkScan()
{
    if (bookmarkScanCanPauseResume()) pauseOrResumeActiveScan();
}

void ApplicationModel::skipBookmarkScan()
{
    if (bookmarkScanCanSkip()) skipActiveScan();
}

void ApplicationModel::stopBookmarkScan()
{
    if (bookmarkScanCanStop()) stopActiveScan();
}

void ApplicationModel::setSettingsPanelWidth(double width)
{
    const double boundedWidth = normalizedSettingsPanelWidth(width);
    if (qFuzzyCompare(m_settingsPanelWidth + 1.0, boundedWidth + 1.0)) {
        return;
    }
    m_settingsPanelWidth = boundedWidth;
    m_settingsPanelWidthPersistenceTimer.start();
    emit settingsPanelWidthChanged();
}

void ApplicationModel::commitSettingsPanelWidth()
{
    m_settingsPanelWidthPersistenceTimer.stop();
    persistSettingsPanelWidth();
}

void ApplicationModel::setConsolePanelWidth(double width)
{
    const double boundedWidth = normalizedConsolePanelWidth(width);
    if (qFuzzyCompare(m_consolePanelWidth + 1.0, boundedWidth + 1.0)) {
        return;
    }
    m_consolePanelWidth = boundedWidth;
    m_consolePanelWidthPersistenceTimer.start();
    emit consolePanelWidthChanged();
}

void ApplicationModel::commitConsolePanelWidth()
{
    m_consolePanelWidthPersistenceTimer.stop();
    persistConsolePanelWidth();
}

void ApplicationModel::setDsdFmeBinaryPath(const QString& path)
{
    const QString normalized = normalizedDsdFmeBinaryPath(path);
    if (m_dsdFmeBinaryPath != normalized) {
        m_dsdFmeBinaryPath = normalized;
        QSettings().setValue(dsdFmeBinaryPathSetting, m_dsdFmeBinaryPath);
        if (m_runtime) {
            m_runtime->setDsdFmeBinaryPath(m_dsdFmeBinaryPath);
        }
        emit dsdFmeBinaryPathChanged();
    }
    revalidateDsdFmeBinaryPath();
}

void ApplicationModel::setDsdFmeBinaryUrl(const QUrl& url)
{
    setDsdFmeBinaryPath(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

void ApplicationModel::revalidateDsdFmeBinaryPath()
{
    bool valid = false;
    const QString status = dsdFmeBinaryStatusForPath(
        m_dsdFmeBinaryPath, &valid);
    if (m_dsdFmeBinaryStatus == status && m_dsdFmeBinaryValid == valid) {
        return;
    }
    m_dsdFmeBinaryStatus = status;
    m_dsdFmeBinaryValid = valid;
    emit dsdFmeBinaryStatusChanged();
}

QString ApplicationModel::beginAddCurrentBookmark(int parentVisibleRow)
{
    const auto* demodulator = sdr::radio::DemodulatorRegistry::findByMode(
        receiverState().demodulationMode);
    if (!demodulator) {
        setStatusText(QStringLiteral(
            "The current demodulator is unavailable for bookmarks"));
        m_pendingBookmarkCreation.reset();
        return {};
    }
    const auto [filterLowHz, filterHighHz] = bookmarkFilterEdges(
        receiverState().demodulationMode,
        filterWidth());
    const double frequencyMhz =
        static_cast<double>(listeningFrequency()) / 1'000'000.0;
    sdr::app::BookmarkData bookmark;
    bookmark.name = QStringLiteral("%1 · %2 MHz")
                        .arg(QString::fromLatin1(
                            demodulator->displayName.data(),
                            static_cast<qsizetype>(
                                demodulator->displayName.size())))
                        .arg(frequencyMhz, 0, 'f', 3);
    bookmark.listeningFrequency = listeningFrequency();
    bookmark.requestedGainDb = requestedGain();
    bookmark.demodulatorId = QString::fromLatin1(
        demodulator->id.data(),
        static_cast<qsizetype>(demodulator->id.size()));
    bookmark.filterLowHz = filterLowHz;
    bookmark.filterHighHz = filterHighHz;
    bookmark.squelchThresholdDb =
        receiverState().squelchMode == sdr::radio::SquelchMode::Disabled
            ? receiverState().manualSquelchLevelDb
            : receiverState().squelchLevelDb;
    bookmark.squelchEnabled =
        receiverState().squelchMode != sdr::radio::SquelchMode::Disabled;
    bookmark.modeSpecificSettings = {
        {QStringLiteral("version"), 1},
    };
    bookmark.scannerIncluded = false;
    const QString suggestion = bookmark.name;
    m_pendingBookmarkCreation = PendingBookmarkCreation{
        parentVisibleRow,
        std::move(bookmark),
    };
    return suggestion;
}

bool ApplicationModel::confirmAddCurrentBookmark(const QString& name)
{
    if (!m_pendingBookmarkCreation || name.trimmed().isEmpty()) {
        return false;
    }
    PendingBookmarkCreation pending = std::move(*m_pendingBookmarkCreation);
    m_pendingBookmarkCreation.reset();
    pending.bookmark.name = name.trimmed();
    const QString uuid = m_bookmarkModel.addBookmark(
        pending.parentVisibleRow, pending.bookmark);
    if (uuid.isEmpty()) {
        setStatusText(QStringLiteral("Could not add the current bookmark"));
        return false;
    }
    setStatusText(QStringLiteral("Bookmark added"));
    return true;
}

void ApplicationModel::cancelAddCurrentBookmark()
{
    m_pendingBookmarkCreation.reset();
}

void ApplicationModel::editBookmark(int visibleRow, const QVariantMap& fields)
{
    const auto mode = sdr::radio::DemodulatorRegistry::resolve(
        fields.value(QStringLiteral("demodulatorId"))
            .toString().trimmed().toStdString());
    if (mode) {
        const auto width = bookmarkFilterWidth(*mode,
            fields.value(QStringLiteral("filterLowHz")).toLongLong(),
            fields.value(QStringLiteral("filterHighHz")).toLongLong());
        if (!width || !sdr::radio::filterWidthRange(
                           *mode, sampleRate()).contains(*width)) {
            setStatusText(QStringLiteral(
                "Filter edges are invalid for the selected demodulator"));
            return;
        }
    }
    if (!m_bookmarkModel.updateBookmark(visibleRow, fields)) {
        setStatusText(QStringLiteral("Bookmark fields are invalid"));
        return;
    }
    setStatusText(QStringLiteral("Bookmark updated"));
}

void ApplicationModel::updateCurrentBookmark(int selectedVisibleRow)
{
    QString targetUuid;
    if (selectedVisibleRow >= 0) {
        const QVariantMap selected = m_bookmarkModel.itemDetails(
            selectedVisibleRow);
        if (!selected.isEmpty() &&
            !selected.value(QStringLiteral("isGroup")).toBool()) {
            targetUuid = selected.value(QStringLiteral("uuid")).toString();
        }
    }
    if (targetUuid.isEmpty()) {
        targetUuid = m_loadedBookmarkUuid;
    }
    const int targetRow = m_bookmarkModel.visibleRowForUuid(targetUuid);
    const auto currentDetails = m_bookmarkModel.itemDetails(targetRow);
    if (targetRow < 0 || currentDetails.isEmpty() ||
        currentDetails.value(QStringLiteral("isGroup")).toBool()) {
        setStatusText(QStringLiteral("Select or tune a bookmark to update"));
        return;
    }

    const auto* demodulator = sdr::radio::DemodulatorRegistry::findByMode(
        receiverState().demodulationMode);
    if (!demodulator) {
        setStatusText(QStringLiteral(
            "The current demodulator is unavailable for bookmarks"));
        return;
    }
    const auto [filterLowHz, filterHighHz] = bookmarkFilterEdges(
        receiverState().demodulationMode, filterWidth());
    QVariantMap fields = currentDetails;
    fields.insert(QStringLiteral("listeningFrequency"),
        QVariant::fromValue<qulonglong>(listeningFrequency()));
    fields.insert(QStringLiteral("requestedGain"), requestedGain());
    fields.insert(QStringLiteral("demodulatorId"), QString::fromLatin1(
        demodulator->id.data(),
        static_cast<qsizetype>(demodulator->id.size())));
    fields.insert(QStringLiteral("filterLowHz"),
        QVariant::fromValue<qlonglong>(filterLowHz));
    fields.insert(QStringLiteral("filterHighHz"),
        QVariant::fromValue<qlonglong>(filterHighHz));
    fields.insert(QStringLiteral("squelchThreshold"),
        receiverState().squelchMode == sdr::radio::SquelchMode::Disabled
            ? receiverState().manualSquelchLevelDb
            : receiverState().squelchLevelDb);
    fields.insert(QStringLiteral("squelchEnabled"),
        receiverState().squelchMode != sdr::radio::SquelchMode::Disabled);
    if (!m_bookmarkModel.updateBookmark(targetRow, fields)) {
        setStatusText(QStringLiteral("Could not update the current bookmark"));
        return;
    }
    m_loadedBookmarkUuid = targetUuid;
    emit bookmarkUpdateAvailableChanged();
    setStatusText(QStringLiteral("Bookmark updated"));
}

void ApplicationModel::renameBookmarkGroup(int visibleRow, const QString& name)
{
    if (!m_bookmarkModel.renameGroup(visibleRow, name)) {
        setStatusText(QStringLiteral("Group name cannot be empty"));
        return;
    }
    setStatusText(QStringLiteral("Group renamed"));
}

void ApplicationModel::tuneBookmark(int visibleRow)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    const auto bookmark = m_bookmarkModel.bookmarkAt(visibleRow);
    if (!bookmark) {
        setStatusText(QStringLiteral("Select a bookmark to tune"));
        return;
    }
    const auto mode = sdr::radio::DemodulatorRegistry::resolve(
        bookmark->demodulatorId.toStdString());
    if (!mode) {
        setStatusText(QStringLiteral(
            "This bookmark's demodulator is unavailable"));
        return;
    }
    const auto width = bookmarkFilterWidth(
        *mode, bookmark->filterLowHz, bookmark->filterHighHz);
    if (!width) {
        setStatusText(QStringLiteral(
            "Bookmark filter edges do not match its demodulator"));
        return;
    }
    const bool applySquelch = bookmark->hasSavedSquelch;
    const QString uuid = m_bookmarkModel.itemDetails(visibleRow)
                             .value(QStringLiteral("uuid")).toString();
    if (m_runtime) {
        m_runtime->applyBookmark(
            bookmark->listeningFrequency,
            bookmark->requestedGainDb,
            static_cast<int>(*mode),
            *width,
            bookmark->squelchThresholdDb,
            bookmark->squelchEnabled,
            applySquelch);
        if (m_loadedBookmarkUuid != uuid) {
            m_loadedBookmarkUuid = uuid;
            emit bookmarkUpdateAvailableChanged();
        }
        return;
    }
    const auto previousState = m_receiver->state();
    const auto apply = [this](sdr::radio::OperationResult result) {
        if (result.succeeded()) {
            return true;
        }
        setStatusText(QString::fromStdString(result.message));
        return false;
    };
    if (!apply(m_receiver->setCenterFrequency(bookmark->listeningFrequency)) ||
        !apply(m_receiver->setGain(bookmark->requestedGainDb)) ||
        !apply(m_receiver->setDemodulationMode(*mode)) ||
        !apply(m_receiver->setFilterWidth(*width)) ||
        (applySquelch &&
         (!apply(m_receiver->setSquelchLevel(bookmark->squelchThresholdDb)) ||
          (!bookmark->squelchEnabled && !apply(m_receiver->disableSquelch()))))) {
        notifyStateChanges(previousState, m_receiver->state(), false);
        return;
    }
    m_requestedGainDb = bookmark->requestedGainDb;
    notifyStateChanges(previousState, m_receiver->state(), true);
    if (m_loadedBookmarkUuid != uuid) {
        m_loadedBookmarkUuid = uuid;
        emit bookmarkUpdateAvailableChanged();
    }
    setStatusText(QStringLiteral("Bookmark tuned"));
}

void ApplicationModel::reportWaterfallHistoryMetrics(
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
    quint64 viewportRows)
{
    const bool configurationChanged =
        requestedSeconds != m_lastWaterfallHistoryRequestedSeconds ||
        storedBins != m_lastWaterfallHistoryStoredBins ||
        fitsMemoryBudget != m_lastWaterfallHistoryFit;
    if (!fitsMemoryBudget && configurationChanged) {
        qWarning().noquote()
            << QStringLiteral(
                   "Waterfall history request exceeds the memory budget after reducing horizontal storage: requested=%1 s capacity=%2 s stored-bins=%3 budget=%4 bytes")
                   .arg(requestedSeconds)
                   .arg(retainedCapacitySeconds, 0, 'f', 2)
                   .arg(storedBins)
                   .arg(waterfallHistoryMemoryBudget);
    }
    m_lastWaterfallHistoryFit = fitsMemoryBudget;
    m_lastWaterfallHistoryRequestedSeconds = requestedSeconds;
    m_lastWaterfallHistoryStoredBins = storedBins;
    if (!m_verboseDiagnostics) {
        return;
    }
    if (m_waterfallHistoryMetricsTimer.isValid() &&
        m_waterfallHistoryMetricsTimer.elapsed() < 1'000) {
        return;
    }
    const double elapsedSeconds = m_waterfallHistoryMetricsTimer.isValid()
                                      ? static_cast<double>(
                                            m_waterfallHistoryMetricsTimer.elapsed()) /
                                            1'000.0
                                      : 1.0;
    const quint64 renderedDelta = renderedFrames >= m_lastRenderedWaterfallFrames
                                      ? renderedFrames - m_lastRenderedWaterfallFrames
                                      : renderedFrames;
    const quint64 mergedDelta = mergedRenderUpdates >= m_lastMergedWaterfallUpdates
                                    ? mergedRenderUpdates - m_lastMergedWaterfallUpdates
                                    : mergedRenderUpdates;
    qInfo().noquote()
        << QStringLiteral(
               "waterfall render: rendered=%1 frames/s merged-updates=%2 compact-memory=%3 bytes compact-budget=%4 bytes compact-rows=%5 stored-bins=%6 requested=%7 s capacity=%8 s retained=%9 s fits-budget=%10 capture-span=%11 Hz visible-span=%12 Hz zoom=%13x visible-center=%14 Hz listening-anchor=%15 zoom-events=%16 coalesced=%17 reprojection=%18 ms stale-reprojections=%19 viewport-memory=%20 bytes viewport-budget=%21 bytes viewport-rows=%22")
               .arg(static_cast<double>(renderedDelta) / elapsedSeconds, 0, 'f', 1)
               .arg(mergedDelta)
               .arg(bytesUsed)
               .arg(waterfallHistoryMemoryBudget)
               .arg(retainedRows)
               .arg(storedBins)
               .arg(requestedSeconds)
               .arg(retainedCapacitySeconds, 0, 'f', 2)
               .arg(retainedSeconds, 0, 'f', 2)
               .arg(fitsMemoryBudget ? QStringLiteral("yes")
                                     : QStringLiteral("no"))
               .arg(effectiveSampleRate())
               .arg(visibleSpan())
               .arg(displayZoomFactor(), 0, 'f', 2)
               .arg(visibleCenterFrequency())
               .arg(listeningPosition(), 0, 'f', 3)
               .arg(m_waterfallZoomEvents)
               .arg(m_coalescedWaterfallZoomEvents)
               .arg(reprojectionMilliseconds, 0, 'f', 2)
               .arg(staleReprojectionsDiscarded)
               .arg(viewportBytesUsed)
               .arg(viewportMemoryBudget)
               .arg(viewportRows);
    m_lastRenderedWaterfallFrames = renderedFrames;
    m_lastMergedWaterfallUpdates = mergedRenderUpdates;
    if (m_waterfallHistoryMetricsTimer.isValid()) {
        m_waterfallHistoryMetricsTimer.restart();
    } else {
        m_waterfallHistoryMetricsTimer.start();
    }
}

void ApplicationModel::setFilterWidth(quint64 newFilterWidth)
{
    if (m_runtime) {
        m_runtime->setFilterWidth(newFilterWidth);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->setFilterWidth(newFilterWidth));
}

void ApplicationModel::setFilterWidthText(const QString& filterWidthText)
{
    QString text = filterWidthText.trimmed();
    const bool kilohertz = text.contains(QStringLiteral("k"), Qt::CaseInsensitive);
    text.remove(QStringLiteral("kHz"), Qt::CaseInsensitive);
    text.remove(QStringLiteral("Hz"), Qt::CaseInsensitive);
    bool valid = false;
    const double value = text.trimmed().toDouble(&valid);
    if (!valid || !std::isfinite(value) || value <= 0.0) {
        setStatusText(QStringLiteral("Filter width must be a positive numeric value"));
        return;
    }
    setFilterWidth(static_cast<quint64>(std::llround(
        kilohertz ? value * 1'000.0 : value)));
}

void ApplicationModel::setGain(double gainDb)
{
    m_requestedGainDb = gainDb;
    emit requestedGainChanged();
    if (m_runtime) {
        m_runtime->setGain(gainDb);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->setGain(gainDb));
}

void ApplicationModel::previewGain(double gainDb)
{
    // The slider keeps this displayed value responsive and commits only on
    // release, preventing a stream of hardware gain writes while dragging.
    if (m_requestedGainDb != gainDb) {
        m_requestedGainDb = gainDb;
        emit requestedGainChanged();
    }
}

void ApplicationModel::commitGain(double gainDb)
{
    setGain(gainDb);
}

void ApplicationModel::setPpmCorrection(double newPpmCorrection)
{
    if (m_runtime) {
        m_runtime->setPpmCorrection(newPpmCorrection);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->setPpmCorrection(newPpmCorrection));
}

void ApplicationModel::startAutomaticPpmCalibration()
{
    if (m_runtime) {
        m_runtime->startAutomaticPpmCalibration();
        return;
    }
    setStatusText(QStringLiteral(
        "Automatic PPM calibration requires the hardware runtime"));
}

void ApplicationModel::cancelAutomaticPpmCalibration()
{
    if (m_runtime) {
        m_runtime->cancelAutomaticPpmCalibration();
    }
}

void ApplicationModel::setDemodulationModeIndex(int modeIndex)
{
    if (modeIndex < static_cast<int>(sdr::radio::DemodulationMode::Am) ||
        modeIndex > static_cast<int>(
                        sdr::radio::DemodulationMode::DigitalDecoderOutput)) {
        setStatusText(QStringLiteral("Unsupported demodulation mode"));
        return;
    }

    if (m_runtime) {
        m_runtime->setDemodulationMode(modeIndex);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(
        previousState,
        m_receiver->setDemodulationMode(
            static_cast<sdr::radio::DemodulationMode>(modeIndex)));
}

void ApplicationModel::setSquelchLevel(double squelchLevelDb)
{
    if (m_runtime) {
        m_runtime->setSquelchLevel(squelchLevelDb);
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->setSquelchLevel(squelchLevelDb));
}

void ApplicationModel::enableManualSquelch()
{
    if (m_runtime) {
        m_runtime->enableManualSquelch();
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->enableManualSquelch());
}

void ApplicationModel::enableAutomaticSquelch()
{
    if (m_runtime) {
        m_runtime->enableAutomaticSquelch();
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->enableAutomaticSquelch());
}

void ApplicationModel::setAutomaticSquelchEnabled(bool enabled)
{
    if (enabled) {
        enableAutomaticSquelch();
    } else {
        enableManualSquelch();
    }
}

void ApplicationModel::disableSquelch()
{
    if (m_runtime) {
        m_runtime->disableSquelch();
        return;
    }
    const auto previousState = m_receiver->state();
    applyOperation(previousState, m_receiver->disableSquelch());
}

void ApplicationModel::setSquelchDisabled(bool disabled)
{
    if (disabled) {
        disableSquelch();
    } else {
        enableManualSquelch();
    }
}

void ApplicationModel::applyOperation(
    const sdr::radio::ReceiverState& previousState,
    sdr::radio::OperationResult result)
{
    const auto& state = m_receiver->state();
    notifyStateChanges(previousState, state, result.succeeded());
    setStatusText(QString::fromStdString(result.message));
}

void ApplicationModel::applyRuntimeSnapshot(
    const sdr::app::ReceiverRuntimeSnapshot& snapshot)
{
    const auto previousState = m_runtimeState;
    const quint64 previousEffectiveSampleRate = m_runtimeEffectiveSampleRate;
    const quint64 previousSpectrumFftSize = m_spectrumFftSize;
    const quint64 previousEffectiveSpectrumFftSize =
        m_effectiveSpectrumFftSize;
    const double previousSpectrumHertzPerBin = m_spectrumHertzPerBin;
    const double previousEffectiveWaterfallRowsPerSecond =
        m_effectiveWaterfallRowsPerSecond;
    const quint32 previousVisibleWaterfallHistorySeconds =
        m_visibleWaterfallHistorySeconds;
    const QStringList previousIdentifiers = m_deviceIdentifiers;
    const QStringList previousDisplayNames = m_deviceDisplayNames;
    const QString previousSelectedIdentifier = m_selectedDeviceIdentifier;
    const QString previousDeviceState = m_deviceState;
    const QString previousBackendDescription = m_backendDescription;
    const QString previousCapabilitySummary = m_deviceCapabilitySummary;
    const auto previousCapabilities = m_runtimeCapabilities;
    const auto previousFrequencyRanges = m_deviceFrequencyRanges;
    const auto previousSampleRateRanges = m_deviceSampleRateRanges;
    const QStringList previousCaptureBandwidthOptions = m_captureBandwidthOptions;
    const bool previousCustomCaptureBandwidthSupported =
        m_customCaptureBandwidthSupported;
    const double previousMinimumGain = m_minimumGainDb;
    const double previousMaximumGain = m_maximumGainDb;
    const double previousGainStep = m_gainStepDb;
    const double previousRequestedGain = m_requestedGainDb;
    const bool previousGainSupported = m_gainSupported;
    const bool previousBackendReady = m_backendReady;
    const bool previousDiscoveryAvailable = m_deviceDiscoveryAvailable;
    const bool previousMockMode = m_mockMode;
    const bool previousAutomaticPpmCalibrationSupported =
        m_automaticPpmCalibrationSupported;
    const bool previousPpmCalibrationRunning = m_ppmCalibrationRunning;
    const QString previousPpmCalibrationStatus =
        m_ppmCalibrationStatus;
    const int previousPpmCalibrationProgressPercent =
        m_ppmCalibrationProgressPercent;
    const quint64 previousPpmCalibrationDisplayResetGeneration =
        m_ppmCalibrationDisplayResetGeneration;
    const QStringList previousAudioIdentifiers = m_audioDeviceIdentifiers;
    const QStringList previousAudioDisplayNames = m_audioDeviceDisplayNames;
    const QString previousSelectedAudioIdentifier =
        m_selectedAudioDeviceIdentifier;
    const QString previousAudioStatus = m_audioStatusText;
    const QString previousDsdFmeStatus = m_dsdFmeStatusText;
    const int previousAudioVolume = m_audioVolumePercent;
    const bool previousAudioMuted = m_audioMuted;
    const bool previousAudioReady = m_audioReady;
    const bool previousAudioRunning = m_audioRunning;
    const quint64 previousAudioOverflowEvents = m_audioOverflowEvents;
    const quint64 previousAudioUnderrunEvents = m_audioUnderrunEvents;

    m_runtimeState = snapshot.receiverState;
    m_runtimeSquelchOpen = snapshot.squelchOpen;
    m_runtimeLimits = snapshot.receiverLimits;
    m_runtimeCapabilities = snapshot.receiverCapabilities;
    m_runtimeEffectiveSampleRate = snapshot.effectiveSampleRate == 0
                                      ? snapshot.receiverState.sampleRate
                                      : snapshot.effectiveSampleRate;
    m_spectrumFftSize = snapshot.spectrumFftSize;
    m_effectiveSpectrumFftSize = snapshot.effectiveSpectrumFftSize;
    m_spectrumHertzPerBin = snapshot.spectrumHertzPerBin;
    m_effectiveWaterfallRowsPerSecond =
        snapshot.effectiveSpectrumFramesPerSecond;
    m_visibleWaterfallHistorySeconds = snapshot.visibleWaterfallHistorySeconds;
    m_deviceIdentifiers = snapshot.deviceIdentifiers;
    m_deviceDisplayNames = snapshot.deviceDisplayNames;
    m_selectedDeviceIdentifier = snapshot.selectedDeviceIdentifier;
    m_deviceState = snapshot.deviceState;
    m_backendDescription = snapshot.backendDescription;
    m_deviceCapabilitySummary = snapshot.deviceCapabilitySummary;
    if (snapshot.selectedDeviceIdentifier.isEmpty() ||
        snapshot.deviceFrequencyRanges.empty()) {
        m_deviceFrequencyRanges.reset();
    } else {
        m_deviceFrequencyRanges = snapshot.deviceFrequencyRanges;
    }
    m_deviceSampleRateRanges = snapshot.deviceSampleRateRanges;
    m_captureBandwidthOptions = snapshot.captureBandwidthOptions;
    m_customCaptureBandwidthSupported = snapshot.customCaptureBandwidthSupported;
    m_minimumGainDb = snapshot.minimumGainDb;
    m_maximumGainDb = snapshot.maximumGainDb;
    m_gainStepDb = snapshot.gainStepDb;
    m_requestedGainDb = snapshot.requestedGainDb;
    m_gainSupported = snapshot.gainSupported;
    m_backendReady = snapshot.backendReady;
    m_deviceDiscoveryAvailable = snapshot.discoveryAvailable;
    m_mockMode = snapshot.mockMode;
    m_automaticPpmCalibrationSupported =
        snapshot.automaticPpmCalibrationSupported;
    m_ppmCalibrationRunning = snapshot.ppmCalibrationRunning;
    m_ppmCalibrationStatus = snapshot.ppmCalibrationStatus;
    m_ppmCalibrationProgressPercent =
        snapshot.ppmCalibrationProgressPercent;
    m_ppmCalibrationDisplayResetGeneration =
        snapshot.ppmCalibrationDisplayResetGeneration;
    m_audioDeviceIdentifiers = snapshot.audioDeviceIdentifiers;
    m_audioDeviceDisplayNames = snapshot.audioDeviceDisplayNames;
    m_selectedAudioDeviceIdentifier = snapshot.selectedAudioDeviceIdentifier;
    m_audioStatusText = snapshot.audioStatusText;
    m_dsdFmeStatusText = snapshot.dsdFmeStatusText;
    m_audioVolumePercent = snapshot.audioVolumePercent;
    m_audioMuted = snapshot.audioMuted;
    m_audioReady = snapshot.audioReady;
    m_audioRunning = snapshot.audioRunning;
    m_audioOverflowEvents = snapshot.audioOverflowEvents;
    m_audioUnderrunEvents = snapshot.audioUnderrunEvents;
    if (m_audioUnderrunEvents > previousAudioUnderrunEvents) {
        m_applicationLog.post(
            sdr::app::ApplicationLogModel::Warning,
            QStringLiteral("Audio"),
            QStringLiteral("Output underrun count increased to %1")
                .arg(m_audioUnderrunEvents));
    }
    if (m_audioOverflowEvents > previousAudioOverflowEvents) {
        m_applicationLog.post(
            sdr::app::ApplicationLogModel::Warning,
            QStringLiteral("Audio"),
            QStringLiteral("Output overflow count increased to %1")
                .arg(m_audioOverflowEvents));
    }
    if (previousAudioStatus != m_audioStatusText &&
        !m_audioStatusText.trimmed().isEmpty()) {
        m_applicationLog.post(
            m_audioReady ? sdr::app::ApplicationLogModel::Info
                         : sdr::app::ApplicationLogModel::Warning,
            QStringLiteral("Audio"),
            m_audioStatusText);
    }
    if (previousDeviceState != m_deviceState &&
        !m_deviceState.trimmed().isEmpty()) {
        m_applicationLog.post(
            snapshot.operationSucceeded
                ? sdr::app::ApplicationLogModel::Info
                : sdr::app::ApplicationLogModel::Error,
            QStringLiteral("SDR"),
            m_deviceState);
    }

    bool listeningWheelRecentered = false;
    if (m_pendingListeningWheelFrequency.has_value() &&
        snapshot.operationSucceeded &&
        snapshot.receiverState.listeningFrequency ==
            *m_pendingListeningWheelFrequency) {
        listeningWheelRecentered =
            m_frequencyViewport.centerOn(
                snapshot.receiverState.listeningFrequency);
        m_pendingListeningWheelFrequency.reset();
    } else if (!snapshot.operationSucceeded) {
        m_pendingListeningWheelFrequency.reset();
    }

    auto notificationState = m_runtimeState;
    if (m_pendingCenterWheelFrequency.has_value()) {
        notificationState.centerFrequency = previousState.centerFrequency;
        notificationState.listeningFrequency = previousState.listeningFrequency;
    }
    notifyStateChanges(
        previousState, notificationState, snapshot.operationSucceeded);
    updateScannerSquelchActivity();
    if (listeningWheelRecentered) {
        emitFrequencyViewportChanges();
    }
    if (previousEffectiveSampleRate != m_runtimeEffectiveSampleRate) {
        resetSpectrumFrame();
        emit effectiveSampleRateChanged();
    }
    if (previousState.centerFrequency == m_runtimeState.centerFrequency &&
        (previousEffectiveSampleRate != m_runtimeEffectiveSampleRate ||
         previousFrequencyRanges != m_deviceFrequencyRanges) &&
        m_frequencyViewport.configureCapture(
            centerFrequency(),
            effectiveSampleRate(),
            listeningFrequency(),
            advertisedRfRangeForCenter(centerFrequency()))) {
        emitFrequencyViewportChanges();
    }
    if (previousEffectiveSampleRate != m_runtimeEffectiveSampleRate ||
        previousFrequencyRanges != m_deviceFrequencyRanges) {
        validateActiveScanRange();
    }
    if (previousEffectiveSpectrumFftSize != m_effectiveSpectrumFftSize) {
        resetSpectrumFrame();
        if (m_frequencyViewport.configureDetail(
                m_effectiveSpectrumFftSize,
                filterWidth(),
                listeningFrequency(),
                true,
                passbandAlignment(receiverState().demodulationMode))) {
            emitFrequencyViewportChanges();
        }
    }
    if (previousEffectiveSampleRate != m_runtimeEffectiveSampleRate ||
        previousSpectrumFftSize != m_spectrumFftSize ||
        previousEffectiveSpectrumFftSize != m_effectiveSpectrumFftSize ||
        previousSpectrumHertzPerBin != m_spectrumHertzPerBin) {
        emit spectrumFftSizeChanged();
    }
    if (previousEffectiveWaterfallRowsPerSecond !=
            m_effectiveWaterfallRowsPerSecond ||
        previousVisibleWaterfallHistorySeconds !=
            m_visibleWaterfallHistorySeconds) {
        emit waterfallSettingsChanged();
    }

    if (previousIdentifiers != m_deviceIdentifiers ||
        previousDisplayNames != m_deviceDisplayNames) {
        emit devicesChanged();
    }
    if (previousSelectedIdentifier != m_selectedDeviceIdentifier ||
        previousDeviceState != m_deviceState ||
        previousBackendDescription != m_backendDescription ||
        previousBackendReady != m_backendReady ||
        previousDiscoveryAvailable != m_deviceDiscoveryAvailable ||
        previousMockMode != m_mockMode) {
        emit deviceStateChanged();
    }
    if (previousCapabilitySummary != m_deviceCapabilitySummary ||
        previousCapabilities.ppmCorrectionSupported !=
            m_runtimeCapabilities.ppmCorrectionSupported ||
        previousCapabilities.automaticPpmCalibrationSupported !=
            m_runtimeCapabilities.automaticPpmCalibrationSupported ||
        previousFrequencyRanges != m_deviceFrequencyRanges ||
        previousSampleRateRanges != m_deviceSampleRateRanges ||
        previousCaptureBandwidthOptions != m_captureBandwidthOptions ||
        previousCustomCaptureBandwidthSupported != m_customCaptureBandwidthSupported ||
        previousMinimumGain != m_minimumGainDb ||
        previousMaximumGain != m_maximumGainDb ||
        previousGainStep != m_gainStepDb ||
        previousGainSupported != m_gainSupported) {
        emit deviceCapabilitiesChanged();
    }
    if (previousAutomaticPpmCalibrationSupported !=
            m_automaticPpmCalibrationSupported ||
        previousPpmCalibrationRunning != m_ppmCalibrationRunning ||
        previousPpmCalibrationStatus != m_ppmCalibrationStatus ||
        previousPpmCalibrationProgressPercent !=
            m_ppmCalibrationProgressPercent) {
        emit ppmCalibrationChanged();
    }
    if (previousPpmCalibrationDisplayResetGeneration !=
        m_ppmCalibrationDisplayResetGeneration) {
        resetSpectrumFrame();
        emit waterfallReset();
    }
    if (previousRequestedGain != m_requestedGainDb) {
        emit requestedGainChanged();
    }
    if (previousCaptureBandwidthOptions != m_captureBandwidthOptions ||
        previousCustomCaptureBandwidthSupported != m_customCaptureBandwidthSupported) {
        emit captureBandwidthCapabilitiesChanged();
    }
    if (previousAudioIdentifiers != m_audioDeviceIdentifiers ||
        previousAudioDisplayNames != m_audioDeviceDisplayNames) {
        emit audioDevicesChanged();
    }
    if (previousSelectedAudioIdentifier != m_selectedAudioDeviceIdentifier ||
        previousAudioStatus != m_audioStatusText ||
        previousDsdFmeStatus != m_dsdFmeStatusText ||
        previousAudioVolume != m_audioVolumePercent ||
        previousAudioMuted != m_audioMuted ||
        previousAudioReady != m_audioReady ||
        previousAudioRunning != m_audioRunning ||
        previousAudioOverflowEvents != m_audioOverflowEvents ||
        previousAudioUnderrunEvents != m_audioUnderrunEvents) {
        emit audioStateChanged();
    }
    if (m_runtimeBusy != m_ppmCalibrationRunning) {
        m_runtimeBusy = m_ppmCalibrationRunning;
        emit runtimeBusyChanged();
    }
    setStatusText(snapshot.statusText);
}

void ApplicationModel::applyScannerListeningFrequency(
    quint64 frequency,
    bool succeeded,
    const QString& message)
{
    if (!succeeded) {
        setStatusText(message);
        stopScanner(QStringLiteral("Scanner stopped: %1").arg(message));
        return;
    }
    if (m_runtimeState.listeningFrequency == frequency) {
        if (m_pendingBookmarkListeningFrequency == frequency) {
            m_pendingBookmarkListeningFrequency.reset();
            if (m_stopScanAfterRetune) {
                stopScanner(QStringLiteral("Scanner stopped"));
                return;
            }
            m_bookmarkScannerAwaitingSettle = true;
            m_scanStatus = QStringLiteral("Applying bookmark settings");
            m_scanSettlingTimer.start(scannerTunerSettlingMilliseconds);
            emit scannerChanged();
            return;
        }
        if (m_pendingWideListeningFrequency == frequency) {
            m_pendingWideListeningFrequency.reset();
            if (m_stopScanAfterRetune) {
                stopScanner(QStringLiteral("Scanner stopped"));
            } else {
                finishWideRangeRetune();
            }
        }
        return;
    }
    const auto previousState = m_runtimeState;
    m_runtimeState.listeningFrequency = frequency;
    notifyStateChanges(previousState, m_runtimeState, true);
    if (m_pendingBookmarkListeningFrequency == frequency) {
        m_pendingBookmarkListeningFrequency.reset();
        if (m_stopScanAfterRetune) {
            stopScanner(QStringLiteral("Scanner stopped"));
            return;
        }
        m_bookmarkScannerAwaitingSettle = true;
        m_scanStatus = QStringLiteral("Applying bookmark settings");
        m_scanSettlingTimer.start(scannerTunerSettlingMilliseconds);
        emit scannerChanged();
        return;
    }
    if (m_pendingWideListeningFrequency == frequency) {
        m_pendingWideListeningFrequency.reset();
        if (m_stopScanAfterRetune) {
            stopScanner(QStringLiteral("Scanner stopped"));
        } else {
            finishWideRangeRetune();
        }
    }
}

void ApplicationModel::applyScannerCenterFrequency(
    quint64 requestedFrequency,
    quint64 appliedCenterFrequency,
    quint64 appliedListeningFrequency,
    bool succeeded,
    const QString& message)
{
    if (succeeded) {
        const auto previousState = m_runtimeState;
        m_runtimeState.centerFrequency = appliedCenterFrequency;
        m_runtimeState.listeningFrequency = appliedListeningFrequency;
        notifyStateChanges(previousState, m_runtimeState, true);
    } else {
        setStatusText(message);
    }
    finishScannerCentering(requestedFrequency, succeeded);
}

void ApplicationModel::finishCenterFrequencyRequest(
    quint64 frequency, bool succeeded)
{
    finishCenterWheelRequest(frequency, succeeded);
    finishScannerCentering(frequency, succeeded);
}

void ApplicationModel::notifyStateChanges(
    const sdr::radio::ReceiverState& previousState,
    const sdr::radio::ReceiverState& state,
    bool operationSucceeded)
{
    if (previousState.running && !state.running && scannerOwnsTuning()) {
        stopScanner(QStringLiteral("Scanner stopped: reception stopped"));
    }
    bool viewportChanged = false;
    const bool scanGeometryChanged =
        state.centerFrequency != previousState.centerFrequency ||
        state.sampleRate != previousState.sampleRate ||
        state.filterWidth != previousState.filterWidth ||
        state.demodulationMode != previousState.demodulationMode;
    if (wideRangeScanActive() && scanGeometryChanged) {
        m_wideRangePlanDirty = true;
    }
    if (state.centerFrequency != previousState.centerFrequency ||
        state.sampleRate != previousState.sampleRate) {
        viewportChanged = m_frequencyViewport.configureCapture(
            state.centerFrequency,
            effectiveSampleRate(),
            state.listeningFrequency,
            advertisedRfRangeForCenter(state.centerFrequency));
        validateActiveScanRange();
    }
    if (state.filterWidth != previousState.filterWidth ||
        state.demodulationMode != previousState.demodulationMode) {
        viewportChanged =
            m_frequencyViewport.configureDetail(
                m_effectiveSpectrumFftSize,
                state.filterWidth,
                state.listeningFrequency,
                false,
                passbandAlignment(state.demodulationMode)) ||
            viewportChanged;
        validateActiveScanRange();
    }

    if (state.centerFrequency != previousState.centerFrequency) {
        emit centerFrequencyChanged();
        emit centerFrequencyDigitsChanged();
    }
    if (state.listeningFrequency != previousState.listeningFrequency) {
        emit listeningFrequencyChanged();
    }
    if (viewportChanged) {
        emit frequencyViewportChanged();
        emit visibleRangeChanged();
    }
    if (state.centerFrequency != previousState.centerFrequency ||
        state.listeningFrequency != previousState.listeningFrequency ||
        state.sampleRate != previousState.sampleRate ||
        viewportChanged) {
        emit listeningPositionChanged();
    }
    if (state.centerFrequency != previousState.centerFrequency ||
        state.listeningFrequency != previousState.listeningFrequency ||
        state.sampleRate != previousState.sampleRate ||
        state.filterWidth != previousState.filterWidth) {
        emit filterMarkerChanged();
    }
    if (state.sampleRate != previousState.sampleRate) {
        m_spectrumHertzPerBin = static_cast<double>(effectiveSampleRate()) /
                                static_cast<double>(m_effectiveSpectrumFftSize);
        resetSpectrumFrame();
        emit sampleRateChanged();
        emit spectrumFftSizeChanged();
        emit filterLimitsChanged();
        emit filterPresetsChanged();
    }
    if (state.filterWidth != previousState.filterWidth) {
        emit filterWidthChanged();
        emit filterPresetsChanged();
    }
    if (state.gainDb != previousState.gainDb) {
        emit gainChanged();
    }
    if (state.ppmCorrection != previousState.ppmCorrection) {
        emit ppmCorrectionChanged();
    }
    if (state.demodulationMode != previousState.demodulationMode) {
        m_pendingFilterWidthWheelDelta = 0;
        m_filterWidthWheelRemainder = 0;
        m_filterWidthWheelTimer.stop();
        emit demodulationModeChanged();
        emit filterLimitsChanged();
        emit filterPresetsChanged();
    }
    if (state.squelchLevelDb != previousState.squelchLevelDb ||
        state.squelchMode != previousState.squelchMode) {
        emit squelchStateChanged();
    }
    if (state.running != previousState.running) {
        if (!state.running) {
            resetSpectrumFrame();
            emit waterfallReset();
        }
        emit receiverRunningChanged();
        emit scannerChanged();
    }

    if (!operationSucceeded) {
        // A Qt control may have changed its local display value before the
        // backend rejects an operation. Re-notify confirmed values so QML
        // never presents an unconfirmed receiver state.
        emit filterWidthChanged();
        emit gainChanged();
        emit ppmCorrectionChanged();
        emit demodulationModeChanged();
        emit squelchStateChanged();
        emit receiverRunningChanged();
    }

}

void ApplicationModel::receiveRuntimeSpectrumFrame(
    const QVector<float>& normalizedMagnitudes,
    quint64 frameCenterFrequency,
    quint64 frameSampleRate,
    quint64 frameFftSize,
    quint64 frameSequence,
    quint64 frameTimestampNanoseconds,
    quint64 frameTuningGeneration)
{
    if (!canPublishSpectrumFrame(
            static_cast<quint64>(normalizedMagnitudes.size()),
            frameCenterFrequency,
            frameSampleRate,
            frameFftSize,
            frameSequence,
            frameTimestampNanoseconds)) {
        return;
    }
    recordPublishedSpectrumFrame(
        frameSequence, frameTimestampNanoseconds);
    emit spectrumFrameReady(
        normalizedMagnitudes,
        frameCenterFrequency,
        frameSampleRate,
        frameFftSize,
        frameSequence,
        frameTimestampNanoseconds,
        frameTuningGeneration);
}

void ApplicationModel::receiveRuntimeWaterfallFrame(
    const QVector<float>& normalizedMagnitudes,
    quint64 frameCenterFrequency,
    quint64 frameSampleRate,
    quint64 frameFftSize,
    quint64 frameSequence,
    quint64 frameTimestampNanoseconds,
    quint64 frameTuningGeneration)
{
    if (normalizedMagnitudes.size() < 2 || frameSequence == 0 ||
        frameTimestampNanoseconds == 0 ||
        frameFftSize != static_cast<quint64>(normalizedMagnitudes.size()) ||
        frameFftSize != m_effectiveSpectrumFftSize ||
        frameSampleRate != effectiveSampleRate()) {
        return;
    }
    emit waterfallFrameReady(
        normalizedMagnitudes,
        frameCenterFrequency,
        frameSampleRate,
        frameFftSize,
        frameSequence,
        frameTimestampNanoseconds,
        frameTuningGeneration);
}

const sdr::radio::ReceiverState& ApplicationModel::receiverState() const noexcept
{
    return m_runtime ? m_runtimeState : m_receiver->state();
}

const sdr::radio::ReceiverLimits& ApplicationModel::receiverLimits() const noexcept
{
    return m_runtime ? m_runtimeLimits : m_receiver->limits();
}

const sdr::radio::ReceiverCapabilities&
ApplicationModel::receiverCapabilities() const noexcept
{
    return m_runtime ? m_runtimeCapabilities : m_receiver->capabilities();
}

sdr::radio::ReceiverState ApplicationModel::displayState() const noexcept
{
    auto state = receiverState();
    state.sampleRate = effectiveSampleRate();
    if (m_pendingCenterWheelFrequency.has_value()) {
        state.centerFrequency = *m_pendingCenterWheelFrequency;
        state.listeningFrequency = *m_pendingCenterWheelFrequency;
    }
    return state;
}

sdr::radio::FrequencyRange ApplicationModel::advertisedRfRangeForCenter(
    std::uint64_t centerFrequency) const noexcept
{
    if (m_deviceFrequencyRanges.has_value()) {
        const auto matching = std::find_if(
            m_deviceFrequencyRanges->begin(),
            m_deviceFrequencyRanges->end(),
            [centerFrequency](const auto range) {
                return range.contains(centerFrequency);
            });
        return matching == m_deviceFrequencyRanges->end()
                   ? sdr::radio::FrequencyRange{}
                   : *matching;
    }
    return receiverLimits().frequency;
}

std::vector<sdr::radio::FrequencyRange>
ApplicationModel::effectiveCenterFrequencyRanges() const
{
    const auto receiverRange = sdr::radio::validCenterFrequencyRange(
        receiverLimits(), receiverState().sampleRate);
    if (!m_deviceFrequencyRanges.has_value()) {
        return {receiverRange};
    }

    std::vector<sdr::radio::FrequencyRange> intersections;
    intersections.reserve(m_deviceFrequencyRanges->size());
    for (const auto deviceRange : *m_deviceFrequencyRanges) {
        const sdr::radio::FrequencyRange intersection{
            std::max(receiverRange.minimum, deviceRange.minimum),
            std::min(receiverRange.maximum, deviceRange.maximum),
        };
        if (intersection.maximum >= intersection.minimum) {
            intersections.push_back(intersection);
        }
    }
    return intersections;
}

void ApplicationModel::applyCenterFrequencyEdit(
    const sdr::app::FrequencyEditResult& edit)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    clearCenterFrequencyDigitEdit();
    if (!edit.succeeded()) {
        setStatusText(QString::fromStdString(edit.message));
        return;
    }

    if (m_runtime) {
        if (m_pendingTuningWheelAction == PendingTuningWheelAction::Center) {
            m_pendingTuningWheelAction = PendingTuningWheelAction::None;
            m_pendingWheelTuningShift = 0;
            m_wheelTuningTimer.stop();
            clearCenterWheelPreview(true);
        } else {
            flushPendingWheelTuning();
        }
        m_runtime->setCenterFrequency(edit.frequency);
        if (edit.adjustedToLimit) {
            const QString limitSource = m_deviceFrequencyRanges.has_value()
                                            ? QStringLiteral("receiver and device limits")
                                            : QStringLiteral("receiver limits");
            setStatusText(
                QStringLiteral("Requesting center frequency %1 Hz after applying %2")
                    .arg(static_cast<qulonglong>(edit.frequency))
                    .arg(limitSource));
        }
        return;
    }

    const auto previousState = m_receiver->state();
    const auto operation = m_receiver->setCenterFrequency(edit.frequency);
    applyOperation(previousState, operation);
    if (!operation.succeeded() || !edit.adjustedToLimit) {
        return;
    }

    const QString limitSource = m_deviceFrequencyRanges.has_value()
                                    ? QStringLiteral("receiver and device limits")
                                    : QStringLiteral("receiver limits");
    QString message = QStringLiteral("Center frequency limited to %1 Hz by %2")
                          .arg(static_cast<qulonglong>(edit.frequency))
                          .arg(limitSource);
    setStatusText(std::move(message));
}

void ApplicationModel::applyExactCenterFrequencyEdit(
    const sdr::app::FrequencyEditResult& edit)
{
    if (rejectManualTuningWhileScanning()) {
        return;
    }
    if (!edit.succeeded()) {
        setStatusText(QString::fromStdString(edit.message));
        return;
    }

    const auto constrained = sdr::app::FrequencyDigitController::constrain(
        edit.frequency, effectiveCenterFrequencyRanges());
    if (!constrained.succeeded() || constrained.adjustedToLimit) {
        setStatusText(QStringLiteral(
            "Center frequency is outside the available receiver and device limits"));
        return;
    }
    applyCenterFrequencyEdit(constrained);
}

void ApplicationModel::clearCenterFrequencyDigitEdit()
{
    if (!m_centerFrequencyDigitEditPending.has_value()) {
        return;
    }
    m_centerFrequencyDigitEditOriginal.reset();
    m_centerFrequencyDigitEditPending.reset();
    m_centerFrequencyDigitEditIndex = -1;
    m_centerFrequencyDigitEditStartIndex = -1;
    emit centerFrequencyDigitsChanged();
    emit centerFrequencyDigitEditChanged();
}

bool ApplicationModel::rejectManualTuningWhileScanning()
{
    if (!scannerOwnsTuning()) {
        return false;
    }
    setStatusText(QStringLiteral(
        "Scanner has exclusive tuning control; stop scanning before tuning manually"));
    return true;
}

void ApplicationModel::cancelPendingManualTuning()
{
    m_pendingTuningWheelAction = PendingTuningWheelAction::None;
    m_pendingWheelTuningShift = 0;
    m_wheelTuningTimer.stop();
    m_pendingListeningWheelFrequency.reset();
    clearCenterWheelPreview(true);
    resetCenterWheelAcceleration();
}

void ApplicationModel::initializeWheelTuningCoalescing()
{
    m_wheelTuningTimer.setInterval(wheelTuningCoalescingMilliseconds);
    m_wheelTuningTimer.setSingleShot(true);
    m_wheelTuningTimer.setTimerType(Qt::PreciseTimer);
    connect(
        &m_wheelTuningTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::flushPendingWheelTuning);
    m_viewportWheelTimer.setInterval(displayWheelCoalescingMilliseconds);
    m_viewportWheelTimer.setSingleShot(true);
    m_viewportWheelTimer.setTimerType(Qt::PreciseTimer);
    connect(
        &m_viewportWheelTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::flushPendingViewportWheelAction);
    m_filterWidthWheelTimer.setInterval(displayWheelCoalescingMilliseconds);
    m_filterWidthWheelTimer.setSingleShot(true);
    m_filterWidthWheelTimer.setTimerType(Qt::PreciseTimer);
    connect(
        &m_filterWidthWheelTimer,
        &QTimer::timeout,
        this,
        &ApplicationModel::flushPendingFilterWidthAdjustment);
}

void ApplicationModel::flushPendingWheelTuning()
{
    const auto action =
        std::exchange(
            m_pendingTuningWheelAction, PendingTuningWheelAction::None);
    const qint64 requestedShift =
        std::exchange(m_pendingWheelTuningShift, 0);
    if (scannerOwnsTuning()) {
        m_pendingListeningWheelFrequency.reset();
        return;
    }
    if (!m_runtime) {
        return;
    }
    if (action == PendingTuningWheelAction::Center) {
        m_pendingListeningWheelFrequency.reset();
        if (m_pendingCenterWheelFrequency.has_value()) {
            m_runtime->setCenterFrequency(*m_pendingCenterWheelFrequency);
        }
        return;
    }
    if (action != PendingTuningWheelAction::Listening || requestedShift == 0) {
        return;
    }

    const auto captureRange = m_frequencyViewport.captureRange();
    const quint64 base =
        m_pendingListeningWheelFrequency.value_or(listeningFrequency());
    const qint64 requested = static_cast<qint64>(base) + requestedShift;
    const quint64 adjusted = static_cast<quint64>(std::clamp(
        requested,
        static_cast<qint64>(captureRange.minimum),
        static_cast<qint64>(captureRange.maximum)));
    if (adjusted == base) {
        return;
    }
    m_pendingListeningWheelFrequency = adjusted;
    m_runtime->setListeningFrequency(adjusted);
}

void ApplicationModel::queueTuningWheelAction(
    PendingTuningWheelAction action, qint64 requestedStep)
{
    if (m_pendingTuningWheelAction != PendingTuningWheelAction::None &&
        m_pendingTuningWheelAction != action) {
        flushPendingWheelTuning();
    }
    if (action == PendingTuningWheelAction::Center) {
        m_pendingListeningWheelFrequency.reset();
        previewCenterWheelShift(requestedStep);
        m_pendingTuningWheelAction = PendingTuningWheelAction::Center;
        m_pendingWheelTuningShift = 0;
        m_wheelTuningTimer.start();
        return;
    }
    if (m_pendingTuningWheelAction != action) {
        m_pendingTuningWheelAction = action;
        m_pendingWheelTuningShift = 0;
    }
    if (requestedStep > 0 &&
        m_pendingWheelTuningShift >
            std::numeric_limits<qint64>::max() - requestedStep) {
        m_pendingWheelTuningShift = std::numeric_limits<qint64>::max();
    } else if (
        requestedStep < 0 &&
        m_pendingWheelTuningShift <
            std::numeric_limits<qint64>::min() - requestedStep) {
        m_pendingWheelTuningShift = std::numeric_limits<qint64>::min();
    } else {
        m_pendingWheelTuningShift += requestedStep;
    }
    if (!m_wheelTuningTimer.isActive()) {
        m_wheelTuningTimer.start();
    }
}

void ApplicationModel::previewCenterWheelShift(qint64 requestedStep)
{
    const quint64 base = m_pendingCenterWheelFrequency.value_or(
        receiverState().centerFrequency);
    const auto edit = sdr::app::FrequencyDigitController::constrain(
        shiftedFrequency(base, requestedStep),
        effectiveCenterFrequencyRanges());
    if (!edit.succeeded()) {
        setStatusText(QString::fromStdString(edit.message));
        return;
    }
    if (edit.frequency == base) {
        return;
    }

    m_pendingCenterWheelFrequency = edit.frequency;
    const bool viewportChanged = m_frequencyViewport.configureCapture(
        edit.frequency,
        effectiveSampleRate(),
        edit.frequency,
        advertisedRfRangeForCenter(edit.frequency));
    emit centerFrequencyChanged();
    emit centerFrequencyDigitsChanged();
    emit listeningFrequencyChanged();
    if (viewportChanged) {
        emitFrequencyViewportChanges();
    } else {
        emit listeningPositionChanged();
        emit filterMarkerChanged();
    }
}

void ApplicationModel::clearCenterWheelPreview(bool restoreConfirmedViewport)
{
    if (!m_pendingCenterWheelFrequency.has_value()) {
        return;
    }
    m_pendingCenterWheelFrequency.reset();
    bool viewportChanged = false;
    if (restoreConfirmedViewport) {
        viewportChanged = m_frequencyViewport.configureCapture(
            receiverState().centerFrequency,
            effectiveSampleRate(),
            receiverState().listeningFrequency,
            advertisedRfRangeForCenter(receiverState().centerFrequency));
    }
    emit centerFrequencyChanged();
    emit centerFrequencyDigitsChanged();
    emit listeningFrequencyChanged();
    if (viewportChanged) {
        emitFrequencyViewportChanges();
    } else {
        emit listeningPositionChanged();
        emit filterMarkerChanged();
    }
}

void ApplicationModel::finishCenterWheelRequest(
    quint64 frequency, bool succeeded)
{
    if (!m_pendingCenterWheelFrequency.has_value() ||
        frequency != *m_pendingCenterWheelFrequency) {
        return;
    }
    if (succeeded) {
        m_pendingCenterWheelFrequency.reset();
        return;
    }
    clearCenterWheelPreview(true);
}

void ApplicationModel::queueViewportWheelAction(
    PendingViewportWheelAction action, int wheelDelta)
{
    if (action == PendingViewportWheelAction::None) {
        return;
    }
    if (m_pendingViewportWheelAction != action) {
        m_pendingViewportWheelAction = action;
        m_pendingViewportWheelDelta = 0;
    }
    accumulateWheelDelta(m_pendingViewportWheelDelta, wheelDelta);
    if (!m_viewportWheelTimer.isActive()) {
        m_viewportWheelTimer.start();
    }
}

void ApplicationModel::discardPendingViewportWheelAction()
{
    m_pendingViewportWheelAction = PendingViewportWheelAction::None;
    m_pendingViewportWheelDelta = 0;
    m_viewportWheelTimer.stop();
}

void ApplicationModel::flushPendingViewportWheelAction()
{
    const auto action =
        std::exchange(
            m_pendingViewportWheelAction, PendingViewportWheelAction::None);
    const int wheelDelta = std::exchange(m_pendingViewportWheelDelta, 0);
    bool changed = false;
    switch (action) {
    case PendingViewportWheelAction::Zoom:
        changed = m_frequencyViewport.zoomBySteps(
            listeningFrequency(),
            static_cast<double>(wheelDelta) / wheelDeltaPerStep);
        break;
    case PendingViewportWheelAction::None:
        break;
    }
    if (changed) {
        emitFrequencyViewportChanges();
    }
}

void ApplicationModel::requestFilterWidthAdjustment(int wheelDelta)
{
    accumulateWheelDelta(m_pendingFilterWidthWheelDelta, wheelDelta);
    if (!m_filterWidthWheelTimer.isActive()) {
        m_filterWidthWheelTimer.start();
    }
}

void ApplicationModel::flushPendingFilterWidthAdjustment()
{
    accumulateWheelDelta(
        m_filterWidthWheelRemainder,
        std::exchange(m_pendingFilterWidthWheelDelta, 0));
    const int steps = m_filterWidthWheelRemainder /
                      static_cast<int>(wheelDeltaPerStep);
    int adjustedSteps = steps;
    if (adjustedSteps == 0 &&
        m_filterWidthWheelRemainder >= minimumDiscreteWheelTriggerDelta) {
        adjustedSteps = 1;
    } else if (
        adjustedSteps == 0 &&
        m_filterWidthWheelRemainder <= -minimumDiscreteWheelTriggerDelta) {
        adjustedSteps = -1;
    }
    if (adjustedSteps == 0) {
        return;
    }
    m_filterWidthWheelRemainder -=
        adjustedSteps * static_cast<int>(wheelDeltaPerStep);
    const auto range = sdr::radio::filterWidthRange(
        receiverState().demodulationMode, receiverState().sampleRate);
    const qint64 requested =
        static_cast<qint64>(filterWidth()) +
        static_cast<qint64>(adjustedSteps) *
            static_cast<qint64>(
                filterWheelStep(receiverState().demodulationMode));
    const quint64 adjusted = static_cast<quint64>(std::clamp(
        requested,
        static_cast<qint64>(range.minimum),
        static_cast<qint64>(range.maximum)));
    if (adjusted != filterWidth()) {
        setFilterWidth(adjusted);
    }
}

void ApplicationModel::emitFrequencyViewportChanges()
{
    emit frequencyViewportChanged();
    emit visibleRangeChanged();
    emit listeningPositionChanged();
    emit filterMarkerChanged();
}

bool ApplicationModel::canPublishSpectrumFrame(
    quint64 magnitudeCount,
    quint64 frameCenterFrequency,
    quint64 frameSampleRate,
    quint64 frameFftSize,
    quint64 frameSequence,
    quint64 frameTimestampNanoseconds) const noexcept
{
    if (magnitudeCount < 2 || frameSequence == 0 ||
        frameFftSize != magnitudeCount ||
        frameFftSize != m_effectiveSpectrumFftSize ||
        frameSampleRate != effectiveSampleRate() ||
        frameSequence <= m_lastSpectrumFrameSequence ||
        (frameTimestampNanoseconds != 0 &&
         m_lastSpectrumFrameTimestampNanoseconds != 0 &&
         frameTimestampNanoseconds <=
             m_lastSpectrumFrameTimestampNanoseconds)) {
        return false;
    }

    const auto frameRange = sdr::radio::visibleCaptureRange(
        frameCenterFrequency,
        frameSampleRate,
        advertisedRfRangeForCenter(frameCenterFrequency));
    if (!frameRange.has_value()) {
        return false;
    }
    const auto visibleRange = m_frequencyViewport.visibleRange();
    return m_frequencyViewport.valid() &&
           frameRange->maximum > visibleRange.minimum &&
           frameRange->minimum < visibleRange.maximum;
}

void ApplicationModel::recordPublishedSpectrumFrame(
    quint64 frameSequence,
    quint64 frameTimestampNanoseconds) noexcept
{
    m_lastSpectrumFrameSequence = frameSequence;
    if (frameTimestampNanoseconds != 0) {
        m_lastSpectrumFrameTimestampNanoseconds =
            frameTimestampNanoseconds;
    }
}

void ApplicationModel::resetSpectrumFrame()
{
    m_lastSpectrumFrameSequence = 0;
    m_lastSpectrumFrameTimestampNanoseconds = 0;
    emit spectrumReset();
}

void ApplicationModel::publishLatestSpectrumFrame()
{
    auto frame = m_receiver->takeLatestSpectrumFrame();
    if (!frame.has_value() || frame->normalizedMagnitudes.empty()) {
        return;
    }
    if (!canPublishSpectrumFrame(
            static_cast<quint64>(frame->normalizedMagnitudes.size()),
            frame->centerFrequency,
            frame->sampleRate,
            static_cast<quint64>(frame->fftSize),
            frame->sequence,
            frame->timestampNanoseconds)) {
        return;
    }

    QVector<float> displayMagnitudes;
    displayMagnitudes.reserve(
        static_cast<qsizetype>(frame->normalizedMagnitudes.size()));
    for (const float magnitude : frame->normalizedMagnitudes) {
        displayMagnitudes.push_back(magnitude);
    }
    recordPublishedSpectrumFrame(
        frame->sequence, frame->timestampNanoseconds);
    emit spectrumFrameReady(
        displayMagnitudes,
        frame->centerFrequency,
        frame->sampleRate,
        static_cast<quint64>(frame->fftSize),
        frame->sequence,
        frame->timestampNanoseconds,
        frame->tuningGeneration);
    emit waterfallFrameReady(
        displayMagnitudes,
        frame->centerFrequency,
        frame->sampleRate,
        static_cast<quint64>(frame->fftSize),
        frame->sequence,
        frame->timestampNanoseconds,
        frame->tuningGeneration);
}

sdr::app::CurrentPassbandScanSettings ApplicationModel::scanSettings() const noexcept
{
    return {
        m_scanLowerFrequency,
        m_scanUpperFrequency,
        m_scanStepSize,
        m_scanDwellMilliseconds,
        m_scanResumeDelayMilliseconds,
    };
}

quint64 ApplicationModel::scanMidpoint() const noexcept
{
    return m_scanLowerFrequency +
           (m_scanUpperFrequency - m_scanLowerFrequency) / 2;
}

std::optional<sdr::radio::FrequencyRange>
ApplicationModel::centeredScanPassband(quint64 centerFrequency) const noexcept
{
    const auto centerRanges = effectiveCenterFrequencyRanges();
    const bool centerSupported = std::any_of(
        centerRanges.cbegin(),
        centerRanges.cend(),
        [centerFrequency](const sdr::radio::FrequencyRange range) {
            return range.contains(centerFrequency);
        });
    if (!centerSupported) {
        return std::nullopt;
    }
    return sdr::radio::visibleCaptureRange(
        centerFrequency,
        effectiveSampleRate(),
        advertisedRfRangeForCenter(centerFrequency));
}

QString ApplicationModel::scanFitValidationError() const
{
    if (const auto validation =
            sdr::app::CurrentPassbandScanner::validateSettings(scanSettings());
        validation.has_value()) {
        return QString::fromStdString(*validation);
    }

    if (m_scanTypeIndex == wideRangeScanTypeIndex) {
        const auto planned = makeWideRangePlan();
        return planned.succeeded() ? QString()
                                   : QString::fromStdString(planned.error);
    }

    const quint64 requiredBandwidth =
        m_scanUpperFrequency - m_scanLowerFrequency;
    const auto centeredPassband = centeredScanPassband(scanMidpoint());
    const quint64 availableBandwidth = centeredPassband.has_value()
                                           ? centeredPassband->maximum -
                                                 centeredPassband->minimum
                                           : 0;
    if (!centeredPassband.has_value() ||
        !centeredPassband->contains(m_scanLowerFrequency) ||
        !centeredPassband->contains(m_scanUpperFrequency)) {
        return QStringLiteral(
                   "Scan range requires %1 Hz; receiver provides %2 Hz of usable capture bandwidth")
            .arg(static_cast<qulonglong>(requiredBandwidth))
            .arg(static_cast<qulonglong>(availableBandwidth));
    }
    return {};
}

sdr::app::ScanFilterOffsets ApplicationModel::scanFilterOffsets() const noexcept
{
    return sdr::app::WideRangeScanPlanner::filterOffsets(
        receiverState().demodulationMode, receiverState().filterWidth);
}

sdr::app::WideRangeCaptureGeometry
ApplicationModel::wideRangeCaptureGeometry() const
{
    const quint64 captureBandwidth = effectiveSampleRate();
    std::vector<sdr::radio::FrequencyRange> tuningRanges =
        m_deviceFrequencyRanges.value_or(
            std::vector<sdr::radio::FrequencyRange>{receiverLimits().frequency});
    return {
        captureBandwidth,
        captureBandwidth / scannerCaptureEdgeGuardDivisor,
        std::move(tuningRanges),
        effectiveCenterFrequencyRanges(),
    };
}

sdr::app::WideRangePlanResult ApplicationModel::makeWideRangePlan() const
{
    return sdr::app::WideRangeScanPlanner::plan(
        scanSettings(), scanFilterOffsets(), wideRangeCaptureGeometry());
}

bool ApplicationModel::wideRangeScanActive() const noexcept
{
    return !m_bookmarkScanSession &&
           m_scanTypeIndex == wideRangeScanTypeIndex && scannerOwnsTuning();
}

bool ApplicationModel::bookmarkScanActive() const noexcept
{
    return m_bookmarkScanSession &&
           m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped;
}

bool ApplicationModel::scannerRetuning() const noexcept
{
    return (m_pendingScanStart.has_value() && m_pendingScanStart->wideRange) ||
           (m_pendingScanStart.has_value() && m_pendingScanStart->bookmark) ||
           m_scanSettlingTimer.isActive() ||
           m_pendingWideListeningFrequency.has_value() ||
           m_pendingBookmarkListeningFrequency.has_value() ||
           m_bookmarkScannerAwaitingSettle;
}

std::optional<sdr::app::BookmarkSnapshot>
ApplicationModel::bookmarkScanEntry() const
{
    if (!bookmarkScanActive()) {
        return std::nullopt;
    }
    const quint64 index = m_scanner.currentFrequency();
    if (index >= m_bookmarkScanBookmarks.size()) {
        return std::nullopt;
    }
    return m_bookmarkScanBookmarks.at(static_cast<std::size_t>(index));
}

QString ApplicationModel::bookmarkScanValidationError(
    const std::vector<sdr::app::BookmarkSnapshot>& bookmarks) const
{
    if (bookmarks.empty()) {
        return QStringLiteral("Select at least one bookmark for scanning");
    }
    for (const auto& entry : bookmarks) {
        const auto mode = sdr::radio::DemodulatorRegistry::resolve(
            entry.bookmark.demodulatorId.toStdString());
        if (!mode) {
            return QStringLiteral("Bookmark “%1” uses an unavailable demodulator")
                .arg(entry.bookmark.name);
        }
        const auto width = bookmarkFilterWidth(
            *mode, entry.bookmark.filterLowHz, entry.bookmark.filterHighHz);
        if (!width) {
            return QStringLiteral("Bookmark “%1” has invalid filter edges")
                .arg(entry.bookmark.name);
        }
        const auto geometry = wideRangeCaptureGeometry();
        const auto oneBookmarkPlan = sdr::app::WideRangeScanPlanner::plan(
            {entry.bookmark.listeningFrequency, entry.bookmark.listeningFrequency,
             1, m_bookmarkScanDwellMilliseconds,
             m_bookmarkScanResumeDelayMilliseconds},
            sdr::app::WideRangeScanPlanner::filterOffsets(*mode, *width), geometry);
        if (!oneBookmarkPlan.succeeded()) {
            return QStringLiteral("Bookmark “%1” is unsupported: %2")
                .arg(entry.bookmark.name,
                     QString::fromStdString(oneBookmarkPlan.error));
        }
    }
    return {};
}

bool ApplicationModel::refreshWideRangePlan()
{
    const auto planned = makeWideRangePlan();
    if (!planned.succeeded()) {
        m_wideRangePlan.reset();
        m_scanValidationError = QString::fromStdString(planned.error);
        return false;
    }
    m_wideRangePlan = *planned.plan;
    if (m_scanner.state() !=
        sdr::app::CurrentPassbandScanState::Stopped) {
        const auto frequencyIndex =
            sdr::app::WideRangeScanPlanner::frequencyIndex(
                *m_wideRangePlan, m_scanner.currentFrequency());
        const auto blockIndex = frequencyIndex.has_value()
                                    ? sdr::app::WideRangeScanPlanner::blockIndex(
                                          *m_wideRangePlan, *frequencyIndex)
                                    : std::nullopt;
        if (blockIndex.has_value()) {
            m_wideRangeBlockIndex = *blockIndex;
        }
    }
    m_wideRangePlanDirty = false;
    return true;
}

ApplicationModel::WideTuneResult ApplicationModel::tuneWideScannerTo(
    quint64 frequency)
{
    if (m_wideRangePlanDirty || !m_wideRangePlan.has_value()) {
        if (!refreshWideRangePlan()) {
            stopScanner(QStringLiteral("Scanner stopped: %1").arg(
                m_scanValidationError));
            return WideTuneResult::Failed;
        }
    }
    const auto frequencyIndex =
        sdr::app::WideRangeScanPlanner::frequencyIndex(
            *m_wideRangePlan, frequency);
    const auto blockIndex = frequencyIndex.has_value()
                                ? sdr::app::WideRangeScanPlanner::blockIndex(
                                      *m_wideRangePlan, *frequencyIndex)
                                : std::nullopt;
    if (!frequencyIndex.has_value() || !blockIndex.has_value()) {
        stopScanner(QStringLiteral(
            "Scanner stopped: current frequency is not in the wide-range plan"));
        return WideTuneResult::Failed;
    }
    if (sdr::app::WideRangeScanPlanner::frequencyFits(
            centerFrequency(),
            frequency,
            scanFilterOffsets(),
            wideRangeCaptureGeometry())) {
        m_wideRangeBlockIndex = *blockIndex;
        tuneScannerTo(frequency);
        return m_scanner.state() ==
                       sdr::app::CurrentPassbandScanState::Stopped
                   ? WideTuneResult::Failed
                   : WideTuneResult::Tuned;
    }
    requestScannerCenter(
        m_wideRangePlan->blocks[*blockIndex].centerFrequency,
        frequency,
        *blockIndex,
        false);
    return WideTuneResult::Retuning;
}

ApplicationModel::WideTuneResult ApplicationModel::tuneBookmarkScannerTo(
    std::size_t index)
{
    if (index >= m_bookmarkScanBookmarks.size()) {
        stopScanner(QStringLiteral("Scanner stopped: bookmark snapshot is invalid"));
        return WideTuneResult::Failed;
    }
    notifyScanCurrentFrequencyChanged();
    const auto& entry = m_bookmarkScanBookmarks.at(index).bookmark;
    const auto mode = sdr::radio::DemodulatorRegistry::resolve(
        entry.demodulatorId.toStdString());
    const auto width = mode ? bookmarkFilterWidth(
                                  *mode, entry.filterLowHz, entry.filterHighHz)
                            : std::nullopt;
    if (!mode || !width) {
        stopScanner(QStringLiteral("Scanner stopped: current bookmark is invalid"));
        return WideTuneResult::Failed;
    }
    const auto offsets = sdr::app::WideRangeScanPlanner::filterOffsets(
        *mode, *width);
    if (sdr::app::WideRangeScanPlanner::frequencyFits(
            centerFrequency(), entry.listeningFrequency, offsets,
            wideRangeCaptureGeometry())) {
        applyBookmarkScannerEntry(index);
        return WideTuneResult::Retuning;
    }
    const auto plan = sdr::app::WideRangeScanPlanner::plan(
        {entry.listeningFrequency, entry.listeningFrequency, 1,
         m_bookmarkScanDwellMilliseconds,
         m_bookmarkScanResumeDelayMilliseconds},
        offsets, wideRangeCaptureGeometry());
    if (!plan.succeeded() || plan.plan->blocks.empty()) {
        stopScanner(QStringLiteral("Scanner stopped: bookmark cannot fit receiver capture bandwidth"));
        return WideTuneResult::Failed;
    }
    requestBookmarkScannerCenter(
        plan.plan->blocks.front().centerFrequency, index, false);
    return WideTuneResult::Retuning;
}

void ApplicationModel::requestBookmarkScannerCenter(
    quint64 centerFrequency, std::size_t bookmarkIndex, bool starting)
{
    m_scanDwellTimer.stop();
    m_scanResumeTimer.stop();
    if (m_runtime) {
        m_runtime->cancelScannerListeningFrequencyRequests();
    }
    m_pendingScanStart = PendingScanStart{
        centerFrequency, listeningFrequency(),
        static_cast<quint64>(bookmarkIndex), bookmarkIndex,
        false, true, starting};
    m_scanStatus = QStringLiteral("Retuning hardware for bookmark %1")
                       .arg(static_cast<qulonglong>(bookmarkIndex + 1));
    emit scannerChanged();
    if (m_runtime) {
        m_runtime->requestScannerCenterFrequency(centerFrequency);
        return;
    }
    const auto previousState = m_receiver->state();
    const auto operation = m_receiver->setCenterFrequency(centerFrequency);
    applyOperation(previousState, operation);
    finishScannerCentering(centerFrequency, operation.succeeded());
}

void ApplicationModel::applyBookmarkScannerEntry(std::size_t index)
{
    if (index >= m_bookmarkScanBookmarks.size()) {
        stopScanner(QStringLiteral("Scanner stopped: bookmark snapshot is invalid"));
        return;
    }
    const auto& bookmark = m_bookmarkScanBookmarks.at(index).bookmark;
    const auto mode = sdr::radio::DemodulatorRegistry::resolve(
        bookmark.demodulatorId.toStdString());
    const auto width = mode ? bookmarkFilterWidth(
                                  *mode, bookmark.filterLowHz, bookmark.filterHighHz)
                            : std::nullopt;
    if (!mode || !width) {
        stopScanner(QStringLiteral("Scanner stopped: current bookmark is invalid"));
        return;
    }
    m_pendingBookmarkListeningFrequency = bookmark.listeningFrequency;
    if (m_runtime) {
        m_runtime->setGain(bookmark.requestedGainDb);
        m_runtime->setDemodulationMode(static_cast<int>(*mode));
        m_runtime->setFilterWidth(*width);
        m_runtime->setSquelchLevel(bookmark.squelchThresholdDb);
        if (!bookmark.squelchEnabled) {
            m_runtime->disableSquelch();
        }
        tuneScannerTo(bookmark.listeningFrequency);
        return;
    }
    const auto previousState = m_receiver->state();
    const auto apply = [this](sdr::radio::OperationResult result) {
        if (result.succeeded()) return true;
        stopScanner(QStringLiteral("Scanner stopped: %1")
                        .arg(QString::fromStdString(result.message)));
        return false;
    };
    if (!apply(m_receiver->setGain(bookmark.requestedGainDb)) ||
        !apply(m_receiver->setDemodulationMode(*mode)) ||
        !apply(m_receiver->setFilterWidth(*width)) ||
        !apply(m_receiver->setSquelchLevel(bookmark.squelchThresholdDb)) ||
        (!bookmark.squelchEnabled && !apply(m_receiver->disableSquelch()))) {
        notifyStateChanges(previousState, m_receiver->state(), false);
        return;
    }
    m_requestedGainDb = bookmark.requestedGainDb;
    notifyStateChanges(previousState, m_receiver->state(), true);
    tuneScannerTo(bookmark.listeningFrequency);
    if (m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped) {
        m_pendingBookmarkListeningFrequency.reset();
        m_bookmarkScannerAwaitingSettle = true;
        m_scanStatus = QStringLiteral("Applying bookmark settings");
        m_scanSettlingTimer.start(scannerTunerSettlingMilliseconds);
        emit scannerChanged();
    }
}

void ApplicationModel::finishBookmarkScannerEntry()
{
    m_bookmarkScannerAwaitingSettle = false;
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Paused) {
        m_scanStatus = QStringLiteral("Scanner paused");
    } else if (scannerSquelchOpen()) {
        m_scanner.setSquelchOpen(true);
        m_scanStatus = QStringLiteral("Holding on squelch activity");
    } else {
        m_scanStatus = QStringLiteral("Scanning bookmarks");
        scheduleScanDwell();
    }
    emit scannerChanged();
}

void ApplicationModel::requestScannerCenter(
    quint64 center,
    quint64 targetFrequency,
    std::size_t blockIndex,
    bool starting)
{
    m_scanDwellTimer.stop();
    m_scanResumeTimer.stop();
    if (m_runtime) {
        m_runtime->cancelScannerListeningFrequencyRequests();
    }
    m_pendingScanStart = PendingScanStart{
        center,
        listeningFrequency(),
        targetFrequency,
        blockIndex,
        true,
        false,
        starting,
    };
    m_scanStatus = QStringLiteral("Retuning hardware for capture block %1")
                       .arg(static_cast<qulonglong>(blockIndex + 1));
    emit scannerChanged();
    if (m_runtime) {
        m_runtime->requestScannerCenterFrequency(center);
        return;
    }
    const auto previousState = m_receiver->state();
    const auto operation = m_receiver->setCenterFrequency(center);
    applyOperation(previousState, operation);
    finishScannerCentering(center, operation.succeeded());
}

bool ApplicationModel::scannerSquelchOpen() const noexcept
{
    if (squelchDisabled()) {
        return false;
    }
    return m_runtime ? m_runtimeSquelchOpen : m_receiver->squelchOpen();
}

void ApplicationModel::resetScanBoundsToCaptureRange()
{
    const auto passband = m_frequencyViewport.captureRange();
    m_scanLowerFrequency = passband.minimum;
    m_scanUpperFrequency = passband.maximum;
    m_scanBoundsFollowCapture = true;
    m_wideRangePlanDirty = true;
    static_cast<void>(updateScanValidation());
    emit scannerChanged();
}

bool ApplicationModel::updateScanValidation()
{
    const QString error = scanFitValidationError();
    if (m_scanValidationError == error) {
        return false;
    }
    m_scanValidationError = error;
    return true;
}

void ApplicationModel::validateActiveScanRange()
{
    if (m_scanBoundsFollowCapture &&
        m_scanner.state() == sdr::app::CurrentPassbandScanState::Stopped) {
        resetScanBoundsToCaptureRange();
        return;
    }
    const bool scannerActive =
        m_scanner.state() != sdr::app::CurrentPassbandScanState::Stopped;
    if (scannerActive && m_bookmarkScanSession) {
        if (scannerRetuning()) {
            return;
        }
        const QString error = bookmarkScanValidationError(m_bookmarkScanBookmarks);
        if (!error.isEmpty()) {
            stopScanner(QStringLiteral("Scanner stopped: %1").arg(error));
            return;
        }
        return;
    }
    if (scannerActive && m_scanTypeIndex == wideRangeScanTypeIndex) {
        if (scannerRetuning()) {
            return;
        }
        if (!refreshWideRangePlan()) {
            stopScanner(QStringLiteral("Scanner stopped: %1").arg(
                m_scanValidationError));
            return;
        }
        if (!sdr::app::WideRangeScanPlanner::frequencyFits(
                centerFrequency(),
                m_scanner.currentFrequency(),
                scanFilterOffsets(),
                wideRangeCaptureGeometry())) {
            static_cast<void>(tuneWideScannerTo(m_scanner.currentFrequency()));
            return;
        }
        if (!m_scanValidationError.isEmpty()) {
            m_scanValidationError.clear();
            emit scannerChanged();
        }
        return;
    }
    QString activeError;
    if (scannerActive) {
        const auto validation = sdr::app::CurrentPassbandScanner::validate(
            scanSettings(), m_frequencyViewport.captureRange());
        activeError = validation.has_value()
                          ? QString::fromStdString(*validation)
                          : QString();
    }
    const QString error = scannerActive ? activeError : scanFitValidationError();
    const bool validationChanged = m_scanValidationError != error;
    m_scanValidationError = error;
    if (scannerActive && !m_scanValidationError.isEmpty()) {
        stopScanner(
            QStringLiteral("Scanner stopped: scan range is outside the current usable capture passband"));
    } else if (validationChanged) {
        emit scannerChanged();
    }
}

void ApplicationModel::updateScannerSquelchActivity()
{
    if (scannerRetuning()) {
        return;
    }
    const bool open = scannerSquelchOpen();
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Running && open) {
        m_scanDwellTimer.stop();
        if (m_runtime) {
            m_runtime->cancelScannerListeningFrequencyRequests();
        }
        m_scanner.setSquelchOpen(true);
        m_scanStatus = QStringLiteral("Holding on squelch activity");
        emit scannerChanged();
        return;
    }
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Holding) {
        if (open) {
            m_scanResumeTimer.stop();
            return;
        }
        if (!m_scanResumeTimer.isActive()) {
            m_scanStatus = QStringLiteral("Squelch closed; waiting to resume");
            scheduleScanResumeDelay();
            emit scannerChanged();
        }
    }
}

void ApplicationModel::scheduleScanDwell()
{
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Running) {
        m_scanDwellTimer.start(activeScanDwellMilliseconds());
    }
}

void ApplicationModel::scheduleScanResumeDelay()
{
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Holding) {
        m_scanResumeTimer.start(activeScanResumeDelayMilliseconds());
    }
}

int ApplicationModel::activeScanDwellMilliseconds() const noexcept
{
    return m_bookmarkScanSession ? m_bookmarkScanDwellMilliseconds
                                 : m_scanDwellMilliseconds;
}

int ApplicationModel::activeScanResumeDelayMilliseconds() const noexcept
{
    return m_bookmarkScanSession ? m_bookmarkScanResumeDelayMilliseconds
                                 : m_scanResumeDelayMilliseconds;
}

void ApplicationModel::tuneScannerTo(quint64 frequency)
{
    notifyScanCurrentFrequencyChanged();
    if (m_runtime) {
        m_runtime->requestScannerListeningFrequency(frequency);
        return;
    }
    const auto previousState = m_receiver->state();
    const auto result = m_receiver->setListeningFrequency(frequency);
    if (!result.succeeded()) {
        setStatusText(QString::fromStdString(result.message));
        stopScanner(
            QStringLiteral("Scanner stopped: %1")
                .arg(QString::fromStdString(result.message)));
        return;
    }
    notifyStateChanges(previousState, m_receiver->state(), true);
}

void ApplicationModel::notifyScanCurrentFrequencyChanged()
{
    if (m_bookmarkScanSession) {
        emit bookmarkScannerChanged();
        return;
    }
    const quint64 frequency = m_scanner.currentFrequency();
    if (m_lastNotifiedScanCurrentFrequency == frequency) {
        return;
    }
    m_lastNotifiedScanCurrentFrequency = frequency;
    emit scanCurrentFrequencyChanged();
}

void ApplicationModel::scannerDwellElapsed()
{
    updateScannerSquelchActivity();
    const auto frequency = m_scanner.advanceAfterDwell();
    if (!frequency.has_value()) {
        return;
    }
    notifyScanCurrentFrequencyChanged();
    const WideTuneResult tuneResult =
        m_bookmarkScanSession
            ? tuneBookmarkScannerTo(static_cast<std::size_t>(*frequency))
            : (m_scanTypeIndex == wideRangeScanTypeIndex
                   ? tuneWideScannerTo(*frequency)
                   : (tuneScannerTo(*frequency), WideTuneResult::Tuned));
    if (tuneResult == WideTuneResult::Tuned) {
        scheduleScanDwell();
    }
}

void ApplicationModel::scannerResumeDelayElapsed()
{
    const auto frequency = m_scanner.advanceAfterResumeDelay(scannerSquelchOpen());
    if (!frequency.has_value()) {
        updateScannerSquelchActivity();
        return;
    }
    notifyScanCurrentFrequencyChanged();
    const WideTuneResult tuneResult =
        m_bookmarkScanSession
            ? tuneBookmarkScannerTo(static_cast<std::size_t>(*frequency))
            : (m_scanTypeIndex == wideRangeScanTypeIndex
                   ? tuneWideScannerTo(*frequency)
                   : (tuneScannerTo(*frequency), WideTuneResult::Tuned));
    if (tuneResult != WideTuneResult::Tuned) {
        return;
    }
    m_scanStatus = m_bookmarkScanSession
                       ? QStringLiteral("Scanning bookmarks")
                       : (m_scanTypeIndex == wideRangeScanTypeIndex
                              ? QStringLiteral("Scanning wide range")
                              : QStringLiteral("Scanning current capture passband"));
    scheduleScanDwell();
    emit scannerChanged();
}

void ApplicationModel::finishScannerCentering(quint64 frequency, bool succeeded)
{
    if (!m_pendingScanStart.has_value() ||
        frequency != m_pendingScanStart->centerFrequency) {
        return;
    }
    const PendingScanStart pending = *m_pendingScanStart;
    if ((pending.wideRange || pending.bookmark) && m_stopScanAfterRetune) {
        const QString status = succeeded
                                   ? QStringLiteral("Scanner stopped")
                                   : QStringLiteral(
                                         "Scanner stopped after hardware retune failed");
        stopScanner(status);
        return;
    }
    if (!succeeded) {
        const QString status = pending.starting
                                   ? QStringLiteral(
                                         "Scanner not started: receiver center-frequency tuning failed")
                                   : QStringLiteral(
                                         "Scanner stopped: receiver center-frequency tuning failed");
        if (pending.bookmark) stopScanner(status);
        else {
            m_pendingScanStart.reset();
            if (pending.wideRange) m_scanner.stop();
            m_scanStatus = status;
            emit scannerChanged();
        }
        return;
    }
    if (!receiverRunning()) {
        const QString status = QStringLiteral(
            "Scanner not started: reception stopped while centering");
        if (pending.bookmark) stopScanner(status);
        else {
            m_pendingScanStart.reset();
            if (pending.wideRange) m_scanner.stop();
            m_scanStatus = status;
            emit scannerChanged();
        }
        return;
    }
    if (centerFrequency() != pending.centerFrequency) {
        const QString status = QStringLiteral(
            "Scanner not started: receiver did not apply the requested midpoint");
        if (pending.bookmark) stopScanner(status);
        else {
            m_pendingScanStart.reset();
            if (pending.wideRange) m_scanner.stop();
            m_scanStatus = status;
            emit scannerChanged();
        }
        return;
    }
    if (pending.wideRange) {
        m_wideRangeBlockIndex = pending.blockIndex;
        m_scanStatus = QStringLiteral("Retuning: waiting for tuner settling");
        m_scanSettlingTimer.start(scannerTunerSettlingMilliseconds);
        emit scannerChanged();
        return;
    }
    if (pending.bookmark) {
        m_scanStatus = QStringLiteral("Retuning: waiting for tuner settling");
        m_scanSettlingTimer.start(scannerTunerSettlingMilliseconds);
        emit scannerChanged();
        return;
    }
    m_pendingScanStart.reset();
    m_stopScanAfterRetune = false;
    beginScannerAfterCentering(pending.previousListeningFrequency);
}

void ApplicationModel::scannerSettlingElapsed()
{
    if (m_bookmarkScannerAwaitingSettle) {
        finishBookmarkScannerEntry();
        return;
    }
    if (!m_pendingScanStart.has_value()) {
        return;
    }
    if (m_pendingScanStart->bookmark) {
        const std::size_t index = m_pendingScanStart->blockIndex;
        m_pendingScanStart.reset();
        applyBookmarkScannerEntry(index);
        return;
    }
    if (!m_pendingScanStart->wideRange) {
        return;
    }
    continueWideScanAfterRetune();
}

void ApplicationModel::continueWideScanAfterRetune()
{
    const PendingScanStart pending = *m_pendingScanStart;
    m_pendingScanStart.reset();
    m_wideRangePlanDirty = true;
    if (!receiverRunning()) {
        stopScanner(QStringLiteral(
            "Scanner stopped: reception stopped while retuning"));
        return;
    }
    if (!refreshWideRangePlan()) {
        stopScanner(QStringLiteral("Scanner stopped: %1").arg(
            m_scanValidationError));
        return;
    }
    if (!sdr::app::WideRangeScanPlanner::frequencyFits(
            centerFrequency(),
            pending.targetFrequency,
            scanFilterOffsets(),
            wideRangeCaptureGeometry())) {
        stopScanner(QStringLiteral(
            "Scanner stopped: active receive filter does not fit after retuning"));
        return;
    }
    m_pendingWideListeningFrequency = pending.targetFrequency;
    tuneScannerTo(pending.targetFrequency);
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Stopped) {
        m_pendingWideListeningFrequency.reset();
        return;
    }
    if (m_runtime) {
        return;
    }
    m_pendingWideListeningFrequency.reset();
    finishWideRangeRetune();
}

void ApplicationModel::finishWideRangeRetune()
{
    if (m_wideRangePlanDirty) {
        if (!refreshWideRangePlan()) {
            stopScanner(QStringLiteral("Scanner stopped: %1").arg(
                m_scanValidationError));
            return;
        }
        if (!sdr::app::WideRangeScanPlanner::frequencyFits(
                centerFrequency(),
                m_scanner.currentFrequency(),
                scanFilterOffsets(),
                wideRangeCaptureGeometry())) {
            static_cast<void>(tuneWideScannerTo(m_scanner.currentFrequency()));
            return;
        }
    }
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Paused) {
        m_scanStatus = QStringLiteral("Scanner paused");
    } else if (scannerSquelchOpen()) {
        m_scanner.setSquelchOpen(true);
        m_scanStatus = QStringLiteral("Holding on squelch activity");
    } else {
        m_scanStatus = QStringLiteral("Scanning wide range");
        scheduleScanDwell();
    }
    emit scannerChanged();
}

void ApplicationModel::beginScannerAfterCentering(
    quint64 previousListeningFrequency)
{
    if (!m_scanner.start(
            scanSettings(),
            m_frequencyViewport.captureRange(),
            previousListeningFrequency,
            scannerSquelchOpen())) {
        static_cast<void>(updateScanValidation());
        m_scanStatus = QStringLiteral("Scanner not started: invalid centered range");
        emit scannerChanged();
        return;
    }
    tuneScannerTo(m_scanner.currentFrequency());
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Stopped) {
        return;
    }
    if (m_scanner.state() == sdr::app::CurrentPassbandScanState::Holding) {
        m_scanStatus = QStringLiteral("Holding on squelch activity");
    } else {
        m_scanStatus = QStringLiteral("Scanning current capture passband");
        scheduleScanDwell();
    }
    emit scannerChanged();
}

void ApplicationModel::stopScanner(const QString& status)
{
    const bool stoppedBookmarkScanner = m_bookmarkScanSession;
    m_scanDwellTimer.stop();
    m_scanResumeTimer.stop();
    m_scanSettlingTimer.stop();
    if (m_runtime) {
        m_runtime->cancelScannerListeningFrequencyRequests();
    }
    m_pendingScanStart.reset();
    m_stopScanAfterRetune = false;
    m_pendingWideListeningFrequency.reset();
    m_pendingBookmarkListeningFrequency.reset();
    m_bookmarkScannerAwaitingSettle = false;
    m_bookmarkScanBookmarks.clear();
    m_bookmarkScanSession = false;
    m_wideRangePlan.reset();
    m_wideRangePlanDirty = false;
    m_scanner.stop();
    if (stoppedBookmarkScanner) {
        m_bookmarkScanStatus = status;
        m_scanStatus = m_scanStatusBeforeBookmarkScan;
        m_scanStatusBeforeBookmarkScan.clear();
    } else {
        m_scanStatus = status;
    }
    emit scannerChanged();
}

void ApplicationModel::setStatusText(QString statusText)
{
    if (m_statusText == statusText) {
        return;
    }

    m_statusText = std::move(statusText);
    emit statusTextChanged();
}
