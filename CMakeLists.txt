# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 vibesnake

cmake_minimum_required(VERSION 3.22)

project(
    slopsdr
    VERSION 0.10.4
    DESCRIPTION "slopSDR software-defined radio receiver"
    LANGUAGES CXX
)

set(SDR_RECEIVER_VERSION_SUFFIX "")
set(SDR_RECEIVER_RELEASE_DATE "2026-07-30")

include(GNUInstallDirs)

option(BUILD_TESTING "Build the test suite" ON)
option(ENABLE_GNURADIO "Enable GNU Radio integration" OFF)
option(ENABLE_SOAPYSDR "Enable SoapySDR integration" OFF)
option(BUILD_DESKTOP_APP "Build the complete desktop hardware application" OFF)
option(
    ENABLE_HARDWARE_TESTING
    "Build opt-in tests that access explicitly selected SDR hardware"
    OFF
)
option(
    ENABLE_AUDIO_HARDWARE_TESTING
    "Build an opt-in Qt audio-output smoke test"
    OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(
    Qt6 6.2
    REQUIRED COMPONENTS
        Core
        Gui
        Multimedia
        Qml
        Quick
        QuickControls2
        QuickDialogs2
)

message(STATUS "GNU Radio integration: ${ENABLE_GNURADIO}")
message(STATUS "SoapySDR integration: ${ENABLE_SOAPYSDR}")
message(STATUS "Desktop hardware application: ${BUILD_DESKTOP_APP}")

if(BUILD_DESKTOP_APP AND (NOT ENABLE_GNURADIO OR NOT ENABLE_SOAPYSDR))
    message(
        FATAL_ERROR
        "BUILD_DESKTOP_APP requires ENABLE_GNURADIO=ON and ENABLE_SOAPYSDR=ON"
    )
endif()

if(ENABLE_GNURADIO)
    cmake_policy(PUSH)
    if(POLICY CMP0148)
        cmake_policy(SET CMP0148 OLD)
    endif()
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 OLD)
    endif()
    find_package(
        Gnuradio
        REQUIRED COMPONENTS
            analog
            blocks
            fft
            filter
    )
    cmake_policy(POP)
endif()

if(ENABLE_SOAPYSDR)
    find_package(SoapySDR CONFIG REQUIRED)
endif()

set(SDR_RECEIVER_ENABLE_GNURADIO "${ENABLE_GNURADIO}")
set(SDR_RECEIVER_ENABLE_SOAPYSDR "${ENABLE_SOAPYSDR}")
set(SDR_RECEIVER_BUILD_DESKTOP_APP "${BUILD_DESKTOP_APP}")
configure_file(
    cmake/project_config.hpp.in
    generated/project_config.hpp
    @ONLY
)

include(cmake/ProjectWarnings.cmake)

add_library(
    sdr_radio_domain STATIC
    radio/FrequencyMapping.cpp
    radio/FrequencyMapping.hpp
    radio/DemodulatorRegistry.cpp
    radio/DemodulatorRegistry.hpp
    radio/AudioSampleBuffer.cpp
    radio/AudioSampleBuffer.hpp
    radio/MockReceiverBackend.cpp
    radio/MockReceiverBackend.hpp
    radio/ReceiverBackend.hpp
    radio/ReceiverStateModel.cpp
    radio/ReceiverStateModel.hpp
    radio/SpectrumFrame.cpp
    radio/SpectrumFrame.hpp
)
target_include_directories(sdr_radio_domain PUBLIC radio)
set_target_properties(sdr_radio_domain PROPERTIES AUTOMOC OFF)
sdr_enable_warnings(sdr_radio_domain)

add_library(
    sdr_spectrum_data STATIC
    dsp/FftFrameProcessor.cpp
    dsp/FftFrameProcessor.hpp
    dsp/SpectrumFramePacing.cpp
    dsp/SpectrumFramePacing.hpp
    dsp/SpectrumWindow.cpp
    dsp/SpectrumWindow.hpp
)
target_include_directories(sdr_spectrum_data PUBLIC dsp)
target_link_libraries(sdr_spectrum_data PUBLIC sdr_radio_domain)
set_target_properties(sdr_spectrum_data PROPERTIES AUTOMOC OFF)
sdr_enable_warnings(sdr_spectrum_data)

add_library(
    sdr_audio_dsp_support STATIC
    dsp/AudioDspPlan.cpp
    dsp/AudioDspPlan.hpp
)
target_include_directories(sdr_audio_dsp_support PUBLIC dsp)
target_link_libraries(sdr_audio_dsp_support PUBLIC sdr_radio_domain)
set_target_properties(sdr_audio_dsp_support PROPERTIES AUTOMOC OFF)
sdr_enable_warnings(sdr_audio_dsp_support)

add_library(
    sdr_platform_audio STATIC
    platform/AudioOutputService.cpp
    platform/AudioOutputService.hpp
    platform/DsdFmeProcessService.cpp
    platform/DsdFmeProcessService.hpp
    platform/WavRecordingService.cpp
    platform/WavRecordingService.hpp
    platform/QtAudioSinkBackend.cpp
)
target_include_directories(sdr_platform_audio PUBLIC platform)
target_link_libraries(
    sdr_platform_audio
    PUBLIC sdr_radio_domain
    PRIVATE Qt6::Core Qt6::Multimedia
)
set_target_properties(sdr_platform_audio PROPERTIES AUTOMOC OFF)
sdr_enable_warnings(sdr_platform_audio)

add_library(
    sdr_platform_persistence STATIC
    platform/BookmarkJsonStore.cpp
    platform/BookmarkJsonStore.hpp
)
target_include_directories(sdr_platform_persistence PUBLIC platform)
target_link_libraries(sdr_platform_persistence PUBLIC Qt6::Core)
sdr_enable_warnings(sdr_platform_persistence)

add_library(
    sdr_device_domain STATIC
    devices/DeviceAccess.hpp
    devices/DeviceController.cpp
    devices/DeviceController.hpp
    devices/PpmCalibration.cpp
    devices/PpmCalibration.hpp
    devices/RtlSdrCapabilities.cpp
    devices/RtlSdrCapabilities.hpp
)
target_include_directories(sdr_device_domain PUBLIC devices)
target_link_libraries(sdr_device_domain PUBLIC sdr_radio_domain)
set_target_properties(sdr_device_domain PROPERTIES AUTOMOC OFF)
sdr_enable_warnings(sdr_device_domain)

if(ENABLE_SOAPYSDR)
    add_library(
        sdr_soapy_device_provider STATIC
        devices/SoapyDeviceProvider.cpp
        devices/SoapyDeviceProvider.hpp
    )
    target_include_directories(sdr_soapy_device_provider PUBLIC devices)
    target_link_libraries(
        sdr_soapy_device_provider
        PUBLIC
            sdr_device_domain
        PRIVATE
            SoapySDR
    )
    set_target_properties(sdr_soapy_device_provider PROPERTIES AUTOMOC OFF)
    sdr_enable_warnings(sdr_soapy_device_provider)
endif()

if(ENABLE_GNURADIO)
    add_library(
        sdr_gnuradio_backend STATIC
        dsp/FlowgraphLifecycle.cpp
        dsp/FlowgraphLifecycle.hpp
        dsp/GnuRadioReceiverBackend.cpp
        dsp/GnuRadioReceiverBackend.hpp
    )
    target_include_directories(sdr_gnuradio_backend PUBLIC dsp)
    target_link_libraries(
        sdr_gnuradio_backend
        PUBLIC
            sdr_device_domain
            sdr_audio_dsp_support
            sdr_radio_domain
            sdr_spectrum_data
        PRIVATE
            gnuradio::gnuradio-analog
            gnuradio::gnuradio-blocks
            gnuradio::gnuradio-fft
            gnuradio::gnuradio-filter
            gnuradio::gnuradio-runtime
    )
    set_target_properties(sdr_gnuradio_backend PROPERTIES AUTOMOC OFF)
    sdr_enable_warnings(sdr_gnuradio_backend)
endif()

add_library(
    sdr_application_model STATIC
    app/ApplicationModel.cpp
    app/ApplicationModel.hpp
    app/ApplicationLogModel.cpp
    app/ApplicationLogModel.hpp
    app/BookmarkTreeModel.cpp
    app/BookmarkTreeModel.hpp
    app/CurrentPassbandScanner.cpp
    app/CurrentPassbandScanner.hpp
    app/WideRangeScanPlanner.cpp
    app/WideRangeScanPlanner.hpp
    app/FrequencyDigitController.cpp
    app/FrequencyDigitController.hpp
    app/FrequencyViewport.cpp
    app/FrequencyViewport.hpp
    app/ReceiverRuntime.cpp
    app/ReceiverRuntime.hpp
    app/WaterfallFrameDelivery.cpp
    app/WaterfallFrameDelivery.hpp
)
target_include_directories(sdr_application_model PUBLIC app)
target_link_libraries(
    sdr_application_model
    PUBLIC
        Qt6::Core
        Qt6::Gui
        sdr_device_domain
        sdr_platform_audio
        sdr_platform_persistence
        sdr_radio_domain
        sdr_spectrum_data
)
sdr_enable_warnings(sdr_application_model)

qt_add_executable(slopsdr app/main.cpp)
add_executable(slopsdr::app ALIAS slopsdr)

target_compile_features(slopsdr PRIVATE cxx_std_20)
target_include_directories(
    slopsdr
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        gui
)
target_link_libraries(
    slopsdr
    PRIVATE
        sdr_application_model
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)
if(ENABLE_GNURADIO AND ENABLE_SOAPYSDR)
    target_link_libraries(
        slopsdr
        PRIVATE
            SoapySDR
            gnuradio::gnuradio-runtime
            sdr_gnuradio_backend
            sdr_soapy_device_provider
    )
endif()
sdr_enable_warnings(slopsdr)

set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(
    gui/assets/slopsdr-logo.png
    PROPERTIES
        QT_RESOURCE_ALIAS assets/slopsdr-logo.png
)
qt_add_resources(
    slopsdr
    slopsdr_assets
    PREFIX "/"
    FILES gui/assets/slopsdr-logo.png
)
qt_add_qml_module(
    slopsdr
    URI SDRReceiver
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    NO_IMPORT_SCAN
    DEPENDENCIES QtQuick QtQuick.Dialogs
    QML_FILES qml/Main.qml
    SOURCES
        gui/SpectrumAmplitudeScale.cpp
        gui/SpectrumAmplitudeScale.hpp
        gui/SpectrumAverager.cpp
        gui/SpectrumAverager.hpp
        gui/FrequencyAlignedDisplay.cpp
        gui/FrequencyAlignedDisplay.hpp
        gui/FilterIndicator.cpp
        gui/FilterIndicator.hpp
        gui/SpectrumDisplayReduction.cpp
        gui/SpectrumDisplayReduction.hpp
        gui/WaterfallPalette.cpp
        gui/WaterfallPalette.hpp
        gui/SpectrumWaterfallItem.cpp
        gui/SpectrumWaterfallItem.hpp
)

set_target_properties(slopsdr PROPERTIES OUTPUT_NAME slopsdr)
add_custom_target(app DEPENDS slopsdr)

install(TARGETS slopsdr RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
