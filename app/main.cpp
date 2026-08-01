// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationModel.hpp"
#include "ReceiverRuntime.hpp"
#include "SpectrumFramePacing.hpp"
#include "project_config.hpp"

#if SDR_RECEIVER_ENABLE_GNURADIO && SDR_RECEIVER_ENABLE_SOAPYSDR
#include "GnuRadioReceiverBackend.hpp"
#include "RecordedAudioBackend.hpp"
#include "SoapyDeviceProvider.hpp"

#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Version.hpp>
#include <gnuradio/constants.h>
#include <gnuradio/sys_paths.h>
#endif

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <sys/utsname.h>

namespace {

class MainWindowCloseFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Close) {
            QCoreApplication::quit();
        }
        return QObject::eventFilter(watched, event);
    }
};

bool hasArgument(int argc, char* argv[], const std::string& requested)
{
    return std::any_of(
        argv + 1,
        argv + argc,
        [&requested](const char* argument) { return argument == requested; });
}

#if SDR_RECEIVER_ENABLE_GNURADIO && SDR_RECEIVER_ENABLE_SOAPYSDR
std::string selectedVmcircbufFactory(const std::filesystem::path& preference)
{
    std::ifstream stream(preference);
    std::string factory;
    std::getline(stream, factory);
    return factory.empty() ? "<not saved; GNU Radio will auto-detect>" : factory;
}

std::string rtlSdrModuleVersion()
{
    for (const auto& module : SoapySDR::listModules()) {
        if (std::filesystem::path(module).filename().string().find(
                "rtlsdrSupport") != std::string::npos) {
            const std::string loadError = SoapySDR::loadModule(module);
            if (!loadError.empty()) {
                return "available; version query failed: " + loadError;
            }
            const std::string version = SoapySDR::getModuleVersion(module);
            return version.empty() ? "available; version not reported" : version;
        }
    }
    return "not installed";
}

int printGnuRadioDiagnostics()
{
#if SDR_RECEIVER_GNURADIO_HAS_PATHS_USERCONF
    const std::filesystem::path userPreferences = gr::paths::userconf();
#else
    const std::filesystem::path userPreferences = gr::userconf_path();
#endif
    const std::filesystem::path vmcircbufPreference =
        userPreferences / "prefs" / "vmcircbuf_default_factory";
    utsname kernel{};

    std::cout << "slopSDR diagnostics\n"
              << "GNU Radio version: " << gr::version() << '\n'
              << "SoapySDR version: " << SoapySDR::getLibVersion() << '\n'
              << "RTL-SDR Soapy module: " << rtlSdrModuleVersion() << '\n';
    if (uname(&kernel) == 0) {
        std::cout << "Linux kernel version: " << kernel.release << '\n';
    } else {
        std::cout << "Linux kernel version: unavailable\n";
    }
    std::cout << "GNU Radio user preference directory: "
              << userPreferences.string() << '\n'
              << "vmcircbuf preference path: "
              << vmcircbufPreference.string() << '\n'
              << "vmcircbuf_default_factory: "
              << selectedVmcircbufFactory(vmcircbufPreference) << '\n';
    return EXIT_SUCCESS;
}
#endif

}  // namespace

int main(int argc, char* argv[])
{
    QGuiApplication::setApplicationName(sdr_receiver::application_name);
    QGuiApplication::setApplicationVersion(sdr_receiver::application_version);
    QGuiApplication::setOrganizationName(
        QString::fromLatin1(sdr_receiver::application_name));

    if (hasArgument(argc, argv, "--diagnose-gnuradio")) {
        QCoreApplication application(argc, argv);
#if SDR_RECEIVER_ENABLE_GNURADIO && SDR_RECEIVER_ENABLE_SOAPYSDR
        return printGnuRadioDiagnostics();
#else
        std::cerr << "GNU Radio diagnostics require the desktop hardware build\n";
        return EXIT_FAILURE;
#endif
    }

    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(true);
    QCommandLineParser commandLine;
    commandLine.setApplicationDescription(QStringLiteral(
        "slopSDR desktop software-defined radio receiver"));
    commandLine.addHelpOption();
    commandLine.addVersionOption();
    const QCommandLineOption mockOption(
        QStringLiteral("mock"),
        QStringLiteral("Run deliberately with the mock receiver and no hardware."));
    commandLine.addOption(mockOption);
    const QCommandLineOption verboseOption(
        QStringLiteral("verbose"),
        QStringLiteral(
            "Print once-per-second runtime audio, spectrum, and waterfall metrics."));
    commandLine.addOption(verboseOption);
    const QCommandLineOption spectrumFftSizeOption(
        QStringLiteral("spectrum-fft-size"),
        QStringLiteral("Set initial spectrum FFT bins (1024 through 262144 powers of two)."),
        QStringLiteral("bins"),
        QStringLiteral("4096"));
    commandLine.addOption(spectrumFftSizeOption);
    commandLine.addOption(QCommandLineOption(
        QStringLiteral("diagnose-gnuradio"),
        QStringLiteral(
            "Report GNU Radio, SoapySDR, kernel, and vmcircbuf diagnostics without changing configuration.")));
    commandLine.process(application);

    const auto startupMode = commandLine.isSet(mockOption)
                                 ? sdr::app::ReceiverRuntime::StartupMode::Mock
                                 : sdr::app::ReceiverRuntime::StartupMode::Hardware;
    sdr::app::ReceiverRuntime::Factories factories{};
    factories.createAudioOutputService = [] {
        return sdr::platform::makeQtAudioOutputService();
    };
    bool fftSizeValid = false;
    const auto spectrumFftSize = commandLine.value(
        spectrumFftSizeOption).toULongLong(&fftSizeValid);
    if (!fftSizeValid ||
        !sdr::dsp::isSupportedSpectrumFftSize(
            static_cast<std::size_t>(spectrumFftSize))) {
        qCritical().noquote()
            << QStringLiteral("--spectrum-fft-size must be one of 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, or 262144");
        return 1;
    }
    factories.initialSpectrumFftSize =
        static_cast<std::size_t>(spectrumFftSize);
#if SDR_RECEIVER_ENABLE_GNURADIO && SDR_RECEIVER_ENABLE_SOAPYSDR
    const sdr::dsp::SpectrumDisplayConfiguration spectrumConfiguration{
        .fftSize = static_cast<std::size_t>(spectrumFftSize),
    };
    const bool verboseDspMetrics = commandLine.isSet(verboseOption);
    factories.createDeviceProvider = [verboseDspMetrics] {
        return std::make_unique<sdr::devices::SoapyDeviceProvider>(
            verboseDspMetrics);
    };
    factories.createHardwareBackend = [spectrumConfiguration, verboseDspMetrics](
                                          std::unique_ptr<
                                              sdr::devices::DeviceController>
                                              selectedDevice) {
        return std::make_unique<sdr::dsp::GnuRadioReceiverBackend>(
            std::move(selectedDevice),
            spectrumConfiguration,
            verboseDspMetrics);
    };
    factories.createRecordedBackend = [spectrumConfiguration, verboseDspMetrics](
                                         sdr::radio::RecordedIqSourceConfiguration source) {
        return std::make_unique<sdr::dsp::GnuRadioReceiverBackend>(
            std::move(source), spectrumConfiguration, verboseDspMetrics);
    };
    factories.createRecordedAudioBackend = [spectrumConfiguration](
                                                 const std::string& path) {
        return std::make_unique<sdr::dsp::RecordedAudioBackend>(
            std::filesystem::path(path), spectrumConfiguration);
    };
#endif
    sdr::app::ReceiverRuntime runtime(
        startupMode,
        std::move(factories),
        nullptr,
        commandLine.isSet(verboseOption));
    ApplicationModel applicationModel(
        runtime, nullptr, commandLine.isSet(verboseOption));
    QQmlApplicationEngine engine;

    engine.setInitialProperties({
        {QStringLiteral("applicationModel"),
         QVariant::fromValue<QObject*>(&applicationModel)},
        {QStringLiteral("applicationVersion"),
         QString::fromLatin1(sdr_receiver::application_version)},
        {QStringLiteral("applicationReleaseDate"),
         QString::fromLatin1(sdr_receiver::application_release_date)},
    });
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/SDRReceiver/Main.qml")));

    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    auto* mainWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (!mainWindow) {
        return EXIT_FAILURE;
    }
    MainWindowCloseFilter mainWindowCloseFilter;
    mainWindow->installEventFilter(&mainWindowCloseFilter);

    QObject::connect(
        &application,
        &QCoreApplication::aboutToQuit,
        &runtime,
        &sdr::app::ReceiverRuntime::shutdown);
    QObject::connect(
        &application,
        &QGuiApplication::lastWindowClosed,
        &application,
        &QCoreApplication::quit);
    runtime.start();

    return application.exec();
}
