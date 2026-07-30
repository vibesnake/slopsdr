// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ReceiverRuntime.hpp"

#include "MockReceiverBackend.hpp"
#include "DemodulatorRegistry.hpp"
#include "PpmCalibration.hpp"
#include "ReceiverControlSettings.hpp"
#include "RtlSdrCapabilities.hpp"
#include "SpectrumFramePacing.hpp"
#include "WaterfallFrameDelivery.hpp"

#include <QLocale>
#include <QElapsedTimer>
#include <QDebug>
#include <QMetaObject>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <utility>

namespace sdr::app {
namespace {

constexpr int runtimePollIntervalMilliseconds = 33;
constexpr int audioServiceIntervalMilliseconds = 5;
constexpr int centerFrequencyCommandCoalescingMilliseconds = 1;
constexpr std::size_t minimumWaterfallQueueCapacity = 64;
constexpr double waterfallLiveRowsPerSecond = 12.5;
constexpr int audioDeviceRefreshIntervalMilliseconds = 5'000;
constexpr std::size_t maximumAudioTransferFrames =
    static_cast<std::size_t>(radio::receiverAudioSampleRate / 50);
constexpr quint64 conservativeCaptureBandwidth = 2'000'000;
constexpr auto captureBandwidthSettingsKey = "receiver/captureBandwidthSamplesPerSecond";
constexpr auto spectrumFftSizeSettingsKey = "spectrum/fftSize";
constexpr auto obsoleteWaterfallRowsPerSecondSettingsKey =
    "waterfall/rowsPerSecond";
constexpr auto visibleWaterfallHistorySettingsKey = "waterfall/visibleHistorySeconds";
constexpr double defaultVisibleWaterfallHistorySeconds = 10.0;
constexpr double maximumVisibleWaterfallHistorySeconds = 300.0;
constexpr auto ppmCorrectionsByDeviceSettingsPrefix =
    "receiver/ppmByDevice/";
constexpr std::size_t ppmCalibrationReadBufferBytes = 1'048'576;
constexpr int ppmCalibrationReadTimeoutMilliseconds = 50;
constexpr int autoSquelchMeasurementWindowMilliseconds = 400;

bool supportedSpectrumFftSize(std::size_t fftSize) noexcept
{
    return dsp::isSupportedSpectrumFftSize(fftSize);
}

QString formatCaptureBandwidth(quint64 sampleRate)
{
    return QStringLiteral("%1 MS/s").arg(
        static_cast<double>(sampleRate) / 1'000'000.0, 0, 'f', 3);
}

double nearestSupportedGain(
    const devices::DeviceCapabilities& capabilities, double requestedGain)
{
    double gain = std::clamp(
        requestedGain, capabilities.minimumGainDb, capabilities.maximumGainDb);
    if (capabilities.gainStepDb > 0.0) {
        const double steps = std::round(
            (gain - capabilities.minimumGainDb) / capabilities.gainStepDb);
        gain = capabilities.minimumGainDb + steps * capabilities.gainStepDb;
        gain = std::clamp(
            gain, capabilities.minimumGainDb, capabilities.maximumGainDb);
    }
    return gain;
}

quint64 safeCaptureBandwidth(
    const devices::DeviceCapabilities& capabilities, quint64 preferred)
{
    if (devices::supportsReceiveSampleRate(capabilities, preferred)) {
        return preferred;
    }
    if (devices::supportsReceiveSampleRate(capabilities, conservativeCaptureBandwidth)) {
        return conservativeCaptureBandwidth;
    }

    std::optional<quint64> safeRate;
    for (const auto& range : capabilities.receiveSampleRateRanges) {
        const quint64 candidate = range.minimum;
        if (!safeRate.has_value() || candidate < *safeRate) {
            safeRate = candidate;
        }
    }
    return safeRate.value_or(conservativeCaptureBandwidth);
}

QStringList captureBandwidthOptions(const devices::DeviceCapabilities& capabilities)
{
    if (capabilities.receiveSampleRateRanges.empty()) {
        return {};
    }
    constexpr std::array<quint64, 6> commonRates{
        250'000, 1'000'000, 1'200'000, 2'000'000, 2'250'000, 2'400'000,
    };
    std::vector<quint64> rates;
    for (const auto rate : commonRates) {
        if (devices::supportsReceiveSampleRate(capabilities, rate)) {
            rates.push_back(rate);
        }
    }
    for (const auto& range : capabilities.receiveSampleRateRanges) {
        if (range.minimum == range.maximum) {
            rates.push_back(range.minimum);
        }
    }
    std::ranges::sort(rates);
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    QStringList options;
    for (const auto rate : rates) {
        options.append(formatCaptureBandwidth(rate));
    }
    if (devices::allowsCustomReceiveSampleRate(capabilities)) {
        options.append(QStringLiteral("Custom…"));
    }
    return options;
}

QString formatRanges(const std::vector<radio::FrequencyRange>& ranges)
{
    if (ranges.empty()) {
        return QStringLiteral("not reported");
    }

    QStringList formatted;
    formatted.reserve(static_cast<qsizetype>(ranges.size()));
    for (const auto& range : ranges) {
        formatted.append(
            QStringLiteral("%1–%2")
                .arg(QLocale().toString(range.minimum))
                .arg(QLocale().toString(range.maximum)));
    }
    return formatted.join(QStringLiteral(", "));
}

QString capabilitySummary(const devices::DeviceDescriptor& device)
{
    const auto& capabilities = device.capabilities;
    QStringList parts{
        QStringLiteral("Driver: %1").arg(
            QString::fromStdString(
                device.driver.empty() ? std::string("unknown") : device.driver)),
        QStringLiteral("Frequency (Hz): %1").arg(
            formatRanges(capabilities.receiveFrequencyRanges)),
        QStringLiteral("Sample rate (Hz): %1").arg(
            formatRanges(capabilities.receiveSampleRateRanges)),
    };
    if (capabilities.gainSupported) {
        parts.append(
            QStringLiteral("Gain: %1 to %2 dB")
                .arg(capabilities.minimumGainDb, 0, 'f', 1)
                .arg(capabilities.maximumGainDb, 0, 'f', 1));
    } else {
        parts.append(QStringLiteral("Gain: not adjustable"));
    }
    parts.append(
        capabilities.ppmCorrectionSupported
            ? QStringLiteral("PPM correction: supported")
            : QStringLiteral("PPM correction: unsupported"));
    parts.append(
        capabilities.rtlSdrTestModeSupported
            ? QStringLiteral("RTL-SDR test mode: supported")
            : QStringLiteral("RTL-SDR test mode: unsupported"));
    if (capabilities.rtlSdrBlogV4) {
        parts.append(
            capabilities.driverManagedHfBelow27Mhz
                ? QStringLiteral("RTL-SDR Blog V4 HF: driver managed")
                : QStringLiteral("RTL-SDR Blog V4 HF: unavailable"));
    }
    return parts.join(QStringLiteral(" · "));
}

QString stablePpmDeviceIdentity(const devices::DeviceDescriptor& device)
{
    if (device.identifierIsStable && !device.identifier.empty()) {
        return QString::fromStdString(device.identifier);
    }
    if (!device.driver.empty() && !device.serial.empty()) {
        return QStringLiteral("%1:serial=%2")
            .arg(
                QString::fromStdString(device.driver),
                QString::fromStdString(device.serial));
    }
    return {};
}

QString ppmCorrectionSettingsKey(const devices::DeviceDescriptor& device)
{
    const QString identity = stablePpmDeviceIdentity(device);
    if (identity.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(ppmCorrectionsByDeviceSettingsPrefix) +
           QString::fromLatin1(identity.toUtf8().toHex());
}

std::optional<double> savedPpmCorrection(
    QSettings& settings, const devices::DeviceDescriptor& device)
{
    const QString identity = stablePpmDeviceIdentity(device);
    if (identity.isEmpty()) {
        return std::nullopt;
    }
    const QString key = ppmCorrectionSettingsKey(device);
    if (!settings.contains(key)) {
        return std::nullopt;
    }
    bool valid = false;
    const double ppm = settings.value(key).toDouble(&valid);
    if (!valid || !std::isfinite(ppm) || ppm < -200.0 || ppm > 200.0) {
        return std::nullopt;
    }
    return ppm;
}

bool savePpmCorrection(
    QSettings& settings,
    const devices::DeviceDescriptor& device,
    double ppmCorrection)
{
    const QString identity = stablePpmDeviceIdentity(device);
    if (identity.isEmpty() || !std::isfinite(ppmCorrection)) {
        return false;
    }
    const QString key = ppmCorrectionSettingsKey(device);
    const bool hadPreviousValue = settings.contains(key);
    const QVariant previousValue = settings.value(key);
    settings.setValue(key, ppmCorrection);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (hadPreviousValue) {
            settings.setValue(key, previousValue);
        } else {
            settings.remove(key);
        }
        settings.sync();
        return false;
    }
    bool valid = false;
    const double effective = settings.value(key).toDouble(&valid);
    if (valid && effective == ppmCorrection) {
        return true;
    }
    if (hadPreviousValue) {
        settings.setValue(key, previousValue);
    } else {
        settings.remove(key);
    }
    settings.sync();
    return false;
}

std::uint64_t steadyMonotonicNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

class ReceiverRuntime::Worker final : public QObject
{
    Q_OBJECT

public:
    Worker(
        StartupMode startupMode,
        Factories factories,
        bool verboseAudioMetrics)
        : m_startupMode(startupMode)
        , m_factories(std::move(factories))
        , m_verboseAudioMetrics(verboseAudioMetrics)
        , m_waterfallDelivery(
              std::max(
                  minimumWaterfallQueueCapacity,
                  static_cast<std::size_t>(m_targetSpectrumFramesPerSecond)))
    {
        if (!m_factories.monotonicClock) {
            m_factories.monotonicClock = steadyMonotonicNanoseconds;
        }
    }

    void setApplicationLogHandler(ApplicationLogHandler handler)
    {
        m_applicationLogHandler = std::move(handler);
        if (m_dsdFme) {
            configureDsdFmeLogging();
        }
    }

public slots:
    void initialize()
    {
        if (m_pollTimer) {
            return;
        }
        log(
            1,
            QStringLiteral("Application"),
            QStringLiteral("Receiver runtime initialized"));
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(runtimePollIntervalMilliseconds);
        m_pollTimer->setTimerType(Qt::CoarseTimer);
        connect(m_pollTimer, &QTimer::timeout, this, &Worker::pollBackend);
        m_pollTimer->start();

        m_centerFrequencyTimer = new QTimer(this);
        m_centerFrequencyTimer->setInterval(
            centerFrequencyCommandCoalescingMilliseconds);
        m_centerFrequencyTimer->setSingleShot(true);
        m_centerFrequencyTimer->setTimerType(Qt::PreciseTimer);
        connect(
            m_centerFrequencyTimer,
            &QTimer::timeout,
            this,
            &Worker::applyPendingCenterFrequency);

        m_audioServiceTimer = new QTimer(this);
        m_audioServiceTimer->setInterval(audioServiceIntervalMilliseconds);
        m_audioServiceTimer->setTimerType(Qt::PreciseTimer);
        connect(
            m_audioServiceTimer,
            &QTimer::timeout,
            this,
            &Worker::serviceAudio);
        m_audioServiceTimer->start();

        m_audioDeviceTimer = new QTimer(this);
        m_audioDeviceTimer->setInterval(audioDeviceRefreshIntervalMilliseconds);
        m_audioDeviceTimer->setTimerType(Qt::CoarseTimer);
        connect(
            m_audioDeviceTimer,
            &QTimer::timeout,
            this,
            &Worker::refreshAudioDevices);
        m_audioDeviceTimer->start();

        m_waterfallTimer = new QTimer(this);
        m_waterfallTimer->setSingleShot(true);
        m_waterfallTimer->setTimerType(Qt::PreciseTimer);
        connect(
            m_waterfallTimer,
            &QTimer::timeout,
            this,
            &Worker::publishNextWaterfallRow);
        scheduleNextWaterfallTick();

        m_ppmCalibrationTimer = new QTimer(this);
        m_ppmCalibrationTimer->setSingleShot(true);
        m_ppmCalibrationTimer->setTimerType(Qt::PreciseTimer);
        connect(
            m_ppmCalibrationTimer,
            &QTimer::timeout,
            this,
            &Worker::readPpmCalibration);

        if (m_factories.createAudioOutputService) {
            try {
                m_audioOutput = m_factories.createAudioOutputService();
                if (m_audioOutput) {
                    m_audioOutput->refreshDevices();
                }
            } catch (const std::exception& error) {
                m_audioInitializationError = QStringLiteral(
                    "Audio output initialization failed: %1")
                                                 .arg(QString::fromUtf8(error.what()));
            } catch (...) {
                m_audioInitializationError = QStringLiteral(
                    "Audio output initialization failed with an unknown error");
            }
        }
        try {
            m_dsdFme = m_factories.createDsdFmeProcessService
                           ? m_factories.createDsdFmeProcessService()
                           : platform::makeDsdFmeProcessService();
            if (m_dsdFme) {
                m_dsdFme->setDiagnosticsClock(m_factories.monotonicClock);
                m_dsdFme->setDiagnosticsEnabled(m_verboseAudioMetrics);
                configureDsdFmeLogging();
                m_dsdFme->setBinaryPath(
                    QSettings()
                        .value(QStringLiteral(
                            "externalDecoder/dsdFmeBinaryPath"))
                        .toString());
            }
        } catch (const std::exception& error) {
            m_dsdFmeInitializationError = QStringLiteral(
                "DSD-FME initialization failed: %1")
                                              .arg(QString::fromUtf8(
                                                  error.what()));
        } catch (...) {
            m_dsdFmeInitializationError = QStringLiteral(
                "DSD-FME initialization failed with an unknown error");
        }

        QSettings settings;
        bool savedFftSizeValid = false;
        const auto savedFftSize = settings.value(spectrumFftSizeSettingsKey)
                                      .toULongLong(&savedFftSizeValid);
        const std::size_t initialFftSize =
            supportedSpectrumFftSize(m_factories.initialSpectrumFftSize)
                ? m_factories.initialSpectrumFftSize
                : std::size_t{4'096};
        m_spectrumFftSize = savedFftSizeValid &&
                                    supportedSpectrumFftSize(
                                        static_cast<std::size_t>(savedFftSize))
                                ? static_cast<std::size_t>(savedFftSize)
                                : initialFftSize;
        settings.remove(obsoleteWaterfallRowsPerSecondSettingsKey);
        bool savedHistoryValid = false;
        const auto savedHistory = settings.value(visibleWaterfallHistorySettingsKey)
                                      .toDouble(&savedHistoryValid);
        if (savedHistoryValid && std::isfinite(savedHistory) && savedHistory > 0.0 &&
            savedHistory <= maximumVisibleWaterfallHistorySeconds) {
            m_visibleWaterfallHistorySeconds = savedHistory;
        }
        m_waterfallDelivery.setCapacity(std::max(
            minimumWaterfallQueueCapacity,
            static_cast<std::size_t>(m_targetSpectrumFramesPerSecond)));
        bool savedRateValid = false;
        const quint64 savedRate = settings.value(captureBandwidthSettingsKey).toULongLong(
            &savedRateValid);
        if (savedRateValid && savedRate > 0) {
            m_requestedCaptureBandwidth = savedRate;
        }
        m_receiverControls = loadReceiverControlSettings(
            settings, m_requestedCaptureBandwidth);
        m_savedGainDb = m_receiverControls.requestedGainDb;
        m_requestedGainDb = m_savedGainDb.value_or(20.0);

        if (m_startupMode == StartupMode::Mock) {
            m_backend = std::make_unique<radio::MockReceiverBackend>();
            static_cast<void>(m_backend->setSpectrumFftSize(m_spectrumFftSize));
            static_cast<void>(m_backend->setSpectrumFramesPerSecond(
                m_targetSpectrumFramesPerSecond));
            if (m_requestedCaptureBandwidth != m_backend->state().sampleRate) {
                const auto result = m_backend->setSampleRate(m_requestedCaptureBandwidth);
                if (!result.succeeded()) {
                    m_requestedCaptureBandwidth = m_backend->state().sampleRate;
                }
            }
            static_cast<void>(m_backend->setGain(m_requestedGainDb));
            if (!applyRestoredReceiverControls()) {
                publishSnapshot(false);
                return;
            }
            m_statusText = QStringLiteral("Mock backend ready - no hardware device");
            m_deviceState = QStringLiteral("Mock device");
            m_backendDescription = QStringLiteral("Mock backend - no SDR hardware");
        } else if (!hardwareFactoriesAvailable()) {
            m_statusText = QStringLiteral(
                "Hardware support is unavailable in this build; run with --mock");
            m_deviceState = QStringLiteral("Hardware support unavailable");
            m_backendDescription = QStringLiteral("Dependency-disabled development build");
        } else {
            m_statusText = QStringLiteral("Searching for SDR devices…");
            m_deviceState = QStringLiteral("Searching for SDR devices…");
            m_backendDescription = QStringLiteral("GNU Radio + SoapySDR hardware runtime");
        }
        publishSnapshot(true);
        if (m_startupMode == StartupMode::Hardware && hardwareFactoriesAvailable()) {
            refreshDevices();
        }
    }

    void shutdown()
    {
        log(
            1,
            QStringLiteral("Application"),
            QStringLiteral("Receiver runtime shutting down"));
        if (m_pollTimer) {
            m_pollTimer->stop();
        }
        if (m_audioServiceTimer) {
            m_audioServiceTimer->stop();
        }
        if (m_audioDeviceTimer) {
            m_audioDeviceTimer->stop();
        }
        if (m_waterfallTimer) {
            m_waterfallTimer->stop();
        }
        if (m_centerFrequencyTimer) {
            m_centerFrequencyTimer->stop();
        }
        if (m_ppmCalibrationRunning) {
            finishPpmCalibration(
                QStringLiteral("cancelled"),
                QStringLiteral("Application shutdown cancelled calibration"),
                true);
        }
        m_pendingCenterFrequency.reset();
        m_waterfallDelivery.stop();
        if (m_backend && m_backend->state().running) {
            static_cast<void>(m_backend->stopReception());
        }
        if (m_dsdFme) {
            m_dsdFme->stop();
        }
        if (m_audioOutput) {
            m_audioOutput->stop();
        }
        if (m_recording) {
            m_recording->stop();
        }
        m_backend.reset();
        m_audioOutput.reset();
        m_dsdFme.reset();
        m_selectedCapabilities.reset();
    }

    void refreshDevices()
    {
        if (!requireHardwareMode(QStringLiteral("Device discovery"))) {
            return;
        }
        try {
            auto controller = std::make_unique<devices::DeviceController>(
                m_factories.createDeviceProvider());
            const auto result = controller->discover();
            if (!result.succeeded()) {
                m_statusText = QString::fromStdString(result.message);
                publishSnapshot(false);
                return;
            }
            m_devices = controller->devices();
            const auto selected = std::ranges::find_if(
                m_devices,
                [this](const devices::DeviceDescriptor& device) {
                    return device.identifier == m_selectedDeviceIdentifier.toStdString();
                });
            if (!(m_backend && m_backend->state().running)) {
                if (selected != m_devices.end()) {
                    setSelectedDevice(*selected);
                } else if (!m_devices.empty()) {
                    setSelectedDevice(m_devices.front());
                } else {
                    clearStoppedSelection();
                }
            }
            if (m_devices.empty()) {
                m_statusText = QStringLiteral("No SDR devices found");
            } else if (!m_statusText.startsWith(
                           QStringLiteral("Saved capture bandwidth is unsupported"))) {
                m_statusText = QStringLiteral("Found %1 SDR device(s)")
                                   .arg(m_devices.size());
            }
            publishSnapshot(true);
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral("Device discovery failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            m_statusText = QStringLiteral("Device discovery failed with an unknown error");
            publishSnapshot(false);
        }
    }

    void selectDevice(const QString& identifier)
    {
        if (!requireHardwareMode(QStringLiteral("Device selection"))) {
            return;
        }
        if (m_backend && m_backend->state().running) {
            m_statusText = QStringLiteral("Stop reception before selecting another device");
            publishSnapshot(false);
            return;
        }
        const std::string requestedIdentifier = identifier.toStdString();
        if (std::ranges::none_of(
                m_devices,
                [&requestedIdentifier](const devices::DeviceDescriptor& device) {
                    return device.identifier == requestedIdentifier;
                })) {
            m_statusText = QStringLiteral("Refresh devices and select an available SDR");
            publishSnapshot(false);
            return;
        }

        const auto selected = std::ranges::find_if(
            m_devices,
            [&requestedIdentifier](const devices::DeviceDescriptor& device) {
                return device.identifier == requestedIdentifier;
        });
        setSelectedDevice(*selected);
        if (!m_statusText.startsWith(
                QStringLiteral("Saved capture bandwidth is unsupported"))) {
            m_statusText = QStringLiteral(
                "SDR device selected; it will open when Start is pressed");
        }
        publishSnapshot(true);
    }

    void clearDeviceSelection()
    {
        if (!requireHardwareMode(QStringLiteral("Clearing device selection"))) {
            return;
        }
        if (m_backend && m_backend->state().running) {
            m_statusText = QStringLiteral("Stop reception before clearing the device");
            publishSnapshot(false);
            return;
        }
        clearStoppedSelection();
        m_statusText = QStringLiteral("Device selection cleared");
        publishSnapshot(true);
    }

    void startReception()
    {
        if (!m_backend && m_selectedDeviceIdentifier.isEmpty()) {
            m_statusText = QStringLiteral(
                "Select a usable SDR device before starting reception");
            publishSnapshot(false);
            return;
        }
        try {
            QString captureBandwidthNotice;
            if (m_startupMode == StartupMode::Hardware) {
                const QString requestedIdentifier = m_selectedDeviceIdentifier;
                auto controller = std::make_unique<devices::DeviceController>(
                    m_factories.createDeviceProvider());
                const auto discovery = controller->discover();
                if (!discovery.succeeded()) {
                    m_statusText = QStringLiteral("Could not verify the selected SDR: %1")
                                       .arg(QString::fromStdString(discovery.message));
                    publishSnapshot(false);
                    return;
                }
                m_devices = controller->devices();
                const auto selected = std::ranges::find_if(
                    m_devices,
                    [&requestedIdentifier](const devices::DeviceDescriptor& device) {
                        return device.identifier == requestedIdentifier.toStdString();
                    });
                if (selected == m_devices.end()) {
                    if (m_devices.empty()) {
                        clearStoppedSelection();
                    } else {
                        setSelectedDevice(m_devices.front());
                    }
                    m_statusText = QStringLiteral(
                        "The selected SDR disappeared; refreshed the device list. Reception remains stopped");
                    publishSnapshot(false);
                    return;
                }
                const auto selection = controller->selectDevice(requestedIdentifier.toStdString());
                if (!selection.succeeded()) {
                    if (selection.error == devices::DeviceError::DeviceNotFound) {
                        refreshDevices();
                        m_statusText = QStringLiteral(
                            "The selected SDR disappeared while starting; refreshed the device list. Reception remains stopped");
                    } else {
                        m_statusText = QStringLiteral("Opening the selected SDR failed: %1")
                                           .arg(QString::fromStdString(selection.message));
                    }
                    publishSnapshot(false);
                    return;
                }
                const devices::DeviceDescriptor openedDevice =
                    *controller->selectedDevice();
                const quint64 previousCaptureBandwidth = m_requestedCaptureBandwidth;
                setSelectedDevice(openedDevice, true);
                if (m_requestedCaptureBandwidth != previousCaptureBandwidth) {
                    captureBandwidthNotice = m_statusText;
                }
                auto replacement = m_factories.createHardwareBackend(std::move(controller));
                if (!replacement) {
                    m_statusText = QStringLiteral("Failed to create the hardware receiver backend");
                    publishSnapshot(false);
                    return;
                }
                m_backend = std::move(replacement);
                if (openedDevice.capabilities.ppmCorrectionSupported) {
                    const auto ppmResult =
                        m_backend->setPpmCorrection(m_selectedPpmCorrection);
                    if (!ppmResult.succeeded()) {
                        m_backend.reset();
                        m_statusText = QStringLiteral(
                            "Saved PPM correction could not be applied before tuning: %1")
                                           .arg(QString::fromStdString(
                                               ppmResult.message));
                        publishSnapshot(false);
                        return;
                    }
                    m_selectedPpmCorrection =
                        m_backend->state().ppmCorrection;
                }
                if (m_backend->requestedSpectrumFftSize() !=
                    m_spectrumFftSize) {
                    const auto fftResult =
                        m_backend->setSpectrumFftSize(m_spectrumFftSize);
                    if (!fftResult.succeeded()) {
                        m_backend.reset();
                        m_statusText = QString::fromStdString(fftResult.message);
                        publishSnapshot(false);
                        return;
                    }
                }
                if (m_backend->spectrumFramesPerSecond() != 0 &&
                    m_backend->spectrumFramesPerSecond() !=
                    m_targetSpectrumFramesPerSecond) {
                    const auto rateResult = m_backend->setSpectrumFramesPerSecond(
                        m_targetSpectrumFramesPerSecond);
                    if (!rateResult.succeeded()) {
                        m_backend.reset();
                        m_statusText = QString::fromStdString(rateResult.message);
                        publishSnapshot(false);
                        return;
                    }
                }
                if (openedDevice.capabilities.gainSupported) {
                    const double requestedGain = m_requestedGainDb;
                    const double effectiveGain = nearestSupportedGain(
                        openedDevice.capabilities, requestedGain);
                    const auto gainResult = m_backend->setGain(effectiveGain);
                    if (!gainResult.succeeded()) {
                        m_backend.reset();
                        m_statusText = QString::fromStdString(gainResult.message);
                        publishSnapshot(false);
                        return;
                    }
                    if (!m_savedGainDb.has_value() && effectiveGain != 20.0) {
                        m_statusText = QStringLiteral("20 dB gain is unsupported; using %1 dB")
                                           .arg(effectiveGain, 0, 'f', 1);
                    } else if (m_savedGainDb.has_value() &&
                               effectiveGain != requestedGain) {
                        m_statusText = QStringLiteral(
                            "Saved gain %1 dB is unsupported by this SDR; using %2 dB")
                                           .arg(requestedGain, 0, 'f', 1)
                                           .arg(effectiveGain, 0, 'f', 1);
                    }
                }
                const auto captureBandwidthResult =
                    m_backend->setSampleRate(m_requestedCaptureBandwidth);
                if (!captureBandwidthResult.succeeded()) {
                    m_backend.reset();
                    m_statusText = QStringLiteral(
                        "Capture bandwidth could not be applied; reception remains stopped: %1")
                                       .arg(QString::fromStdString(
                                           captureBandwidthResult.message));
                    publishSnapshot(false);
                    return;
                }
                if (!applyRestoredReceiverControls()) {
                    m_backend.reset();
                    publishSnapshot(false);
                    return;
                }
                m_lastBackendAudioDroppedSamples = 0;
                m_backendDescription = QStringLiteral("GNU Radio hardware backend active");
            }
            m_lastBackendAudioDroppedSamples = m_backend->audioDroppedSamples();
            const auto result = m_backend->startReception();
            m_statusText = QString::fromStdString(result.message);
            if (!captureBandwidthNotice.isEmpty()) {
                m_statusText = captureBandwidthNotice + QStringLiteral("; ") + m_statusText;
            }
            if (result.succeeded() && m_backend->state().running && m_audioOutput) {
                m_audioTransferredSamples = 0;
                m_audioServicePasses = 0;
                m_lastMetricsProducedSamples = m_backend->audioProducedSamples();
                m_lastMetricsTransferredSamples = 0;
                m_lastMetricsServicePasses = 0;
                m_lastMetricsWrittenSamples = 0;
                m_audioMetricsTimer.invalidate();
                static_cast<void>(m_audioOutput->start());
            }
            if (result.succeeded() && m_backend->state().running &&
                m_dsdFme &&
                m_backend->state().demodulationMode ==
                    radio::DemodulationMode::DigitalDecoderOutput) {
                m_backend->clearDecoderInputSamples();
                m_lastDecoderInputDroppedSamples =
                    m_backend->decoderInputDroppedSamples();
                m_dsdFme->start();
            }
            if (result.succeeded() && m_backend->state().running) {
                resetWaterfallDelivery();
                log(1, QStringLiteral("SDR"), m_statusText);
            }
            publishSnapshot(result.succeeded());
        } catch (const std::exception& error) {
            m_backend.reset();
            m_statusText = QStringLiteral("Receiver operation failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            m_backend.reset();
            m_statusText = QStringLiteral(
                "Receiver operation failed with an unknown error");
            publishSnapshot(false);
        }
    }

    void stopReception()
    {
        m_autoSquelchRunning = false;
        m_autoSquelchSamples.clear();
        m_waterfallDelivery.stop();
        m_spectrumMetricsTimer.invalidate();
        if (m_recording) {
            m_recording->stop();
        }
        if (!m_backend) {
            if (m_dsdFme) {
                m_dsdFme->stop();
            }
            if (m_audioOutput) {
                m_audioOutput->stop();
            }
            m_statusText = QStringLiteral("No receiver backend is running");
            publishSnapshot(false);
            return;
        }
        try {
            const auto result = m_backend->stopReception();
            if (m_dsdFme) {
                m_dsdFme->stop();
            }
            if (m_audioOutput) {
                m_audioOutput->stop();
            }
            m_backend->clearAudioSamples();
            m_lastBackendAudioDroppedSamples = m_backend->audioDroppedSamples();
            m_statusText = QString::fromStdString(result.message);
            if (result.succeeded()) {
                log(1, QStringLiteral("SDR"), m_statusText);
            }
            if (result.succeeded() && !m_backend->state().running) {
                m_backend.reset();
                m_lastBackendAudioDroppedSamples = 0;
                m_backendDescription = QStringLiteral(
                    "Selected SDR opens when Start is pressed");
            }
            publishSnapshot(result.succeeded());
        } catch (const std::exception& error) {
            if (m_dsdFme) {
                m_dsdFme->stop();
            }
            if (m_audioOutput) {
                m_audioOutput->stop();
            }
            m_statusText = QStringLiteral("Receiver operation failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            if (m_dsdFme) {
                m_dsdFme->stop();
            }
            if (m_audioOutput) {
                m_audioOutput->stop();
            }
            m_statusText = QStringLiteral(
                "Receiver operation failed with an unknown error");
            publishSnapshot(false);
        }
    }

    void selectAudioDevice(const QString& identifier)
    {
        if (!m_audioOutput) {
            publishSnapshot(false);
            return;
        }
        const bool succeeded = m_audioOutput->selectDevice(identifier.toStdString());
        publishSnapshot(succeeded);
    }

    void setAudioVolume(int volumePercent)
    {
        if (!m_audioOutput) {
            publishSnapshot(false);
            return;
        }
        publishSnapshot(m_audioOutput->setVolumePercent(volumePercent));
    }

    void setAudioMuted(bool muted)
    {
        if (!m_audioOutput) {
            publishSnapshot(false);
            return;
        }
        m_audioOutput->setMuted(muted);
        publishSnapshot(true);
    }

    void startAudioRecording(const QString& directory)
    {
        if (!m_backend || !m_backend->state().running || !m_recording) {
            m_statusText = QStringLiteral("Reception must be active before recording");
            publishSnapshot(false);
            return;
        }
        const auto mode = m_backend->state().demodulationMode;
        const auto* descriptor = radio::DemodulatorRegistry::findByMode(mode);
        const bool started = m_recording->start({
            .directory = std::filesystem::path(directory.toStdString()),
            .frequencyHz = m_backend->state().listeningFrequency,
            .modeName = descriptor ? std::string(descriptor->displayName)
                                   : std::string("audio"),
        });
        const auto state = m_recording->state();
        m_statusText = started
                           ? QStringLiteral("Recording %1")
                                 .arg(QString::fromStdString(
                                     state.filePath.filename().string()))
                           : QString::fromStdString(state.statusText);
        log(started ? 1 : 3, QStringLiteral("Recording"), m_statusText);
        publishSnapshot(started);
    }

    void stopAudioRecording()
    {
        if (m_recording) {
            m_recording->stop();
            const auto state = m_recording->state();
            m_statusText = QString::fromStdString(state.statusText);
            log(state.failed ? 3 : 1, QStringLiteral("Recording"), m_statusText);
        }
        publishSnapshot(true);
    }

    void setDsdFmeBinaryPath(const QString& path)
    {
        if (!m_dsdFme) {
            publishSnapshot(false);
            return;
        }
        m_dsdFme->setBinaryPath(path);
        publishSnapshot(true);
    }

    void requestCenterFrequency(quint64 frequency)
    {
        m_pendingCenterFrequency = frequency;
        if (m_centerFrequencyTimer) {
            m_centerFrequencyTimer->start();
        }
    }

    void applyPendingCenterFrequency()
    {
        const auto frequency = std::exchange(
            m_pendingCenterFrequency, std::nullopt);
        if (!frequency.has_value()) {
            return;
        }
        const bool succeeded = applyTuningOperation(
            [frequency = *frequency](radio::ReceiverBackend& backend) {
                return backend.setCenterFrequency(frequency);
            });
        emit centerFrequencyRequestCompleted(*frequency, succeeded);
    }

    void setListeningFrequency(quint64 frequency)
    {
        if (m_pendingCenterFrequency.has_value()) {
            if (m_centerFrequencyTimer) {
                m_centerFrequencyTimer->stop();
            }
            applyPendingCenterFrequency();
        }
        if (!m_backend) {
            m_statusText = QStringLiteral("Select a receiver backend before tuning");
            publishSnapshot(false);
            return;
        }
        const auto result = m_backend->setListeningFrequency(frequency);
        if (result.succeeded()) {
            flushDecoderAfterRetune();
            if (result.stateChanged && m_audioOutput) {
                m_audioOutput->flush();
            }
            persistCurrentReceiverControls();
        }
        m_statusText = QString::fromStdString(result.message);
        publishSnapshot(result.succeeded());
    }

    void setScannerListeningFrequency(quint64 frequency)
    {
        if (!m_backend) {
            emit scannerListeningFrequencyRequestCompleted(
                frequency,
                0,
                false,
                QStringLiteral("Select a receiver backend before scanning"));
            return;
        }
        const auto result = m_backend->setListeningFrequency(frequency);
        emit scannerListeningFrequencyRequestCompleted(
            frequency,
            m_backend->state().listeningFrequency,
            result.succeeded(),
            QString::fromStdString(result.message));
    }

    void setScannerCenterFrequency(quint64 frequency)
    {
        if (!m_backend) {
            emit scannerCenterFrequencyRequestCompleted(
                frequency,
                0,
                0,
                false,
                QStringLiteral("Select a receiver backend before scanning"));
            return;
        }
        try {
            const auto result = m_backend->setCenterFrequency(frequency);
            if (result.succeeded() && result.stateChanged) {
                m_backend->clearAudioSamples();
                flushDecoderAfterRetune();
                if (m_audioOutput) {
                    m_audioOutput->flush();
                }
            }
            emit scannerCenterFrequencyRequestCompleted(
                frequency,
                m_backend->state().centerFrequency,
                m_backend->state().listeningFrequency,
                result.succeeded(),
                QString::fromStdString(result.message));
        } catch (const std::exception& error) {
            emit scannerCenterFrequencyRequestCompleted(
                frequency,
                m_backend->state().centerFrequency,
                m_backend->state().listeningFrequency,
                false,
                QStringLiteral("Receiver operation failed: %1")
                    .arg(QString::fromUtf8(error.what())));
        } catch (...) {
            emit scannerCenterFrequencyRequestCompleted(
                frequency,
                m_backend->state().centerFrequency,
                m_backend->state().listeningFrequency,
                false,
                QStringLiteral(
                    "Receiver operation failed with an unknown error"));
        }
    }

    void shiftCenterFrequency(qint64 requestedStep)
    {
        static_cast<void>(applyTuningOperation(
            [requestedStep](radio::ReceiverBackend& backend) {
                return backend.shiftCenterFrequency(requestedStep);
            }));
    }

    void setSampleRate(quint64 sampleRate)
    {
        if (m_selectedCapabilities &&
            !devices::supportsReceiveSampleRate(
                m_selectedCapabilities->capabilities, sampleRate)) {
            m_statusText = QStringLiteral(
                "The selected SDR does not support the requested capture bandwidth");
            publishSnapshot(false);
            return;
        }
        if (m_backend) {
            m_waterfallDelivery.stop();
            m_spectrumMetricsTimer.invalidate();
            const auto result = m_backend->setSampleRate(sampleRate);
            if (result.succeeded()) {
                flushDecoderAfterRetune();
            }
            if (m_audioOutput) {
                m_audioOutput->flush();
                if (!m_backend->state().running) {
                    m_audioOutput->stop();
                }
            }
            if (!result.succeeded()) {
                if (m_backend->state().running) {
                    static_cast<void>(m_backend->stopReception());
                    if (m_audioOutput) {
                        m_audioOutput->stop();
                    }
                }
                if (m_dsdFme && !m_backend->state().running) {
                    m_dsdFme->stop();
                }
                m_statusText = QString::fromStdString(result.message);
                publishSnapshot(false);
                return;
            }
            if (m_backend->state().running) {
                resetWaterfallDelivery();
            }
        } else if (!m_selectedCapabilities && m_startupMode == StartupMode::Hardware) {
            m_statusText = QStringLiteral(
                "Select an SDR before setting capture bandwidth");
            publishSnapshot(false);
            return;
        }
        m_requestedCaptureBandwidth = sampleRate;
        QSettings().setValue(captureBandwidthSettingsKey, sampleRate);
        if (m_backend) {
            persistCurrentReceiverControls();
        }
        m_statusText = QStringLiteral("Capture bandwidth requested: %1")
                           .arg(formatCaptureBandwidth(sampleRate));
        publishSnapshot(true);
    }

    void setSpectrumFftSize(quint64 fftSize)
    {
        if (!supportedSpectrumFftSize(static_cast<std::size_t>(fftSize))) {
            m_statusText = QStringLiteral("Unsupported spectrum FFT size");
            publishSnapshot(false);
            return;
        }
        if (static_cast<std::size_t>(fftSize) == m_spectrumFftSize) {
            m_statusText = QStringLiteral("Spectrum FFT size is already %1")
                               .arg(fftSize);
            publishSnapshot(true);
            return;
        }

        if (m_backend) {
            m_waterfallDelivery.stop();
            const auto result = m_backend->setSpectrumFftSize(
                static_cast<std::size_t>(fftSize));
            if (!result.succeeded()) {
                m_statusText = QString::fromStdString(result.message);
                if (m_backend->state().running) {
                    resetWaterfallDelivery();
                }
                publishSnapshot(false);
                return;
            }
        }
        m_spectrumFftSize = static_cast<std::size_t>(fftSize);
        QSettings settings;
        settings.setValue(
            spectrumFftSizeSettingsKey, QString::number(fftSize));
        settings.sync();
        const bool preferenceSaved =
            settings.status() == QSettings::NoError &&
            settings.value(spectrumFftSizeSettingsKey).toULongLong() == fftSize;
        if (m_backend && m_backend->state().running) {
            resetWaterfallDelivery();
        }
        const std::size_t effectiveFftSize = m_backend
                                                 ? m_backend->spectrumFftSize()
                                                 : static_cast<std::size_t>(fftSize);
        m_statusText = preferenceSaved
                           ? (effectiveFftSize == fftSize
                                  ? QStringLiteral(
                                        "Spectrum FFT size changed to %1; audio continued uninterrupted")
                                        .arg(fftSize)
                                  : QStringLiteral(
                                        "Spectrum FFT size requested: %1; effective: %2 because the backend could not allocate the requested plan; audio continued uninterrupted")
                                        .arg(fftSize)
                                        .arg(effectiveFftSize))
                           : QStringLiteral(
                                 "Spectrum FFT size requested: %1; effective: %2, but the preference could not be saved (settings status %3)")
                                 .arg(fftSize)
                                 .arg(effectiveFftSize)
                                 .arg(static_cast<int>(settings.status()));
        publishSnapshot(preferenceSaved);
    }

    void setVisibleWaterfallHistorySeconds(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 1.0 ||
            seconds > maximumVisibleWaterfallHistorySeconds) {
            m_statusText = QStringLiteral("Visible waterfall history must be between 1 and %1 seconds")
                               .arg(maximumVisibleWaterfallHistorySeconds);
            publishSnapshot(false);
            return;
        }
        m_visibleWaterfallHistorySeconds = seconds;
        QSettings settings;
        settings.setValue(visibleWaterfallHistorySettingsKey, seconds);
        settings.sync();
        const bool saved = settings.status() == QSettings::NoError &&
                           qFuzzyCompare(
                               settings.value(visibleWaterfallHistorySettingsKey)
                                       .toDouble() + 1.0,
                               seconds + 1.0);
        m_statusText = saved
                           ? QStringLiteral("Visible waterfall history changed to %1 seconds")
                                 .arg(seconds)
                           : QStringLiteral("Visible waterfall history changed, but the preference could not be saved");
        publishSnapshot(saved);
    }

    void setFilterWidth(quint64 filterWidth)
    {
        if (!m_backend) {
            m_statusText = QStringLiteral("Select a receiver backend before setting filter width");
            publishSnapshot(false);
            return;
        }
        const auto result = m_backend->setFilterWidth(filterWidth);
        if (result.succeeded()) {
            m_filterWidths[static_cast<std::size_t>(m_backend->state().demodulationMode)] =
                m_backend->state().filterWidth;
        }
        m_statusText = QString::fromStdString(result.message);
        publishSnapshot(result.succeeded());
    }

    void setGain(double gainDb)
    {
        if (!m_backend) {
            m_statusText = QStringLiteral("Select a receiver backend before setting gain");
            publishSnapshot(false);
            return;
        }
        const auto result = m_backend->setGain(gainDb);
        if (result.succeeded()) {
            m_requestedGainDb = gainDb;
            m_savedGainDb = gainDb;
            QSettings settings;
            settings.setValue(receiverGainSettingsKey, gainDb);
            settings.sync();
        }
        m_statusText = QString::fromStdString(result.message);
        publishSnapshot(result.succeeded());
    }

    void setPpmCorrection(double ppmCorrection)
    {
        if (!m_backend) {
            m_statusText = QStringLiteral(
                "Select a receiver backend before setting PPM correction");
            publishSnapshot(false);
            return;
        }
        if (!m_backend->capabilities().ppmCorrectionSupported ||
            !std::isfinite(ppmCorrection)) {
            m_statusText = QStringLiteral(
                "PPM correction is unsupported or invalid");
            publishSnapshot(false);
            return;
        }
        const auto previousCorrection = m_backend->state().ppmCorrection;
        const auto result = m_backend->setPpmCorrection(ppmCorrection);
        if (!result.succeeded()) {
            m_statusText = QString::fromStdString(result.message);
            publishSnapshot(false);
            return;
        }
        const double effectiveCorrection = m_backend->state().ppmCorrection;
        const auto& limits = m_backend->limits();
        if (!std::isfinite(effectiveCorrection) ||
            effectiveCorrection < limits.minimumPpmCorrection ||
            effectiveCorrection > limits.maximumPpmCorrection ||
            !m_selectedCapabilities.has_value() ||
            ppmCorrectionSettingsKey(*m_selectedCapabilities).isEmpty()) {
            static_cast<void>(m_backend->setPpmCorrection(previousCorrection));
            m_statusText = QStringLiteral(
                "PPM correction readback was invalid; the previous correction was restored");
            publishSnapshot(false);
            return;
        }

        QSettings settings;
        if (!savePpmCorrection(
                settings, *m_selectedCapabilities, effectiveCorrection)) {
            const auto restored = m_backend->setPpmCorrection(previousCorrection);
            m_statusText = QStringLiteral(
                "PPM correction was applied but could not be saved; the previous correction was %1")
                               .arg(restored.succeeded()
                                        ? QStringLiteral("restored")
                                        : QStringLiteral("not restored"));
            publishSnapshot(false);
            return;
        }

        m_selectedPpmCorrection = effectiveCorrection;
        m_statusText = QStringLiteral("PPM correction applied: %1%2 PPM")
                           .arg(effectiveCorrection >= 0.0 ? QStringLiteral("+")
                                                           : QString())
                           .arg(effectiveCorrection, 0, 'f', 0);
        publishSnapshot(true);
    }

    void startAutomaticPpmCalibration()
    {
        if (m_ppmCalibrationRunning) {
            return;
        }
        if (!requireHardwareMode(QStringLiteral("Automatic PPM calibration"))) {
            return;
        }
        if (!automaticPpmCalibrationAvailable()) {
            m_statusText = QStringLiteral(
                "Automatic PPM calibration requires a stable RTL-SDR identity, test mode, and frequency correction");
            m_ppmCalibrationStatus = QStringLiteral("failed");
            log(3, QStringLiteral("PPM"), m_statusText);
            publishSnapshot(false, false);
            return;
        }

        m_ppmCalibrationRunning = true;
        m_ppmCalibrationStatus = QStringLiteral("preparing");
        m_ppmCalibrationProgressPercent = 0;
        m_ppmCalibrationInitialReceptionRunning =
            m_backend && m_backend->state().running;
        m_ppmCalibrationCreatedBackend = !m_backend;
        m_ppmCalibrationPreviousCorrection = m_backend
                                                 ? m_backend->state().ppmCorrection
                                                 : m_selectedPpmCorrection;
        m_ppmCalibrationPreviousAudioMuted =
            m_audioOutput ? m_audioOutput->state().muted : false;
        m_ppmCalibrationLastLoggedWindowCount = 0;
        log(
            1,
            QStringLiteral("PPM"),
            QStringLiteral("Automatic calibration started for %1")
                .arg(m_selectedDeviceIdentifier));
        if (m_pollTimer) {
            m_pollTimer->stop();
        }
        if (m_audioServiceTimer) {
            m_audioServiceTimer->stop();
        }
        if (m_audioDeviceTimer) {
            m_audioDeviceTimer->stop();
        }
        if (m_waterfallTimer) {
            m_waterfallTimer->stop();
        }
        m_waterfallDelivery.stop();
        m_spectrumMetricsTimer.invalidate();
        if (m_dsdFme) {
            m_dsdFme->stop();
        }
        if (m_audioOutput) {
            m_audioOutput->setMuted(true);
            m_audioOutput->stop();
        }
        publishSnapshot(true);

        if (!m_backend && !prepareStoppedPpmCalibrationBackend()) {
            finishPpmCalibration(
                QStringLiteral("failed"), m_statusText, false);
            return;
        }
        if (!m_backend ||
            !m_backend->capabilities().automaticPpmCalibrationSupported) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                QStringLiteral(
                    "The opened device does not expose both RTL-SDR test mode and frequency correction"),
                false);
            return;
        }

        const auto begin = m_backend->beginPpmCalibration();
        if (!begin.succeeded()) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                QString::fromStdString(begin.message),
                false);
            return;
        }
        m_ppmCalibrationEstimator =
            std::make_unique<devices::PpmCalibrationEstimator>(
                m_backend->state().sampleRate);
        m_ppmCalibrationEstimator->start(m_factories.monotonicClock());
        if (m_ppmCalibrationEstimator->phase() ==
            devices::PpmCalibrationPhase::Failed) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                QString::fromStdString(
                    m_ppmCalibrationEstimator->failureMessage()),
                false);
            return;
        }
        m_ppmCalibrationStatus = QStringLiteral("settling");
        m_statusText = QStringLiteral("Automatic PPM calibration is settling");
        publishSnapshot(true);
        m_ppmCalibrationTimer->start(0);
    }

    void cancelAutomaticPpmCalibration()
    {
        if (!m_ppmCalibrationRunning) {
            return;
        }
        finishPpmCalibration(
            QStringLiteral("cancelled"),
            QStringLiteral("Automatic PPM calibration cancelled"),
            false);
    }

    void readPpmCalibration()
    {
        if (!m_ppmCalibrationRunning || !m_backend ||
            !m_ppmCalibrationEstimator) {
            return;
        }
        auto read = m_backend->readPpmCalibrationBytes(
            m_ppmCalibrationReadBuffer,
            std::chrono::milliseconds(ppmCalibrationReadTimeoutMilliseconds));
        if (read.droppedData) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                read.message.empty()
                    ? QStringLiteral(
                          "The SDR stream reported dropped calibration data")
                    : QString::fromStdString(read.message),
                false);
            return;
        }
        if (read.status == radio::PpmCalibrationReadStatus::Timeout) {
            m_ppmCalibrationTimer->start(0);
            return;
        }
        if (read.status != radio::PpmCalibrationReadStatus::Bytes ||
            read.byteCount == 0 ||
            read.byteCount > m_ppmCalibrationReadBuffer.size()) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                read.message.empty()
                    ? QStringLiteral(
                          "The SDR device disconnected during PPM calibration")
                    : QString::fromStdString(read.message),
                false);
            return;
        }

        m_ppmCalibrationEstimator->ingest(
            std::span<const std::uint8_t>(
                m_ppmCalibrationReadBuffer.data(), read.byteCount),
            m_factories.monotonicClock(),
            read.droppedData);
        const auto& windows = m_ppmCalibrationEstimator->windows();
        while (m_ppmCalibrationLastLoggedWindowCount < windows.size()) {
            const auto& window =
                windows[m_ppmCalibrationLastLoggedWindowCount];
            log(
                1,
                QStringLiteral("PPM"),
                QStringLiteral(
                    "Measurement window %1: %2 complex samples in %3 s = %4 PPM")
                    .arg(m_ppmCalibrationLastLoggedWindowCount + 1)
                    .arg(window.complexSamples)
                    .arg(
                        static_cast<double>(window.elapsedNanoseconds) /
                            1'000'000'000.0,
                        0,
                        'f',
                        6)
                    .arg(window.measuredPpm, 0, 'f', 4));
            ++m_ppmCalibrationLastLoggedWindowCount;
        }
        updatePpmCalibrationProgress();
        if (m_ppmCalibrationEstimator->phase() ==
            devices::PpmCalibrationPhase::Failed) {
            const QString failure = QString::fromStdString(
                m_ppmCalibrationEstimator->failureMessage());
            log(
                2,
                QStringLiteral("PPM"),
                QStringLiteral("Measurement rejected: %1").arg(failure));
            finishPpmCalibration(
                QStringLiteral("failed"), failure, false);
            return;
        }
        if (m_ppmCalibrationEstimator->phase() ==
            devices::PpmCalibrationPhase::ReadyToApply) {
            applyMeasuredPpmCalibration();
            return;
        }
        publishSnapshot(true);
        m_ppmCalibrationTimer->start(0);
    }

    void setDemodulationMode(int mode)
    {
        if (mode < static_cast<int>(radio::DemodulationMode::Am) ||
            mode > static_cast<int>(
                       radio::DemodulationMode::DigitalDecoderOutput)) {
            m_statusText = QStringLiteral("Unsupported demodulation mode");
            publishSnapshot(false);
            return;
        }
        if (!m_backend) {
            m_statusText = QStringLiteral(
                "Select a receiver backend before setting mode");
            publishSnapshot(false);
            return;
        }
        try {
            auto result = m_backend->setDemodulationMode(
                static_cast<radio::DemodulationMode>(mode));
            if (result.succeeded() && result.stateChanged) {
                const auto remembered = m_filterWidths[static_cast<std::size_t>(mode)];
                const auto widthResult = m_backend->setFilterWidth(remembered);
                if (widthResult.succeeded()) {
                    result.message = widthResult.message;
                }
                m_backend->clearAudioSamples();
                if (m_audioOutput) {
                    m_audioOutput->flush();
                }
                if (m_dsdFme) {
                    if (static_cast<radio::DemodulationMode>(mode) ==
                            radio::DemodulationMode::DigitalDecoderOutput &&
                        m_backend->state().running) {
                        m_backend->clearDecoderInputSamples();
                        m_lastDecoderInputDroppedSamples =
                            m_backend->decoderInputDroppedSamples();
                        m_dsdFme->start();
                    } else {
                        m_dsdFme->stop();
                    }
                }
            }
            if (result.succeeded()) {
                persistCurrentReceiverControls();
            }
            m_statusText = QString::fromStdString(result.message);
            publishSnapshot(result.succeeded());
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral("Receiver operation failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            m_statusText = QStringLiteral(
                "Receiver operation failed with an unknown error");
            publishSnapshot(false);
        }
    }

    void setSquelchLevel(double squelchLevelDb)
    {
        applySquelchOperation([squelchLevelDb](radio::ReceiverBackend& backend) {
            return backend.setSquelchLevel(squelchLevelDb);
        });
    }

    void enableManualSquelch()
    {
        applySquelchOperation([](radio::ReceiverBackend& backend) {
            return backend.enableManualSquelch();
        });
    }

    void autoSquelch()
    {
        if (m_autoSquelchRunning) {
            return;
        }
        if (!m_backend || !m_backend->state().running) {
            m_statusText = QStringLiteral(
                "Start reception before measuring squelch");
            publishSnapshot(false);
            return;
        }
        const auto measurement = m_backend->squelchSignalStrengthDb();
        if (!measurement.has_value() || !std::isfinite(*measurement)) {
            m_statusText = QStringLiteral(
                "Squelch signal strength is unavailable");
            publishSnapshot(false);
            return;
        }
        m_autoSquelchRunning = true;
        m_autoSquelchSamples.clear();
        m_autoSquelchElapsed.start();
        m_statusText = QStringLiteral("Measuring squelch…");
        sampleAutoSquelch();
        publishSnapshot(true);
    }

    void disableSquelch()
    {
        applySquelchOperation([](radio::ReceiverBackend& backend) {
            return backend.disableSquelch();
        });
    }

    void applyBookmark(quint64 frequency, double requestedGainDb, int mode,
        quint64 filterWidth, double squelchThresholdDb, bool squelchEnabled,
        bool applySquelch)
    {
        if (!m_backend || mode < static_cast<int>(radio::DemodulationMode::Am) ||
            mode > static_cast<int>(
                       radio::DemodulationMode::DigitalDecoderOutput)) {
            m_statusText = QStringLiteral("Bookmark settings are unavailable");
            publishSnapshot(false);
            return;
        }
        if (m_centerFrequencyTimer) m_centerFrequencyTimer->stop();
        m_pendingCenterFrequency.reset();
        try {
            BookmarkApplication changes;
            if (m_backend->state().centerFrequency != frequency ||
                m_backend->state().listeningFrequency != frequency) {
                const auto result = m_backend->setCenterFrequency(frequency);
                if (!bookmarkOperationSucceeded(result)) {
                    publishSnapshot(false);
                    return;
                }
                changes.tuningChanged = result.stateChanged;
            }
            if (!applyBookmarkOwnedSettings(requestedGainDb, mode, filterWidth,
                    squelchThresholdDb, squelchEnabled, applySquelch,
                    changes)) {
                publishSnapshot(false);
                return;
            }
            finishBookmarkApplication(changes, true);
            m_statusText = QStringLiteral("Bookmark tuned");
            publishSnapshot(true);
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral("Receiver operation failed: %1").arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            m_statusText = QStringLiteral("Receiver operation failed with an unknown error");
            publishSnapshot(false);
        }
    }

    void applyScannerBookmark(quint64 frequency, double requestedGainDb,
        int mode, quint64 filterWidth, double squelchThresholdDb,
        bool squelchEnabled)
    {
        if (!m_backend || mode < static_cast<int>(radio::DemodulationMode::Am) ||
            mode > static_cast<int>(
                       radio::DemodulationMode::DigitalDecoderOutput)) {
            emit scannerListeningFrequencyRequestCompleted(
                frequency, m_backend ? m_backend->state().listeningFrequency : 0,
                false, QStringLiteral("Bookmark settings are unavailable"));
            return;
        }
        try {
            BookmarkApplication changes;
            if (!applyBookmarkOwnedSettings(requestedGainDb, mode, filterWidth,
                    squelchThresholdDb, squelchEnabled, true, changes)) {
                publishSnapshot(false);
                emit scannerListeningFrequencyRequestCompleted(
                    frequency, m_backend->state().listeningFrequency, false,
                    m_statusText);
                return;
            }
            if (m_backend->state().listeningFrequency != frequency) {
                const auto result = m_backend->setListeningFrequency(frequency);
                if (!bookmarkOperationSucceeded(result)) {
                    publishSnapshot(false);
                    emit scannerListeningFrequencyRequestCompleted(
                        frequency, m_backend->state().listeningFrequency, false,
                        m_statusText);
                    return;
                }
                changes.tuningChanged = result.stateChanged;
            }
            finishBookmarkApplication(changes, false);
            m_statusText = QStringLiteral("Bookmark scanner tuned");
            publishSnapshot(true);
            emit scannerListeningFrequencyRequestCompleted(
                frequency, m_backend->state().listeningFrequency, true,
                m_statusText);
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral("Receiver operation failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
            emit scannerListeningFrequencyRequestCompleted(
                frequency, m_backend->state().listeningFrequency, false,
                m_statusText);
        } catch (...) {
            m_statusText = QStringLiteral(
                "Receiver operation failed with an unknown error");
            publishSnapshot(false);
            emit scannerListeningFrequencyRequestCompleted(
                frequency, m_backend->state().listeningFrequency, false,
                m_statusText);
        }
    }

signals:
    void snapshotChanged(const sdr::app::ReceiverRuntimeSnapshot& snapshot);
    void centerFrequencyRequestCompleted(quint64 frequency, bool succeeded);
    void scannerListeningFrequencyRequestCompleted(
        quint64 requestedFrequency,
        quint64 appliedFrequency,
        bool succeeded,
        const QString& message);
    void scannerCenterFrequencyRequestCompleted(
        quint64 requestedFrequency,
        quint64 appliedCenterFrequency,
        quint64 appliedListeningFrequency,
        bool succeeded,
        const QString& message);
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
    struct BookmarkApplication {
        bool receiverSettingsChanged = false;
        bool tuningChanged = false;
        bool decoderLifecycleChanged = false;
    };

    bool bookmarkOperationSucceeded(const radio::OperationResult& result)
    {
        if (result.succeeded()) {
            return true;
        }
        m_statusText = QString::fromStdString(result.message);
        return false;
    }

    bool applyBookmarkOwnedSettings(double requestedGainDb, int mode,
        quint64 filterWidth, double squelchThresholdDb, bool squelchEnabled,
        bool applySquelch, BookmarkApplication& changes)
    {
        const auto requestedMode = static_cast<radio::DemodulationMode>(mode);
        const auto originalMode = m_backend->state().demodulationMode;
        if (std::abs(m_requestedGainDb - requestedGainDb) > 1.0e-9) {
            const auto result = m_backend->setGain(requestedGainDb);
            if (!bookmarkOperationSucceeded(result)) return false;
            changes.receiverSettingsChanged |= result.stateChanged;
            m_requestedGainDb = requestedGainDb;
            m_savedGainDb = requestedGainDb;
        }
        if (m_backend->state().demodulationMode != requestedMode) {
            const auto result = m_backend->setDemodulationMode(requestedMode);
            if (!bookmarkOperationSucceeded(result)) return false;
            changes.receiverSettingsChanged |= result.stateChanged;
            changes.decoderLifecycleChanged =
                originalMode == radio::DemodulationMode::DigitalDecoderOutput ||
                requestedMode == radio::DemodulationMode::DigitalDecoderOutput;
        }
        if (m_backend->state().filterWidth != filterWidth) {
            const auto result = m_backend->setFilterWidth(filterWidth);
            if (!bookmarkOperationSucceeded(result)) return false;
            changes.receiverSettingsChanged |= result.stateChanged;
        }
        if (applySquelch) {
            const auto& state = m_backend->state();
            const bool thresholdChanged =
                std::abs(state.manualSquelchLevelDb - squelchThresholdDb) >
                1.0e-9;
            if (squelchEnabled &&
                (thresholdChanged ||
                 state.squelchMode != radio::SquelchMode::Manual)) {
                const auto result =
                    m_backend->setSquelchLevel(squelchThresholdDb);
                if (!bookmarkOperationSucceeded(result)) return false;
                changes.receiverSettingsChanged |= result.stateChanged;
            } else if (!squelchEnabled) {
                if (thresholdChanged) {
                    const auto result =
                        m_backend->setSquelchLevel(squelchThresholdDb);
                    if (!bookmarkOperationSucceeded(result)) return false;
                    changes.receiverSettingsChanged |= result.stateChanged;
                }
                if (m_backend->state().squelchMode !=
                    radio::SquelchMode::Disabled) {
                    const auto result = m_backend->disableSquelch();
                    if (!bookmarkOperationSucceeded(result)) return false;
                    changes.receiverSettingsChanged |= result.stateChanged;
                }
            }
        }
        m_filterWidths[static_cast<std::size_t>(mode)] = filterWidth;
        return true;
    }

    void finishBookmarkApplication(
        const BookmarkApplication& changes, bool persistTuning)
    {
        if (changes.tuningChanged || changes.receiverSettingsChanged) {
            flushDecoderAfterRetune();
        }
        if (changes.receiverSettingsChanged ||
            (persistTuning && changes.tuningChanged)) {
            persistCurrentReceiverControls();
        }
        if (changes.decoderLifecycleChanged) {
            m_backend->clearAudioSamples();
            if (m_audioOutput) m_audioOutput->flush();
            if (m_dsdFme) {
                if (m_backend->state().demodulationMode ==
                        radio::DemodulationMode::DigitalDecoderOutput &&
                    m_backend->state().running) {
                    m_dsdFme->start();
                } else {
                    m_dsdFme->stop();
                }
            }
        }
    }

    [[nodiscard]] bool automaticPpmCalibrationAvailable() const
    {
        if (!m_selectedCapabilities ||
            stablePpmDeviceIdentity(*m_selectedCapabilities).isEmpty()) {
            return false;
        }
        const auto& capabilities = m_selectedCapabilities->capabilities;
        if (capabilities.ppmCorrectionSupported &&
            capabilities.rtlSdrTestModeSupported) {
            return true;
        }
        return !m_selectedCapabilitiesConfirmed &&
               devices::isRtlSdrDriver(m_selectedCapabilities->driver);
    }

    bool prepareStoppedPpmCalibrationBackend()
    {
        try {
            const QString requestedIdentifier = m_selectedDeviceIdentifier;
            auto controller = std::make_unique<devices::DeviceController>(
                m_factories.createDeviceProvider());
            const auto discovery = controller->discover();
            if (!discovery.succeeded()) {
                m_statusText =
                    QStringLiteral("Could not verify the selected SDR: %1")
                        .arg(QString::fromStdString(discovery.message));
                return false;
            }
            m_devices = controller->devices();
            const auto selected = std::ranges::find_if(
                m_devices,
                [&requestedIdentifier](const devices::DeviceDescriptor& device) {
                    return device.identifier ==
                           requestedIdentifier.toStdString();
                });
            if (selected == m_devices.end()) {
                m_statusText = QStringLiteral(
                    "The selected SDR disappeared before calibration");
                return false;
            }
            const auto selection =
                controller->selectDevice(requestedIdentifier.toStdString());
            if (!selection.succeeded()) {
                m_statusText =
                    QStringLiteral("Opening the selected SDR failed: %1")
                        .arg(QString::fromStdString(selection.message));
                return false;
            }
            const devices::DeviceDescriptor openedDevice =
                *controller->selectedDevice();
            setSelectedDevice(openedDevice, true);
            auto replacement =
                m_factories.createHardwareBackend(std::move(controller));
            if (!replacement) {
                m_statusText =
                    QStringLiteral("Failed to create the calibration backend");
                return false;
            }
            m_backend = std::move(replacement);
            if (openedDevice.capabilities.ppmCorrectionSupported) {
                const auto ppm =
                    m_backend->setPpmCorrection(m_selectedPpmCorrection);
                if (!ppm.succeeded()) {
                    m_statusText = QString::fromStdString(ppm.message);
                    return false;
                }
                m_selectedPpmCorrection =
                    m_backend->state().ppmCorrection;
            }
            const auto sampleRate =
                m_backend->setSampleRate(m_requestedCaptureBandwidth);
            if (!sampleRate.succeeded()) {
                m_statusText = QString::fromStdString(sampleRate.message);
                return false;
            }
            return true;
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral(
                "Preparing automatic PPM calibration failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            return false;
        } catch (...) {
            m_statusText = QStringLiteral(
                "Preparing automatic PPM calibration failed");
            return false;
        }
    }

    void updatePpmCalibrationProgress()
    {
        if (!m_ppmCalibrationEstimator) {
            return;
        }
        m_ppmCalibrationProgressPercent =
            m_ppmCalibrationEstimator->progressPercent();
        switch (m_ppmCalibrationEstimator->phase()) {
        case devices::PpmCalibrationPhase::Settling:
            m_ppmCalibrationStatus = QStringLiteral("settling");
            break;
        case devices::PpmCalibrationPhase::MeasuringFirst:
            m_ppmCalibrationStatus = QStringLiteral("measuring 1/3");
            break;
        case devices::PpmCalibrationPhase::MeasuringSecond:
            m_ppmCalibrationStatus = QStringLiteral("measuring 2/3");
            break;
        case devices::PpmCalibrationPhase::MeasuringThird:
            m_ppmCalibrationStatus = QStringLiteral("measuring 3/3");
            break;
        case devices::PpmCalibrationPhase::ReadyToApply:
            m_ppmCalibrationStatus = QStringLiteral("applying");
            m_ppmCalibrationProgressPercent = 95;
            break;
        case devices::PpmCalibrationPhase::Failed:
            m_ppmCalibrationStatus = QStringLiteral("failed");
            break;
        }
        m_statusText =
            QStringLiteral("Automatic PPM calibration: %1")
                .arg(m_ppmCalibrationStatus);
    }

    void restoreLiveServicesAfterPpmCalibration(bool restartTimers = true)
    {
        if (restartTimers) {
            if (m_pollTimer) {
                m_pollTimer->start();
            }
            if (m_audioServiceTimer) {
                m_audioServiceTimer->start();
            }
            if (m_audioDeviceTimer) {
                m_audioDeviceTimer->start();
            }
            scheduleNextWaterfallTick();
        }
        const bool receptionValid =
            m_ppmCalibrationInitialReceptionRunning && m_backend &&
            m_backend->state().running;
        if (receptionValid) {
            resetWaterfallDelivery();
            if (m_audioOutput) {
                static_cast<void>(m_audioOutput->start());
                m_audioOutput->setMuted(
                    m_ppmCalibrationPreviousAudioMuted);
            }
            if (m_dsdFme &&
                m_backend->state().demodulationMode ==
                    radio::DemodulationMode::DigitalDecoderOutput) {
                m_backend->clearDecoderInputSamples();
                m_lastDecoderInputDroppedSamples =
                    m_backend->decoderInputDroppedSamples();
                m_dsdFme->start();
            }
        } else {
            if (m_audioOutput) {
                m_audioOutput->stop();
                m_audioOutput->setMuted(
                    m_ppmCalibrationPreviousAudioMuted);
            }
            if (m_dsdFme) {
                m_dsdFme->stop();
            }
        }
    }

    void applyMeasuredPpmCalibration()
    {
        if (!m_backend || !m_ppmCalibrationEstimator ||
            !m_ppmCalibrationEstimator->correctionPpm().has_value()) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                QStringLiteral("Calibration produced no reliable correction"),
                false);
            return;
        }
        m_ppmCalibrationStatus = QStringLiteral("applying");
        m_ppmCalibrationProgressPercent = 95;
        publishSnapshot(true);

        const auto testModeOff = m_backend->endPpmCalibration();
        if (!testModeOff.succeeded()) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                QString::fromStdString(testModeOff.message),
                false);
            return;
        }
        const int measuredCorrection =
            *m_ppmCalibrationEstimator->correctionPpm();
        const auto applied =
            m_backend->setPpmCorrection(measuredCorrection);
        if (!applied.succeeded()) {
            finishPpmCalibration(
                QStringLiteral("failed"),
                QString::fromStdString(applied.message),
                false);
            return;
        }
        const double effectiveCorrection =
            m_backend->state().ppmCorrection;
        const auto resumed =
            m_backend->resumeReceptionAfterPpmCalibration();
        if (!resumed.succeeded()) {
            static_cast<void>(m_backend->setPpmCorrection(
                m_ppmCalibrationPreviousCorrection));
            finishPpmCalibration(
                QStringLiteral("failed"),
                QStringLiteral("Applying PPM succeeded, but reception restoration failed: %1")
                    .arg(QString::fromStdString(resumed.message)),
                false);
            return;
        }

        QSettings settings;
        if (!m_selectedCapabilities ||
            !savePpmCorrection(
                settings, *m_selectedCapabilities, effectiveCorrection)) {
            static_cast<void>(m_backend->setPpmCorrection(
                m_ppmCalibrationPreviousCorrection));
            finishPpmCalibration(
                QStringLiteral("failed"),
                QStringLiteral(
                    "Calibration result was not saved; the previous correction was restored"),
                false);
            return;
        }

        const bool correctionChanged =
            effectiveCorrection != m_ppmCalibrationPreviousCorrection;
        m_selectedPpmCorrection = effectiveCorrection;
        restoreLiveServicesAfterPpmCalibration();
        if (correctionChanged) {
            ++m_ppmCalibrationDisplayResetGeneration;
        }
        if (m_ppmCalibrationCreatedBackend &&
            !m_ppmCalibrationInitialReceptionRunning) {
            m_backend.reset();
        }
        m_ppmCalibrationRunning = false;
        m_ppmCalibrationStatus = QStringLiteral("completed");
        m_ppmCalibrationProgressPercent = 100;
        m_statusText =
            QStringLiteral("Automatic PPM calibration completed: %1%2 PPM")
                .arg(effectiveCorrection >= 0.0 ? QStringLiteral("+")
                                                : QString())
                .arg(effectiveCorrection, 0, 'f', 0);
        log(1, QStringLiteral("PPM"), m_statusText);
        log(
            1,
            QStringLiteral("PPM"),
            QStringLiteral(
                "Receiver state restored; reception %1 as before calibration")
                .arg(m_ppmCalibrationInitialReceptionRunning
                         ? QStringLiteral("running")
                         : QStringLiteral("stopped")));
        m_ppmCalibrationEstimator.reset();
        publishSnapshot(true);
    }

    void finishPpmCalibration(
        const QString& finalStatus,
        const QString& detail,
        bool shuttingDown)
    {
        if (m_ppmCalibrationTimer) {
            m_ppmCalibrationTimer->stop();
        }
        QStringList restorationFailures;
        if (m_backend) {
            const auto testModeOff = m_backend->endPpmCalibration();
            if (!testModeOff.succeeded()) {
                restorationFailures.append(
                    QString::fromStdString(testModeOff.message));
            }
            if (m_backend->capabilities().ppmCorrectionSupported) {
                const auto correction = m_backend->setPpmCorrection(
                    m_ppmCalibrationPreviousCorrection);
                if (!correction.succeeded()) {
                    restorationFailures.append(
                        QString::fromStdString(correction.message));
                }
            }
            const auto resumed =
                m_backend->resumeReceptionAfterPpmCalibration();
            if (!resumed.succeeded()) {
                restorationFailures.append(
                    QString::fromStdString(resumed.message));
            }
        }
        restoreLiveServicesAfterPpmCalibration(!shuttingDown);
        if (m_ppmCalibrationCreatedBackend &&
            !m_ppmCalibrationInitialReceptionRunning) {
            m_backend.reset();
        }
        m_ppmCalibrationRunning = false;
        m_ppmCalibrationStatus = finalStatus;
        if (finalStatus == QStringLiteral("cancelled")) {
            log(1, QStringLiteral("PPM"), detail);
        } else {
            log(3, QStringLiteral("PPM"), detail);
        }
        QString restorationMessage = QStringLiteral(
            "Previous PPM correction and receiver state restored");
        if (!restorationFailures.isEmpty()) {
            restorationMessage += QStringLiteral("; restoration issue: ") +
                                  restorationFailures.join(
                                      QStringLiteral("; "));
        }
        log(
            restorationFailures.isEmpty() ? 1 : 3,
            QStringLiteral("PPM"),
            restorationMessage);
        m_statusText = detail;
        if (!restorationFailures.isEmpty()) {
            m_statusText += QStringLiteral("; ") + restorationMessage;
        }
        m_ppmCalibrationEstimator.reset();
        if (!shuttingDown) {
            publishSnapshot(
                finalStatus == QStringLiteral("cancelled") &&
                    restorationFailures.isEmpty(),
                false);
        }
    }

    [[nodiscard]] bool hardwareFactoriesAvailable() const
    {
        return static_cast<bool>(m_factories.createDeviceProvider) &&
               static_cast<bool>(m_factories.createHardwareBackend);
    }

    bool requireHardwareMode(const QString& operation)
    {
        if (m_startupMode == StartupMode::Mock) {
            m_statusText = operation + QStringLiteral(" is unavailable in mock mode");
            publishSnapshot(false);
            return false;
        }
        if (!hardwareFactoriesAvailable()) {
            m_statusText = operation +
                           QStringLiteral(" requires the desktop-app build preset");
            publishSnapshot(false);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool applyRestoredReceiverControls()
    {
        if (!m_backend) {
            return false;
        }

        const auto apply = [this](const radio::OperationResult& result) {
            if (result.succeeded()) {
                return true;
            }
            m_statusText = QString::fromStdString(result.message);
            return false;
        };

        if (!apply(m_backend->setCenterFrequency(m_receiverControls.centerFrequency)) ||
            !apply(m_backend->setListeningFrequency(
                m_receiverControls.listeningFrequency)) ||
            !apply(m_backend->setDemodulationMode(
                m_receiverControls.demodulationMode)) ||
            !apply(m_backend->setSquelchLevel(
                m_receiverControls.squelchThresholdDb))) {
            return false;
        }
        if (m_receiverControls.squelchDisabled &&
            !apply(m_backend->disableSquelch())) {
            return false;
        }
        return true;
    }

    void persistCurrentReceiverControls()
    {
        if (!m_backend) {
            return;
        }
        const auto& state = m_backend->state();
        m_receiverControls.centerFrequency = state.centerFrequency;
        m_receiverControls.listeningFrequency = state.listeningFrequency;
        m_receiverControls.demodulationMode = state.demodulationMode;
        m_receiverControls.squelchThresholdDb = state.manualSquelchLevelDb;
        m_receiverControls.squelchDisabled =
            state.squelchMode == radio::SquelchMode::Disabled;

        QSettings settings;
        saveReceiverControlSettings(settings, m_receiverControls);
        settings.sync();
    }

    template <typename Operation>
    void applySquelchOperation(Operation operation)
    {
        if (!m_backend) {
            m_statusText = QStringLiteral("Select a receiver backend before setting squelch");
            publishSnapshot(false);
            return;
        }
        try {
            const auto result = operation(*m_backend);
            if (result.succeeded()) {
                persistCurrentReceiverControls();
            }
            m_statusText = QString::fromStdString(result.message);
            publishSnapshot(result.succeeded());
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral("Receiver operation failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            m_statusText = QStringLiteral("Receiver operation failed with an unknown error");
            publishSnapshot(false);
        }
    }

    template <typename Operation>
    void applyBackendOperation(const QString& noBackendMessage, Operation operation)
    {
        if (!m_backend) {
            m_statusText = noBackendMessage;
            publishSnapshot(false);
            return;
        }
        try {
            const radio::OperationResult result = operation(*m_backend);
            m_statusText = QString::fromStdString(result.message);
            publishSnapshot(result.succeeded());
        } catch (const std::exception& error) {
            m_statusText = QStringLiteral("Receiver operation failed: %1")
                               .arg(QString::fromUtf8(error.what()));
            publishSnapshot(false);
        } catch (...) {
            m_statusText = QStringLiteral("Receiver operation failed with an unknown error");
            publishSnapshot(false);
        }
    }

    template <typename Operation>
    [[nodiscard]] bool applyTuningOperation(Operation operation)
    {
        if (!m_backend) {
            m_statusText = QStringLiteral("Select a receiver backend before tuning");
            publishSnapshot(false);
            return false;
        }
        const auto result = operation(*m_backend);
        if (result.succeeded() && result.stateChanged) {
            m_backend->clearAudioSamples();
            flushDecoderAfterRetune();
            if (m_audioOutput) {
                m_audioOutput->flush();
            }
        }
        if (result.succeeded()) {
            persistCurrentReceiverControls();
        }
        m_statusText = QString::fromStdString(result.message);
        publishSnapshot(result.succeeded());
        return result.succeeded();
    }

    void flushDecoderAfterRetune()
    {
        if (m_backend) {
            m_backend->clearDecoderInputSamples();
            m_lastDecoderInputDroppedSamples =
                m_backend->decoderInputDroppedSamples();
        }
        if (m_dsdFme) {
            m_dsdFme->flushDecodedOutput();
        }
    }

    void sampleAutoSquelch()
    {
        if (!m_autoSquelchRunning) {
            return;
        }
        if (!m_backend || !m_backend->state().running) {
            m_autoSquelchRunning = false;
            m_autoSquelchSamples.clear();
            m_statusText = QStringLiteral(
                "Squelch measurement stopped with reception");
            publishSnapshot(false);
            return;
        }

        if (const auto measurement = m_backend->squelchSignalStrengthDb();
            measurement.has_value() && std::isfinite(*measurement)) {
            m_autoSquelchSamples.push_back(*measurement);
        }
        if (!m_autoSquelchElapsed.isValid() ||
            m_autoSquelchElapsed.elapsed() <
                autoSquelchMeasurementWindowMilliseconds) {
            return;
        }

        const auto threshold = radio::estimateOneShotSquelchThreshold(
            m_autoSquelchSamples, m_backend->limits());
        m_autoSquelchRunning = false;
        m_autoSquelchElapsed.invalidate();
        m_autoSquelchSamples.clear();
        if (!threshold.has_value()) {
            m_statusText = QStringLiteral(
                "Squelch signal strength was unavailable");
            publishSnapshot(false);
            return;
        }

        const auto result = m_backend->setSquelchLevel(*threshold);
        if (result.succeeded()) {
            persistCurrentReceiverControls();
            m_statusText = QStringLiteral("Squelch set to %1 dB")
                               .arg(*threshold, 0, 'f', 0);
        } else {
            m_statusText = QString::fromStdString(result.message);
        }
        publishSnapshot(result.succeeded());
    }

    void pollBackend()
    {
        if (m_recording && m_recording->state().active &&
            (!m_backend || !m_backend->state().running)) {
            m_recording->stop();
            publishSnapshot(true);
        }
        const std::optional<platform::AudioOutputState> previousAudioState =
            m_audioOutput
                ? std::optional<platform::AudioOutputState>(m_audioOutput->state())
                : std::nullopt;
        if (m_backend && !m_ppmCalibrationRunning) {
            if (auto runtimeError = m_backend->takeRuntimeError()) {
                if (m_audioOutput) {
                    m_audioOutput->stop();
                }
                if (m_dsdFme) {
                    m_dsdFme->stop();
                }
                if (m_recording) {
                    m_recording->stop();
                }
                m_statusText = QString::fromStdString(runtimeError->message);
                m_waterfallDelivery.stop();
                publishSnapshot(false);
            }
            auto frames = m_backend->takePendingSpectrumFrames(
                m_waterfallDelivery.capacity());
            const auto latest = std::ranges::find_if(
                frames.rbegin(),
                frames.rend(),
                [this](const radio::SpectrumFrame& frame) {
                    return radio::isCompatibleSpectrumFrame(
                        frame,
                        m_backend->effectiveSampleRate(),
                        m_backend->spectrumFftSize());
                });
            if (latest != frames.rend()) {
                QVector<float> magnitudes(
                    latest->normalizedMagnitudes.begin(),
                    latest->normalizedMagnitudes.end());
                emit spectrumFrameReady(
                    magnitudes,
                    latest->centerFrequency,
                    latest->sampleRate,
                    static_cast<quint64>(latest->fftSize),
                    latest->sequence,
                    latest->timestampNanoseconds,
                    latest->tuningGeneration);
                const std::uint64_t now = m_factories.monotonicClock();
                m_lastSpectrumSourceAgeNanoseconds =
                    now - std::min(now, latest->timestampNanoseconds);
                ++m_spectrumFramesDisplayed;
            }
            for (auto& frame : frames) {
                static_cast<void>(m_waterfallDelivery.enqueue(std::move(frame)));
            }
            const bool squelchOpen = m_backend->squelchOpen();
            const bool measurementAvailable =
                [this] {
                    const auto measurement =
                        m_backend->squelchSignalStrengthDb();
                    return measurement.has_value() &&
                           std::isfinite(*measurement);
                }();
            if (!m_lastPublishedSquelchOpen.has_value() ||
                *m_lastPublishedSquelchOpen != squelchOpen ||
                !m_lastPublishedSquelchMeasurement.has_value() ||
                *m_lastPublishedSquelchMeasurement != measurementAvailable) {
                m_lastPublishedSquelchOpen = squelchOpen;
                m_lastPublishedSquelchMeasurement = measurementAvailable;
                publishSnapshot(true);
            }
            sampleAutoSquelch();
        }
        reportSpectrumMetrics();
        if (m_audioOutput &&
            (!previousAudioState.has_value() ||
             *previousAudioState != m_audioOutput->state())) {
            publishSnapshot(true);
        }
    }

    void publishNextWaterfallRow()
    {
        if (m_ppmCalibrationRunning) {
            scheduleNextWaterfallTick();
            return;
        }
        if (m_waterfallDelivery.size() > 0) {
            auto frame = m_waterfallDelivery.takeLatestRow();
            if (frame) {
                if (m_waterfallPublishIntervalTimer.isValid()) {
                    m_lastWaterfallPublishedIntervalNanoseconds =
                        static_cast<std::uint64_t>(
                            m_waterfallPublishIntervalTimer.nsecsElapsed());
                    m_waterfallPublishIntervalTimer.restart();
                } else {
                    m_waterfallPublishIntervalTimer.start();
                }
                QVector<float> magnitudes(
                    frame->normalizedMagnitudes.begin(),
                    frame->normalizedMagnitudes.end());
                emit waterfallFrameReady(
                    magnitudes,
                    frame->centerFrequency,
                    frame->sampleRate,
                    static_cast<quint64>(frame->fftSize),
                    frame->sequence,
                    frame->timestampNanoseconds,
                    frame->tuningGeneration);
                const std::uint64_t now = m_factories.monotonicClock();
                m_lastWaterfallSourceAgeNanoseconds =
                    now - std::min(now, frame->timestampNanoseconds);
            }
        }
        scheduleNextWaterfallTick();
    }

    void scheduleNextWaterfallTick()
    {
        if (!m_waterfallTimer) {
            return;
        }
        double achievableRate = 0.0;
        if (m_backend && m_backend->state().running) {
            achievableRate =
                m_backend->spectrumProcessingMetrics()
                    .achievableFramesPerSecond;
        }
        const auto interval = nextWaterfallPresentationInterval(
            waterfallLiveRowsPerSecond,
            achievableRate,
            m_waterfallTimerFractionalMilliseconds);
        m_waterfallTimerFractionalMilliseconds =
            interval.fractionalMilliseconds;
        m_waterfallTimer->start(interval.milliseconds);
    }

    void resetWaterfallDelivery()
    {
        m_waterfallPublishIntervalTimer.invalidate();
        m_lastWaterfallPublishedIntervalNanoseconds = 0;
        m_waterfallTimerFractionalMilliseconds = 0.0;
        if (!m_backend || !m_backend->state().running) {
            m_waterfallDelivery.stop();
            return;
        }
        m_waterfallDelivery.reset(
            m_backend->effectiveSampleRate(),
            m_backend->spectrumProcessingMetrics().fftSize);
        m_spectrumMetricsTimer.invalidate();
    }

    void serviceAudio()
    {
        if (m_ppmCalibrationRunning) {
            return;
        }
        if (!m_backend || !m_backend->state().running) {
            return;
        }

        ++m_audioServicePasses;
        const std::uint64_t dropped = m_backend->audioDroppedSamples();
        if (m_audioOutput && dropped > m_lastBackendAudioDroppedSamples) {
            m_audioOutput->reportUpstreamOverflow(
                dropped - m_lastBackendAudioDroppedSamples);
        }
        m_lastBackendAudioDroppedSamples = dropped;
        const bool digital =
            m_backend->state().demodulationMode ==
            radio::DemodulationMode::DigitalDecoderOutput;
        if (digital && m_dsdFme && m_audioOutput) {
            if (!m_decoderAudioDiagnosticsActive) {
                const auto& audio = m_audioOutput->state();
                m_lastDecoderAudioUnderruns = audio.underrunEvents;
                m_lastDecoderPlatformAudioUnderruns =
                    audio.platformUnderrunEvents;
                m_decoderAudioDiagnosticsActive = true;
            }
        } else {
            m_decoderAudioDiagnosticsActive = false;
        }
        const auto previousDsdState =
            m_dsdFme
                ? std::optional<platform::DsdFmeProcessState>(
                      m_dsdFme->state())
                : std::nullopt;
        if (digital && m_dsdFme) {
            const std::uint64_t decoderDropped =
                m_backend->decoderInputDroppedSamples();
            if (decoderDropped > m_lastDecoderInputDroppedSamples) {
                const std::uint64_t droppedDelta =
                    decoderDropped - m_lastDecoderInputDroppedSamples;
                m_dsdFme->reportInputDiscontinuity(droppedDelta);
                if (m_audioOutput) {
                    m_audioOutput->reportUpstreamOverflow(droppedDelta);
                }
            }
            m_lastDecoderInputDroppedSamples = decoderDropped;
            const auto decoderInput = m_backend->takeDecoderInputSamples(
                maximumAudioTransferFrames);
            m_dsdFme->enqueueDiscriminator(decoderInput);
            m_dsdFme->process();
            const auto decoded = m_dsdFme->takeDecodedStereo(
                m_recording && m_recording->state().active
                    ? maximumAudioTransferFrames
                    : (m_audioOutput
                           ? std::min(
                                 maximumAudioTransferFrames,
                                 m_audioOutput->availableBufferCapacity())
                           : maximumAudioTransferFrames));
            m_audioTransferredSamples += decoded.size() / 2U;
            if (m_recording) {
                m_recording->enqueueStereo(decoded);
            }
            if (m_audioOutput) {
                m_audioOutput->enqueueStereo(decoded);
            }
        } else if (m_audioOutput || m_recording) {
            const auto audio = m_backend->takeAudioSamples(
                m_recording && m_recording->state().active
                    ? maximumAudioTransferFrames
                    : std::min(
                          maximumAudioTransferFrames,
                          m_audioOutput ? m_audioOutput->availableBufferCapacity()
                                        : maximumAudioTransferFrames));
            m_audioTransferredSamples += audio.size();
            if (m_recording) {
                m_recording->enqueueMono(audio);
            }
            if (m_audioOutput) {
                m_audioOutput->enqueueMono(audio);
            }
        }
        if (m_audioOutput) {
            m_audioOutput->process();
            if (digital && m_dsdFme &&
                m_decoderAudioDiagnosticsActive) {
                const auto& audio = m_audioOutput->state();
                const std::uint64_t softwareDelta =
                    audio.underrunEvents >= m_lastDecoderAudioUnderruns
                        ? audio.underrunEvents - m_lastDecoderAudioUnderruns
                        : audio.underrunEvents;
                const std::uint64_t platformDelta =
                    audio.platformUnderrunEvents >=
                            m_lastDecoderPlatformAudioUnderruns
                        ? audio.platformUnderrunEvents -
                              m_lastDecoderPlatformAudioUnderruns
                        : audio.platformUnderrunEvents;
                m_dsdFme->reportDecoderAudioUnderruns(
                    softwareDelta, platformDelta);
                m_lastDecoderAudioUnderruns = audio.underrunEvents;
                m_lastDecoderPlatformAudioUnderruns =
                    audio.platformUnderrunEvents;
            }
        }
        reportAudioMetrics();
        if (previousDsdState && *previousDsdState != m_dsdFme->state()) {
            publishSnapshot(true);
        }
    }

    void refreshAudioDevices()
    {
        if (!m_audioOutput) {
            return;
        }
        const auto previousState = m_audioOutput->state();
        m_audioOutput->refreshDevices();
        if (previousState != m_audioOutput->state()) {
            publishSnapshot(true);
        }
    }

    void reportAudioMetrics()
    {
        if (!m_verboseAudioMetrics || !m_audioOutput || !m_backend) {
            return;
        }
        if (!m_audioMetricsTimer.isValid()) {
            m_audioMetricsTimer.start();
            m_lastMetricsProducedSamples = m_backend->audioProducedSamples();
            m_lastMetricsTransferredSamples = m_audioTransferredSamples;
            m_lastMetricsServicePasses = m_audioServicePasses;
            m_lastMetricsWrittenSamples = m_audioOutput->state().writtenSamples;
            return;
        }
        const qint64 elapsedMilliseconds = m_audioMetricsTimer.elapsed();
        if (elapsedMilliseconds < 1'000) {
            return;
        }
        const double seconds = static_cast<double>(elapsedMilliseconds) / 1'000.0;
        const auto& audio = m_audioOutput->state();
        const auto producerRate = static_cast<double>(
            m_backend->audioProducedSamples() - m_lastMetricsProducedSamples) / seconds;
        const auto serviceRate = static_cast<double>(
            m_audioServicePasses - m_lastMetricsServicePasses) / seconds;
        const auto sinkWriteRate = static_cast<double>(
            audio.writtenSamples - m_lastMetricsWrittenSamples) / seconds;
        qInfo().noquote()
            << QStringLiteral(
                   "audio metrics: produced=%1 fps service=%2 Hz written=%3 fps "
                   "upstream=%4 frames output=%5 frames sink=%6 frames "
                   "dropped=%7 software-underruns=%8 platform-underruns=%9 "
                   "overflows=%10 dsd-stdout-bytes=%11 dsd-decoded-frames=%12 "
                   "dsd-generated-frames=%13 dsd-output-overflows=%14")
                   .arg(producerRate, 0, 'f', 0)
                   .arg(serviceRate, 0, 'f', 1)
                   .arg(sinkWriteRate, 0, 'f', 0)
                   .arg(m_backend->audioBufferedSampleCount())
                   .arg(m_audioOutput->bufferedSampleCount())
                   .arg(m_audioOutput->sinkBufferedSampleCount())
                   .arg(audio.droppedSamples)
                   .arg(audio.underrunEvents)
                   .arg(audio.platformUnderrunEvents)
                   .arg(audio.overflowEvents)
                   .arg(m_dsdFme
                            ? m_dsdFme->state().standardOutputBytesReceived
                            : 0)
                   .arg(m_dsdFme
                            ? m_dsdFme->state().decodedStereoFramesReceived
                            : 0)
                   .arg(m_dsdFme
                            ? m_dsdFme->state().generatedStereoFrames
                            : 0)
                   .arg(m_dsdFme
                            ? m_dsdFme->state().outputOverflowEvents
                            : 0);
        m_lastMetricsProducedSamples = m_backend->audioProducedSamples();
        m_lastMetricsTransferredSamples = m_audioTransferredSamples;
        m_lastMetricsServicePasses = m_audioServicePasses;
        m_lastMetricsWrittenSamples = audio.writtenSamples;
        m_audioMetricsTimer.restart();
    }

    void reportSpectrumMetrics()
    {
        if (!m_backend) {
            return;
        }
        if (!m_spectrumMetricsTimer.isValid()) {
            m_spectrumMetricsTimer.start();
            const auto metrics = m_backend->spectrumProcessingMetrics();
            m_lastFftsExecuted = metrics.fftsExecuted;
            m_lastFftFramesProduced = metrics.framesPublished;
            m_lastSpectrumFramesDisplayed = m_spectrumFramesDisplayed;
            m_lastWaterfallRowsConsumed =
                m_waterfallDelivery.metrics().rowsConsumed;
            m_lastWaterfallOverflowDrops =
                metrics.framesDropped +
                m_waterfallDelivery.metrics().overflowDrops;
            m_lastWaterfallDisplayUnderruns =
                m_waterfallDelivery.metrics().displayUnderruns;
            return;
        }
        const qint64 elapsedMilliseconds = m_spectrumMetricsTimer.elapsed();
        if (elapsedMilliseconds < 1'000) {
            return;
        }
        const double seconds = static_cast<double>(elapsedMilliseconds) / 1'000.0;
        const auto metrics = m_backend->spectrumProcessingMetrics();
        const auto& delivery = m_waterfallDelivery.metrics();
        const auto totalOverflowDrops =
            metrics.framesDropped + delivery.overflowDrops;
        const auto producedDelta = metrics.framesPublished >= m_lastFftFramesProduced
                                       ? metrics.framesPublished - m_lastFftFramesProduced
                                       : metrics.framesPublished;
        const auto fftDelta = metrics.fftsExecuted >= m_lastFftsExecuted
                                  ? metrics.fftsExecuted - m_lastFftsExecuted
                                  : metrics.fftsExecuted;
        const auto spectrumDelta =
            m_spectrumFramesDisplayed - m_lastSpectrumFramesDisplayed;
        const auto waterfallDelta =
            delivery.rowsConsumed - m_lastWaterfallRowsConsumed;
        const auto overflowDelta =
            totalOverflowDrops >= m_lastWaterfallOverflowDrops
                ? totalOverflowDrops - m_lastWaterfallOverflowDrops
                : totalOverflowDrops;
        const auto underrunDelta =
            delivery.displayUnderruns - m_lastWaterfallDisplayUnderruns;
        const double actualRowsPerSecond =
            static_cast<double>(waterfallDelta) / seconds;
        const double generatedRowsPerSecond =
            static_cast<double>(producedDelta) / seconds;
        const double rowRateTolerance = std::max(
            2.0, metrics.targetFramesPerSecond * 0.05);
        if (m_verboseAudioMetrics) {
            qInfo().noquote()
                << QStringLiteral(
                   "spectrum metrics: effective=%1 sps fft-requested=%2 bins fft-effective=%3 bins resolution=%4 Hz/bin "
                   "hop=%5 samples overlap=%6% internal-source-rate=%7 rows/s effective-fft-rate=%8 frames/s "
                   "ffts=%9/s generated=%10 frames/s spectrum-displayed=%11/s waterfall-published=%12 effective-waterfall-rate=%13 rows/s "
                   "queue=%14 dropped-rows=%15 coalesced=%16 stale-generation=%17 display-underruns=%18 produced-interval=%19 ms displayed-interval=%20 ms "
                   "spectrum-source-age=%21 ms waterfall-source-age=%22 ms sequence-gaps=%23 duplicates=%24 timestamp-regressions=%25 processing=%26 ms/fft")
                   .arg(metrics.effectiveSampleRate, 0, 'f', 0)
                   .arg(m_backend->requestedSpectrumFftSize())
                   .arg(metrics.fftSize)
                   .arg(metrics.hertzPerBin, 0, 'f', 1)
                   .arg(metrics.hopSize, 0, 'f', 3)
                   .arg(metrics.overlapPercentage, 0, 'f', 1)
                   .arg(metrics.targetFramesPerSecond, 0, 'f', 1)
                   .arg(metrics.achievableFramesPerSecond, 0, 'f', 1)
                   .arg(static_cast<double>(fftDelta) / seconds, 0, 'f', 1)
                   .arg(generatedRowsPerSecond, 0, 'f', 1)
                   .arg(static_cast<double>(spectrumDelta) / seconds, 0, 'f', 1)
                   .arg(waterfallDelta)
                   .arg(actualRowsPerSecond, 0, 'f', 1)
                   .arg(m_waterfallDelivery.size())
                   .arg(overflowDelta)
                   .arg(delivery.coalescedRows)
                   .arg(delivery.staleGenerationDrops)
                   .arg(underrunDelta)
                   .arg(
                       static_cast<double>(
                           delivery.lastProducedIntervalNanoseconds) /
                           1'000'000.0,
                       0,
                       'f',
                       3)
                   .arg(
                       static_cast<double>(
                           m_lastWaterfallPublishedIntervalNanoseconds) /
                           1'000'000.0,
                       0,
                       'f',
                       3)
                   .arg(
                       static_cast<double>(
                           m_lastSpectrumSourceAgeNanoseconds) /
                           1'000'000.0,
                       0,
                       'f',
                       3)
                   .arg(
                       static_cast<double>(
                           m_lastWaterfallSourceAgeNanoseconds) /
                           1'000'000.0,
                       0,
                       'f',
                       3)
                   .arg(delivery.sequenceGaps)
                   .arg(delivery.duplicateRows)
                   .arg(delivery.nonMonotonicTimestamps)
                   .arg(metrics.averageProcessingMilliseconds, 0, 'f', 3);
        }
        if (overflowDelta > 0) {
            m_statusText = QStringLiteral(
                "Spectrum performance warning: dropped %1 row(s); effective FFT size %2 was retained")
                               .arg(overflowDelta)
                               .arg(metrics.fftSize);
            publishSnapshot(false);
        } else if (generatedRowsPerSecond + rowRateTolerance <
                       std::min(
                           metrics.targetFramesPerSecond,
                           metrics.achievableFramesPerSecond)) {
            m_statusText = QStringLiteral(
                "Spectrum performance warning: internal cadence %1 rows/s, currently achieving %2 rows/s at FFT size %3")
                               .arg(metrics.targetFramesPerSecond, 0, 'f', 1)
                               .arg(generatedRowsPerSecond, 0, 'f', 1)
                               .arg(metrics.fftSize);
            publishSnapshot(false);
        } else if (metrics.achievableFramesPerSecond + 1.0 <
                   metrics.targetFramesPerSecond) {
            m_statusText = QStringLiteral(
                "Waterfall detail notice: internal cadence %1 rows/s, but the FFT window limits independent rows to %2 rows/s; visible history speed is unchanged")
                               .arg(metrics.targetFramesPerSecond, 0, 'f', 1)
                               .arg(metrics.achievableFramesPerSecond, 0, 'f', 1);
            publishSnapshot(false, false);
        }
        m_lastFftFramesProduced = metrics.framesPublished;
        m_lastFftsExecuted = metrics.fftsExecuted;
        m_lastSpectrumFramesDisplayed = m_spectrumFramesDisplayed;
        m_lastWaterfallRowsConsumed = delivery.rowsConsumed;
        m_lastWaterfallOverflowDrops = totalOverflowDrops;
        m_lastWaterfallDisplayUnderruns = delivery.displayUnderruns;
        m_spectrumMetricsTimer.restart();
    }

    void setSelectedDevice(
        const devices::DeviceDescriptor& device,
        bool capabilitiesConfirmed = false)
    {
        m_selectedCapabilities = device;
        m_selectedCapabilitiesConfirmed = capabilitiesConfirmed;
        QSettings settings;
        m_selectedPpmCorrection =
            savedPpmCorrection(settings, device).value_or(0.0);
        const quint64 safeRate = safeCaptureBandwidth(
            device.capabilities, m_requestedCaptureBandwidth);
        if (safeRate != m_requestedCaptureBandwidth) {
            m_requestedCaptureBandwidth = safeRate;
            QSettings().setValue(captureBandwidthSettingsKey, safeRate);
            m_statusText = QStringLiteral(
                "Saved capture bandwidth is unsupported by this SDR; using safe default %1")
                               .arg(formatCaptureBandwidth(safeRate));
        }
        m_selectedDeviceIdentifier = QString::fromStdString(device.identifier);
        m_deviceState = QStringLiteral("Selected: %1")
                            .arg(QString::fromStdString(device.displayName));
        if (!m_backend) {
            m_backendDescription = QStringLiteral(
                "Selected SDR opens when Start is pressed");
        }
    }

    void clearStoppedSelection()
    {
        m_backend.reset();
        m_lastBackendAudioDroppedSamples = 0;
        m_selectedCapabilities.reset();
        m_selectedCapabilitiesConfirmed = false;
        m_selectedPpmCorrection = 0.0;
        m_selectedDeviceIdentifier.clear();
        m_deviceState = QStringLiteral("No device selected");
        m_backendDescription = QStringLiteral("GNU Radio + SoapySDR hardware runtime");
    }

    void publishSnapshot(
        bool operationSucceeded, bool reportFailureToConsole = true)
    {
        if (!operationSucceeded && reportFailureToConsole &&
            !m_statusText.trimmed().isEmpty()) {
            log(3, QStringLiteral("Receiver"), m_statusText);
        }
        ReceiverRuntimeSnapshot snapshot;
        if (m_backend) {
            snapshot.receiverState = m_backend->state();
            snapshot.receiverLimits = m_backend->limits();
            snapshot.receiverCapabilities = m_backend->capabilities();
            snapshot.squelchOpen = m_backend->squelchOpen();
            snapshot.squelchMeasurementAvailable =
                [this] {
                    const auto measurement =
                        m_backend->squelchSignalStrengthDb();
                    return measurement.has_value() &&
                           std::isfinite(*measurement);
                }();
            snapshot.effectiveSampleRate = m_backend->effectiveSampleRate();
            snapshot.tuningGeneration = m_backend->tuningGeneration();
            const auto spectrumMetrics = m_backend->spectrumProcessingMetrics();
            snapshot.spectrumFftSize = m_spectrumFftSize;
            snapshot.effectiveSpectrumFftSize = spectrumMetrics.fftSize;
            snapshot.spectrumHertzPerBin = spectrumMetrics.hertzPerBin;
            snapshot.effectiveSpectrumFramesPerSecond =
                spectrumMetrics.achievableFramesPerSecond;
            snapshot.effectiveWaterfallRowsPerSecond = std::min(
                waterfallLiveRowsPerSecond,
                spectrumMetrics.achievableFramesPerSecond);
        }
        if (!m_backend) {
            snapshot.receiverState = m_receiverControls.receiverState(
                m_requestedCaptureBandwidth);
            snapshot.receiverState.ppmCorrection =
                m_selectedPpmCorrection;
            snapshot.effectiveSampleRate = m_requestedCaptureBandwidth;
            snapshot.spectrumFftSize = m_spectrumFftSize;
            snapshot.effectiveSpectrumFftSize = m_spectrumFftSize;
            snapshot.spectrumHertzPerBin =
                static_cast<double>(m_requestedCaptureBandwidth) /
                static_cast<double>(m_spectrumFftSize);
            snapshot.effectiveSpectrumFramesPerSecond = std::min(
                static_cast<double>(m_targetSpectrumFramesPerSecond),
                static_cast<double>(m_requestedCaptureBandwidth) /
                static_cast<double>(m_spectrumFftSize));
            snapshot.effectiveWaterfallRowsPerSecond = std::min(
                waterfallLiveRowsPerSecond,
                snapshot.effectiveSpectrumFramesPerSecond);
        }
        if (m_ppmCalibrationRunning &&
            !m_ppmCalibrationInitialReceptionRunning) {
            snapshot.receiverState = m_receiverControls.receiverState(
                m_requestedCaptureBandwidth);
            snapshot.receiverState.ppmCorrection = m_backend
                                                       ? m_backend->state()
                                                             .ppmCorrection
                                                       : m_selectedPpmCorrection;
            snapshot.effectiveSampleRate = m_requestedCaptureBandwidth;
        }
        snapshot.visibleWaterfallHistorySeconds =
            m_visibleWaterfallHistorySeconds;
        snapshot.requestedGainDb = m_requestedGainDb;
        for (const auto& device : m_devices) {
            snapshot.deviceIdentifiers.append(
                QString::fromStdString(device.identifier));
            snapshot.deviceDisplayNames.append(
                QString::fromStdString(device.displayName));
        }
        snapshot.selectedDeviceIdentifier = m_selectedDeviceIdentifier;
        if (m_audioOutput) {
            const auto& audio = m_audioOutput->state();
            for (const auto& device : audio.devices) {
                snapshot.audioDeviceIdentifiers.append(
                    QString::fromStdString(device.identifier));
                snapshot.audioDeviceDisplayNames.append(
                    QString::fromStdString(device.description));
            }
            snapshot.selectedAudioDeviceIdentifier =
                QString::fromStdString(audio.selectedDeviceIdentifier);
            snapshot.audioStatusText = QString::fromStdString(audio.statusText);
            snapshot.audioVolumePercent = audio.volumePercent;
            snapshot.audioMuted = audio.muted;
            snapshot.audioReady = audio.ready;
            snapshot.audioRunning = audio.running;
            snapshot.audioOverflowEvents = audio.overflowEvents;
            snapshot.audioUnderrunEvents = audio.underrunEvents;
        } else if (!m_audioInitializationError.isEmpty()) {
            snapshot.audioStatusText = m_audioInitializationError;
        } else {
            snapshot.audioStatusText = QStringLiteral(
                "Audio output service is unavailable in this build");
        }
        if (m_dsdFme) {
            snapshot.dsdFmeStatusText = m_dsdFme->state().statusText;
            snapshot.decoderRunning =
                m_dsdFme->state().state == platform::DsdFmeState::Running;
        } else if (!m_dsdFmeInitializationError.isEmpty()) {
            snapshot.dsdFmeStatusText = m_dsdFmeInitializationError;
        } else {
            snapshot.dsdFmeStatusText = QStringLiteral(
                "DSD-FME process service is unavailable");
        }
        if (m_recording) {
            const auto recording = m_recording->state();
            snapshot.recordingActive = recording.active;
            snapshot.recordingFailed = recording.failed;
            snapshot.recordingElapsedSeconds = recording.elapsedSeconds;
            snapshot.recordingDroppedFrames = recording.droppedFrames;
            snapshot.recordingStatusText = QString::fromStdString(recording.statusText);
            snapshot.recordingFilePath = QString::fromStdString(recording.filePath.string());
        }
        snapshot.deviceState = m_deviceState;
        snapshot.backendDescription = m_backendDescription;
        snapshot.statusText = m_statusText;
        snapshot.backendReady = m_backend != nullptr || m_selectedCapabilities.has_value();
        snapshot.discoveryAvailable =
            m_startupMode == StartupMode::Hardware && hardwareFactoriesAvailable();
        snapshot.mockMode = m_startupMode == StartupMode::Mock;
        snapshot.automaticPpmCalibrationSupported =
            automaticPpmCalibrationAvailable();
        snapshot.ppmCalibrationRunning = m_ppmCalibrationRunning;
        snapshot.autoSquelchRunning = m_autoSquelchRunning;
        snapshot.ppmCalibrationStatus = m_ppmCalibrationStatus;
        snapshot.ppmCalibrationProgressPercent =
            m_ppmCalibrationProgressPercent;
        snapshot.ppmCalibrationDisplayResetGeneration =
            m_ppmCalibrationDisplayResetGeneration;
        snapshot.operationSucceeded = operationSucceeded;
        snapshot.workerThreadToken = reinterpret_cast<quintptr>(
            QThread::currentThreadId());
        if (m_selectedCapabilities) {
            const auto& capabilities = m_selectedCapabilities->capabilities;
            snapshot.receiverCapabilities.ppmCorrectionSupported =
                capabilities.ppmCorrectionSupported;
            snapshot.receiverCapabilities.automaticPpmCalibrationSupported =
                automaticPpmCalibrationAvailable();
            snapshot.deviceCapabilitySummary =
                capabilitySummary(*m_selectedCapabilities);
            snapshot.deviceFrequencyRanges = capabilities.receiveFrequencyRanges;
            snapshot.deviceSampleRateRanges = capabilities.receiveSampleRateRanges;
            snapshot.captureBandwidthOptions = captureBandwidthOptions(capabilities);
            snapshot.customCaptureBandwidthSupported =
                devices::allowsCustomReceiveSampleRate(capabilities);
            snapshot.minimumGainDb = capabilities.minimumGainDb;
            snapshot.maximumGainDb = capabilities.maximumGainDb;
            snapshot.gainStepDb = capabilities.gainStepDb;
            snapshot.gainSupported = capabilities.gainSupported;
        } else if (m_startupMode == StartupMode::Mock && m_backend) {
            snapshot.deviceCapabilitySummary =
                QStringLiteral("Mock receiver · no physical hardware");
            snapshot.minimumGainDb = snapshot.receiverLimits.minimumGainDb;
            snapshot.maximumGainDb = snapshot.receiverLimits.maximumGainDb;
            snapshot.gainStepDb = 1.0;
            snapshot.gainSupported = true;
            snapshot.captureBandwidthOptions = {
                formatCaptureBandwidth(1'000'000),
                formatCaptureBandwidth(2'000'000),
                formatCaptureBandwidth(2'400'000),
                formatCaptureBandwidth(3'200'000),
            };
            snapshot.customCaptureBandwidthSupported = true;
        }
        emit snapshotChanged(snapshot);
    }

    void configureDsdFmeLogging()
    {
        if (!m_dsdFme) {
            return;
        }
        m_dsdFme->setLogHandler(
            [this](
                platform::DsdFmeLogSeverity severity,
                const QString& message) {
                int mappedSeverity = 1;
                switch (severity) {
                case platform::DsdFmeLogSeverity::Debug:
                    mappedSeverity = 0;
                    break;
                case platform::DsdFmeLogSeverity::Info:
                    mappedSeverity = 1;
                    break;
                case platform::DsdFmeLogSeverity::Warning:
                    mappedSeverity = 2;
                    break;
                case platform::DsdFmeLogSeverity::Error:
                    mappedSeverity = 3;
                    break;
                }
                log(mappedSeverity, QStringLiteral("DSD-FME"), message);
            });
    }

    void log(int severity, const QString& source, const QString& message)
    {
        if (m_applicationLogHandler) {
            m_applicationLogHandler(severity, source, message);
        }
    }

    StartupMode m_startupMode;
    Factories m_factories;
    bool m_verboseAudioMetrics = false;
    QTimer* m_pollTimer = nullptr;
    QTimer* m_audioServiceTimer = nullptr;
    QTimer* m_audioDeviceTimer = nullptr;
    QTimer* m_waterfallTimer = nullptr;
    QTimer* m_centerFrequencyTimer = nullptr;
    QTimer* m_ppmCalibrationTimer = nullptr;
    double m_waterfallTimerFractionalMilliseconds = 0.0;
    ApplicationLogHandler m_applicationLogHandler;
    std::unique_ptr<radio::ReceiverBackend> m_backend;
    std::optional<bool> m_lastPublishedSquelchOpen;
    std::optional<bool> m_lastPublishedSquelchMeasurement;
    bool m_autoSquelchRunning = false;
    QElapsedTimer m_autoSquelchElapsed;
    std::vector<double> m_autoSquelchSamples;
    std::unique_ptr<platform::AudioOutputService> m_audioOutput;
    std::unique_ptr<platform::WavRecordingService> m_recording =
        std::make_unique<platform::WavRecordingService>();
    std::unique_ptr<platform::DsdFmeProcessService> m_dsdFme;
    std::vector<devices::DeviceDescriptor> m_devices;
    std::optional<devices::DeviceDescriptor> m_selectedCapabilities;
    bool m_selectedCapabilitiesConfirmed = false;
    QString m_selectedDeviceIdentifier;
    double m_selectedPpmCorrection = 0.0;
    QString m_deviceState = QStringLiteral("No device selected");
    QString m_backendDescription = QStringLiteral("No receiver backend");
    QString m_statusText = QStringLiteral("Receiver runtime is initializing");
    quint64 m_requestedCaptureBandwidth = conservativeCaptureBandwidth;
    std::optional<quint64> m_pendingCenterFrequency;
    std::array<quint64, 6> m_filterWidths{
        10'000, 12'500, 180'000, 2'400, 2'400, 12'500};
    ReceiverControlSettings m_receiverControls;
    std::optional<double> m_savedGainDb;
    double m_requestedGainDb = 20.0;
    QString m_audioInitializationError;
    QString m_dsdFmeInitializationError;
    std::uint64_t m_lastBackendAudioDroppedSamples = 0;
    std::uint64_t m_lastDecoderInputDroppedSamples = 0;
    std::uint64_t m_lastDecoderAudioUnderruns = 0;
    std::uint64_t m_lastDecoderPlatformAudioUnderruns = 0;
    bool m_decoderAudioDiagnosticsActive = false;
    std::uint64_t m_audioTransferredSamples = 0;
    std::uint64_t m_audioServicePasses = 0;
    std::uint64_t m_lastMetricsProducedSamples = 0;
    std::uint64_t m_lastMetricsTransferredSamples = 0;
    std::uint64_t m_lastMetricsServicePasses = 0;
    std::uint64_t m_lastMetricsWrittenSamples = 0;
    QElapsedTimer m_audioMetricsTimer;
    std::uint32_t m_targetSpectrumFramesPerSecond =
        dsp::defaultSpectrumDisplayFramesPerSecond;
    double m_visibleWaterfallHistorySeconds =
        defaultVisibleWaterfallHistorySeconds;
    std::size_t m_spectrumFftSize = 4'096;
    WaterfallFrameDelivery m_waterfallDelivery;
    std::uint64_t m_spectrumFramesDisplayed = 0;
    std::uint64_t m_lastFftFramesProduced = 0;
    std::uint64_t m_lastFftsExecuted = 0;
    std::uint64_t m_lastSpectrumFramesDisplayed = 0;
    std::uint64_t m_lastWaterfallRowsConsumed = 0;
    std::uint64_t m_lastWaterfallOverflowDrops = 0;
    std::uint64_t m_lastWaterfallDisplayUnderruns = 0;
    std::uint64_t m_lastWaterfallPublishedIntervalNanoseconds = 0;
    std::uint64_t m_lastSpectrumSourceAgeNanoseconds = 0;
    std::uint64_t m_lastWaterfallSourceAgeNanoseconds = 0;
    QElapsedTimer m_waterfallPublishIntervalTimer;
    QElapsedTimer m_spectrumMetricsTimer;
    std::vector<std::uint8_t> m_ppmCalibrationReadBuffer =
        std::vector<std::uint8_t>(ppmCalibrationReadBufferBytes);
    std::unique_ptr<devices::PpmCalibrationEstimator>
        m_ppmCalibrationEstimator;
    bool m_ppmCalibrationRunning = false;
    bool m_ppmCalibrationInitialReceptionRunning = false;
    bool m_ppmCalibrationCreatedBackend = false;
    bool m_ppmCalibrationPreviousAudioMuted = false;
    double m_ppmCalibrationPreviousCorrection = 0.0;
    std::size_t m_ppmCalibrationLastLoggedWindowCount = 0;
    QString m_ppmCalibrationStatus = QStringLiteral("idle");
    int m_ppmCalibrationProgressPercent = 0;
    quint64 m_ppmCalibrationDisplayResetGeneration = 0;
};

ReceiverRuntime::ReceiverRuntime(
    StartupMode startupMode,
    QObject* parent,
    bool verboseAudioMetrics)
    : ReceiverRuntime(
          startupMode, Factories{}, parent, verboseAudioMetrics)
{
}

ReceiverRuntime::ReceiverRuntime(
    StartupMode startupMode,
    Factories factories,
    QObject* parent,
    bool verboseAudioMetrics)
    : QObject(parent)
    , m_startupMode(startupMode)
    , m_worker(new Worker(
          startupMode,
          std::move(factories),
          verboseAudioMetrics))
{
    qRegisterMetaType<ReceiverRuntimeSnapshot>();
    m_worker->moveToThread(&m_workerThread);

    connect(
        &m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(
        this,
        &ReceiverRuntime::initializeRequested,
        m_worker,
        &Worker::initialize,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::refreshDevicesRequested,
        m_worker,
        &Worker::refreshDevices,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::selectDeviceRequested,
        m_worker,
        &Worker::selectDevice,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::clearDeviceSelectionRequested,
        m_worker,
        &Worker::clearDeviceSelection,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::selectAudioDeviceRequested,
        m_worker,
        &Worker::selectAudioDevice,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::setAudioVolumeRequested,
        m_worker,
        &Worker::setAudioVolume,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::setAudioMutedRequested,
        m_worker,
        &Worker::setAudioMuted,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::startAudioRecordingRequested,
        m_worker,
        &Worker::startAudioRecording,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::stopAudioRecordingRequested,
        m_worker,
        &Worker::stopAudioRecording,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::setDsdFmeBinaryPathRequested,
        m_worker,
        &Worker::setDsdFmeBinaryPath,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::startReceptionRequested,
        m_worker,
        &Worker::startReception,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::stopReceptionRequested,
        m_worker,
        &Worker::stopReception,
        Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setCenterFrequencyRequested, m_worker, &Worker::requestCenterFrequency, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setListeningFrequencyRequested, m_worker, &Worker::setListeningFrequency, Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::setScannerListeningFrequencyRequested,
        m_worker,
        &Worker::setScannerListeningFrequency,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::setScannerCenterFrequencyRequested,
        m_worker,
        &Worker::setScannerCenterFrequency,
        Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::shiftCenterFrequencyRequested, m_worker, &Worker::shiftCenterFrequency, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setSampleRateRequested, m_worker, &Worker::setSampleRate, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setSpectrumFftSizeRequested, m_worker, &Worker::setSpectrumFftSize, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setVisibleWaterfallHistorySecondsRequested, m_worker, &Worker::setVisibleWaterfallHistorySeconds, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setFilterWidthRequested, m_worker, &Worker::setFilterWidth, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setGainRequested, m_worker, &Worker::setGain, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setPpmCorrectionRequested, m_worker, &Worker::setPpmCorrection, Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::startAutomaticPpmCalibrationRequested,
        m_worker,
        &Worker::startAutomaticPpmCalibration,
        Qt::QueuedConnection);
    connect(
        this,
        &ReceiverRuntime::cancelAutomaticPpmCalibrationRequested,
        m_worker,
        &Worker::cancelAutomaticPpmCalibration,
        Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setDemodulationModeRequested, m_worker, &Worker::setDemodulationMode, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::setSquelchLevelRequested, m_worker, &Worker::setSquelchLevel, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::enableManualSquelchRequested, m_worker, &Worker::enableManualSquelch, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::autoSquelchRequested, m_worker, &Worker::autoSquelch, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::disableSquelchRequested, m_worker, &Worker::disableSquelch, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::applyBookmarkRequested, m_worker, &Worker::applyBookmark, Qt::QueuedConnection);
    connect(this, &ReceiverRuntime::applyScannerBookmarkRequested, m_worker,
        &Worker::applyScannerBookmark, Qt::QueuedConnection);

    connect(m_worker, &Worker::snapshotChanged, this, &ReceiverRuntime::snapshotChanged);
    connect(
        m_worker,
        &Worker::centerFrequencyRequestCompleted,
        this,
        &ReceiverRuntime::centerFrequencyRequestCompleted);
    connect(
        m_worker,
        &Worker::scannerListeningFrequencyRequestCompleted,
        this,
        &ReceiverRuntime::finishScannerListeningFrequencyRequest);
    connect(
        m_worker,
        &Worker::scannerCenterFrequencyRequestCompleted,
        this,
        &ReceiverRuntime::scannerCenterFrequencyChanged);
    connect(
        m_worker,
        &Worker::spectrumFrameReady,
        this,
        [this](
            const QVector<float>& normalizedMagnitudes,
            quint64 centerFrequency,
            quint64 sampleRate,
            quint64 fftSize,
            quint64 sequence,
            quint64 timestampNanoseconds,
            quint64 tuningGeneration) {
            enqueueDisplayFrame(
                false,
                normalizedMagnitudes,
                centerFrequency,
                sampleRate,
                fftSize,
                sequence,
                timestampNanoseconds,
                tuningGeneration);
        },
        Qt::DirectConnection);
    connect(
        m_worker,
        &Worker::waterfallFrameReady,
        this,
        [this](
            const QVector<float>& normalizedMagnitudes,
            quint64 centerFrequency,
            quint64 sampleRate,
            quint64 fftSize,
            quint64 sequence,
            quint64 timestampNanoseconds,
            quint64 tuningGeneration) {
            enqueueDisplayFrame(
                true,
                normalizedMagnitudes,
                centerFrequency,
                sampleRate,
                fftSize,
                sequence,
                timestampNanoseconds,
                tuningGeneration);
        },
        Qt::DirectConnection);
    m_workerThread.setObjectName(QStringLiteral("SDR receiver runtime"));
    m_workerThread.start();
}

ReceiverRuntime::~ReceiverRuntime()
{
    shutdown();
}

void ReceiverRuntime::enqueueDisplayFrame(
    bool waterfall,
    const QVector<float>& normalizedMagnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 fftSize,
    quint64 sequence,
    quint64 timestampNanoseconds,
    quint64 tuningGeneration)
{
    bool scheduleDispatch = false;
    {
        std::lock_guard lock(m_displayFrameMutex);
        auto& pending =
            waterfall ? m_pendingWaterfallFrame : m_pendingSpectrumFrame;
        bool& scheduled =
            waterfall ? m_waterfallDispatchScheduled
                      : m_spectrumDispatchScheduled;
        pending = PendingDisplayFrame{
            .normalizedMagnitudes = normalizedMagnitudes,
            .centerFrequency = centerFrequency,
            .sampleRate = sampleRate,
            .fftSize = fftSize,
            .sequence = sequence,
            .timestampNanoseconds = timestampNanoseconds,
            .tuningGeneration = tuningGeneration,
        };
        if (!scheduled) {
            scheduled = true;
            scheduleDispatch = true;
        }
    }
    if (scheduleDispatch) {
        QMetaObject::invokeMethod(
            this,
            [this, waterfall] {
                publishPendingDisplayFrame(waterfall);
            },
            Qt::QueuedConnection);
    }
}

void ReceiverRuntime::publishPendingDisplayFrame(bool waterfall)
{
    std::optional<PendingDisplayFrame> frame;
    {
        std::lock_guard lock(m_displayFrameMutex);
        auto& pending =
            waterfall ? m_pendingWaterfallFrame : m_pendingSpectrumFrame;
        frame = std::move(pending);
        pending.reset();
    }
    if (frame.has_value()) {
        if (waterfall) {
            emit waterfallFrameReady(
                frame->normalizedMagnitudes,
                frame->centerFrequency,
                frame->sampleRate,
                frame->fftSize,
                frame->sequence,
                frame->timestampNanoseconds,
                frame->tuningGeneration);
        } else {
            emit spectrumFrameReady(
                frame->normalizedMagnitudes,
                frame->centerFrequency,
                frame->sampleRate,
                frame->fftSize,
                frame->sequence,
                frame->timestampNanoseconds,
                frame->tuningGeneration);
        }
    }

    bool scheduleDispatch = false;
    {
        std::lock_guard lock(m_displayFrameMutex);
        auto& pending =
            waterfall ? m_pendingWaterfallFrame : m_pendingSpectrumFrame;
        bool& scheduled =
            waterfall ? m_waterfallDispatchScheduled
                      : m_spectrumDispatchScheduled;
        if (pending.has_value()) {
            scheduleDispatch = true;
        } else {
            scheduled = false;
        }
    }
    if (scheduleDispatch) {
        QMetaObject::invokeMethod(
            this,
            [this, waterfall] {
                publishPendingDisplayFrame(waterfall);
            },
            Qt::QueuedConnection);
    }
}

ReceiverRuntime::StartupMode ReceiverRuntime::startupMode() const noexcept
{
    return m_startupMode;
}

bool ReceiverRuntime::workerThreadRunning() const noexcept
{
    return m_workerThread.isRunning();
}

void ReceiverRuntime::setApplicationLogHandler(ApplicationLogHandler handler)
{
    if (!m_worker) {
        return;
    }
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, handler = std::move(handler)]() mutable {
            worker->setApplicationLogHandler(std::move(handler));
        },
        Qt::QueuedConnection);
}

void ReceiverRuntime::start()
{
    if (m_started || !m_workerThread.isRunning()) {
        return;
    }
    m_started = true;
    emit initializeRequested();
}

void ReceiverRuntime::shutdown()
{
    if (!m_workerThread.isRunning()) {
        return;
    }
    if (m_worker) {
        QMetaObject::invokeMethod(
            m_worker, &Worker::shutdown, Qt::BlockingQueuedConnection);
    }
    m_workerThread.quit();
    m_workerThread.wait();
    m_worker = nullptr;
}

void ReceiverRuntime::markPending(const QString& description)
{
    emit operationPending(description);
}

void ReceiverRuntime::refreshDevices()
{
    markPending(QStringLiteral("Refreshing SDR devices…"));
    emit refreshDevicesRequested();
}

void ReceiverRuntime::selectDevice(const QString& identifier)
{
    markPending(QStringLiteral("Opening the selected SDR device…"));
    emit selectDeviceRequested(identifier);
}

void ReceiverRuntime::clearDeviceSelection()
{
    markPending(QStringLiteral("Clearing the SDR device selection…"));
    emit clearDeviceSelectionRequested();
}

void ReceiverRuntime::selectAudioDevice(const QString& identifier)
{
    emit selectAudioDeviceRequested(identifier);
}

void ReceiverRuntime::setAudioVolume(int volumePercent)
{
    emit setAudioVolumeRequested(volumePercent);
}

void ReceiverRuntime::setAudioMuted(bool muted)
{
    emit setAudioMutedRequested(muted);
}

void ReceiverRuntime::startAudioRecording(const QString& directory)
{
    emit startAudioRecordingRequested(directory);
}

void ReceiverRuntime::stopAudioRecording()
{
    emit stopAudioRecordingRequested();
}

void ReceiverRuntime::setDsdFmeBinaryPath(const QString& path)
{
    emit setDsdFmeBinaryPathRequested(path);
}

void ReceiverRuntime::startReception()
{
    markPending(QStringLiteral("Starting reception…"));
    emit startReceptionRequested();
}

void ReceiverRuntime::stopReception()
{
    markPending(QStringLiteral("Stopping reception…"));
    emit stopReceptionRequested();
}

void ReceiverRuntime::setCenterFrequency(quint64 frequency)
{
    markPending(QStringLiteral("Applying center frequency…"));
    emit setCenterFrequencyRequested(frequency);
}

void ReceiverRuntime::setListeningFrequency(quint64 frequency)
{
    markPending(QStringLiteral("Applying listening frequency…"));
    emit setListeningFrequencyRequested(frequency);
}

void ReceiverRuntime::requestScannerListeningFrequency(quint64 frequency)
{
    m_latestScannerListeningFrequency = frequency;
    if (m_scannerListeningFrequencyRequestActive) {
        return;
    }
    m_scannerListeningFrequencyRequestActive = true;
    emit setScannerListeningFrequencyRequested(frequency);
}

void ReceiverRuntime::requestScannerCenterFrequency(quint64 frequency)
{
    emit setScannerCenterFrequencyRequested(frequency);
}

void ReceiverRuntime::cancelScannerListeningFrequencyRequests()
{
    m_latestScannerListeningFrequency.reset();
}

void ReceiverRuntime::finishScannerListeningFrequencyRequest(
    quint64 requestedFrequency,
    quint64 appliedFrequency,
    bool succeeded,
    const QString& message)
{
    emit scannerListeningFrequencyChanged(
        appliedFrequency, succeeded, message);
    if (!succeeded) {
        m_latestScannerListeningFrequency.reset();
        m_scannerListeningFrequencyRequestActive = false;
        return;
    }
    if (m_latestScannerListeningFrequency.has_value() &&
        *m_latestScannerListeningFrequency != requestedFrequency) {
        emit setScannerListeningFrequencyRequested(
            *m_latestScannerListeningFrequency);
        return;
    }
    m_latestScannerListeningFrequency.reset();
    m_scannerListeningFrequencyRequestActive = false;
}

void ReceiverRuntime::shiftCenterFrequency(qint64 requestedStep)
{
    markPending(QStringLiteral("Shifting the receiver passband…"));
    emit shiftCenterFrequencyRequested(requestedStep);
}

void ReceiverRuntime::setSampleRate(quint64 sampleRate)
{
    markPending(QStringLiteral("Changing capture bandwidth…"));
    emit setSampleRateRequested(sampleRate);
}

void ReceiverRuntime::setSpectrumFftSize(quint64 fftSize)
{
    markPending(QStringLiteral("Changing spectrum FFT resolution…"));
    emit setSpectrumFftSizeRequested(fftSize);
}

void ReceiverRuntime::setVisibleWaterfallHistorySeconds(double seconds)
{
    markPending(QStringLiteral("Changing visible waterfall history…"));
    emit setVisibleWaterfallHistorySecondsRequested(seconds);
}

void ReceiverRuntime::setFilterWidth(quint64 filterWidth)
{
    markPending(QStringLiteral("Applying filter width…"));
    emit setFilterWidthRequested(filterWidth);
}

void ReceiverRuntime::setGain(double gainDb)
{
    markPending(QStringLiteral("Applying receiver gain…"));
    emit setGainRequested(gainDb);
}

void ReceiverRuntime::setPpmCorrection(double ppmCorrection)
{
    markPending(QStringLiteral("Applying PPM correction…"));
    emit setPpmCorrectionRequested(ppmCorrection);
}

void ReceiverRuntime::startAutomaticPpmCalibration()
{
    markPending(QStringLiteral("Preparing automatic PPM calibration…"));
    emit startAutomaticPpmCalibrationRequested();
}

void ReceiverRuntime::cancelAutomaticPpmCalibration()
{
    emit cancelAutomaticPpmCalibrationRequested();
}

void ReceiverRuntime::setDemodulationMode(int mode)
{
    markPending(QStringLiteral("Applying demodulation mode…"));
    emit setDemodulationModeRequested(mode);
}

void ReceiverRuntime::setSquelchLevel(double squelchLevelDb)
{
    markPending(QStringLiteral("Applying squelch level…"));
    emit setSquelchLevelRequested(squelchLevelDb);
}

void ReceiverRuntime::enableManualSquelch()
{
    markPending(QStringLiteral("Enabling manual squelch…"));
    emit enableManualSquelchRequested();
}

void ReceiverRuntime::autoSquelch()
{
    markPending(QStringLiteral("Measuring squelch…"));
    emit autoSquelchRequested();
}

void ReceiverRuntime::disableSquelch()
{
    markPending(QStringLiteral("Disabling squelch…"));
    emit disableSquelchRequested();
}

void ReceiverRuntime::applyBookmark(quint64 frequency, double requestedGainDb,
    int demodulationMode, quint64 filterWidth,
    double squelchThresholdDb, bool squelchEnabled, bool applySquelch)
{
    markPending(QStringLiteral("Applying bookmark…"));
    emit applyBookmarkRequested(frequency, requestedGainDb, demodulationMode,
        filterWidth, squelchThresholdDb, squelchEnabled, applySquelch);
}

void ReceiverRuntime::applyScannerBookmark(quint64 frequency,
    double requestedGainDb, int demodulationMode, quint64 filterWidth,
    double squelchThresholdDb, bool squelchEnabled)
{
    emit applyScannerBookmarkRequested(frequency, requestedGainDb,
        demodulationMode, filterWidth, squelchThresholdDb, squelchEnabled);
}

}  // namespace sdr::app

#include "ReceiverRuntime.moc"
