// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationModel.hpp"
#include "DeviceController.hpp"
#include "GnuRadioReceiverBackend.hpp"
#include "ReceiverRuntime.hpp"
#include "RtlSdrCapabilities.hpp"
#include "SoapyDeviceProvider.hpp"

#include <QSettings>
#include <QtTest>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class HardwareReceiverSmokeTest final : public QObject
{
    Q_OBJECT

private slots:
    void receivesFromExplicitlyNamedDevice();
    void streamsContinuousRtlSdrTestCounterAfterStartup();
    void completesAutomaticPpmCalibrationThroughRuntime();
};

void HardwareReceiverSmokeTest::receivesFromExplicitlyNamedDevice()
{
    const QByteArray requestedIdentifier = qgetenv("SDR_TEST_DEVICE_ID");
    if (requestedIdentifier.isEmpty()) {
        QSKIP("Set SDR_TEST_DEVICE_ID to opt into an explicitly selected hardware test");
    }

    auto controller = std::make_unique<sdr::devices::DeviceController>(
        std::make_unique<sdr::devices::SoapyDeviceProvider>());
    const auto discovery = controller->discover();
    QVERIFY2(discovery.succeeded(), discovery.message.c_str());
    const auto selection = controller->selectDevice(
        requestedIdentifier.toStdString());
    QVERIFY2(selection.succeeded(), selection.message.c_str());
    const auto& openedDevice = *controller->selectedDevice();
    if (sdr::devices::isRtlSdrDriver(openedDevice.driver)) {
        QVERIFY(openedDevice.capabilities.ppmCorrectionSupported);
        QVERIFY(openedDevice.capabilities.rtlSdrTestModeSupported);
    }

    constexpr std::array<std::uint64_t, 4> commonRates{
        1'000'000,
        1'500'000,
        2'000'000,
        2'400'000,
    };
    std::vector<std::uint64_t> rates;
    for (const auto rate : commonRates) {
        if (sdr::devices::supportsReceiveSampleRate(
                controller->selectedDevice()->capabilities, rate)) {
            rates.push_back(rate);
        }
    }
    if (rates.empty()) {
        QSKIP("The selected SDR reports none of the hardware smoke-test rates");
    }

    sdr::dsp::GnuRadioReceiverBackend receiver(std::move(controller));
    for (const auto rate : rates) {
        const auto configured = receiver.setSampleRate(rate);
        QVERIFY2(configured.succeeded(), configured.message.c_str());
        const auto start = receiver.startReception();
        QVERIFY2(start.succeeded(), start.message.c_str());

        std::optional<sdr::radio::SpectrumFrame> frame;
        for (int attempt = 0; attempt < 100 && !frame.has_value(); ++attempt) {
            QTest::qWait(10);
            if (const auto runtimeError = receiver.takeRuntimeError()) {
                QFAIL(runtimeError->message.c_str());
            }
            frame = receiver.takeLatestSpectrumFrame();
        }
        QVERIFY(frame.has_value());
        QCOMPARE(frame->sampleRate, receiver.effectiveSampleRate());
        const auto stop = receiver.stopReception();
        QVERIFY2(stop.succeeded(), stop.message.c_str());
        QVERIFY(!receiver.state().running);
    }
}

void HardwareReceiverSmokeTest::
    streamsContinuousRtlSdrTestCounterAfterStartup()
{
    const QByteArray requestedIdentifier = qgetenv("SDR_TEST_DEVICE_ID");
    if (requestedIdentifier.isEmpty()) {
        QSKIP("Set SDR_TEST_DEVICE_ID to opt into an explicitly selected hardware test");
    }

    sdr::devices::DeviceController controller(
        std::make_unique<sdr::devices::SoapyDeviceProvider>());
    const auto discovery = controller.discover();
    QVERIFY2(discovery.succeeded(), discovery.message.c_str());
    const auto selection =
        controller.selectDevice(requestedIdentifier.toStdString());
    QVERIFY2(selection.succeeded(), selection.message.c_str());
    if (!controller.selectedDevice()->capabilities.rtlSdrTestModeSupported) {
        QSKIP("The selected SDR does not expose RTL-SDR test mode");
    }

    const auto sampleRate = controller.setSampleRate(2'400'000);
    QVERIFY2(sampleRate.succeeded(), sampleRate.message.c_str());
    const auto start = controller.startRtlSdrTestStream();
    QVERIFY2(start.succeeded(), start.message.c_str());

    std::vector<std::uint8_t> bytes(262'144);
    std::optional<std::uint8_t> previous;
    int buffersRead = 0;
    std::size_t totalDiscontinuities = 0;
    while (buffersRead < 120) {
        const auto read = controller.readRtlSdrTestBytes(
            bytes, std::chrono::milliseconds(100));
        QVERIFY2(read.succeeded(), read.message.c_str());
        QVERIFY2(!read.droppedData, read.message.c_str());
        if (read.status != sdr::devices::DeviceReadStatus::Samples) {
            continue;
        }
        ++buffersRead;
        if (buffersRead == 1) {
            previous = bytes[read.byteCount - 1];
            continue;
        }
        for (std::size_t index = 0; index < read.byteCount; ++index) {
            if (previous.has_value() &&
                bytes[index] !=
                    static_cast<std::uint8_t>(*previous + 1U)) {
                ++totalDiscontinuities;
            }
            previous = bytes[index];
        }
    }
    QCOMPARE(totalDiscontinuities, std::size_t{0});

    const auto stop = controller.stopRtlSdrTestStream();
    QVERIFY2(stop.succeeded(), stop.message.c_str());
}

void HardwareReceiverSmokeTest::completesAutomaticPpmCalibrationThroughRuntime()
{
    const QByteArray requestedIdentifier = qgetenv("SDR_TEST_DEVICE_ID");
    if (requestedIdentifier.isEmpty()) {
        QSKIP("Set SDR_TEST_DEVICE_ID to opt into an explicitly selected hardware test");
    }

    QCoreApplication::setApplicationName(QStringLiteral("slopSDR"));
    QCoreApplication::setOrganizationName(QStringLiteral("slopSDR"));
    sdr::app::ReceiverRuntime::Factories factories{
        .createDeviceProvider = [] {
            return std::make_unique<sdr::devices::SoapyDeviceProvider>();
        },
        .createHardwareBackend =
            [](std::unique_ptr<sdr::devices::DeviceController> device) {
                return std::make_unique<sdr::dsp::GnuRadioReceiverBackend>(
                    std::move(device));
            },
        .createRecordedBackend = {},
        .createAudioOutputService = {},
        .createDsdFmeProcessService = {},
        .monotonicClock = {},
        .initialSpectrumFftSize = 4'096,
    };
    sdr::app::ReceiverRuntime runtime(
        sdr::app::ReceiverRuntime::StartupMode::Hardware,
        std::move(factories));
    ApplicationModel model(runtime);
    const auto waitUntil = [](const auto& predicate, int timeoutMilliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 20);
            QTest::qWait(10);
        }
        return predicate();
    };

    runtime.start();
    QVERIFY(waitUntil(
        [&model] {
            return model.selectedDeviceIndex() >= 0 &&
                   model.automaticPpmCalibrationSupported();
        },
        10'000));
    QVERIFY(!model.receiverRunning());
    model.startAutomaticPpmCalibration();
    const bool calibrationCompleted =
        waitUntil(
            [&model] {
                return !model.ppmCalibrationRunning() &&
                       model.ppmCalibrationStatus() ==
                           QStringLiteral("completed");
            },
            45'000);
    qInfo().noquote() << model.applicationLog()->copyAllText();
    QVERIFY2(
        calibrationCompleted,
        qPrintable(
            model.ppmCalibrationStatus() + QStringLiteral(": ") +
            model.statusText()));
    QVERIFY(!model.receiverRunning());
    qInfo() << "Runtime applied and saved RTL-SDR correction"
            << model.ppmCorrection() << "PPM";

    const QString settingsKey =
        QStringLiteral("receiver/ppmByDevice/") +
        QString::fromLatin1(requestedIdentifier.toHex());
    QCOMPARE(
        QSettings().value(settingsKey).toDouble(),
        model.ppmCorrection());
    runtime.shutdown();
}

QTEST_GUILESS_MAIN(HardwareReceiverSmokeTest)

#include "HardwareReceiverSmokeTest.moc"
