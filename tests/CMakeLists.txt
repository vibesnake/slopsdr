# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 vibesnake

add_executable(application_model_tests ApplicationModelTest.cpp)
target_link_libraries(
    application_model_tests
    PRIVATE
        sdr_application_model
        Qt6::Test
)
sdr_enable_warnings(application_model_tests)

add_test(NAME application_model_tests COMMAND application_model_tests)

add_executable(application_log_model_tests ApplicationLogModelTest.cpp)
target_link_libraries(
    application_log_model_tests
    PRIVATE
        sdr_application_model
        Qt6::Test
)
sdr_enable_warnings(application_log_model_tests)

add_test(NAME application_log_model_tests COMMAND application_log_model_tests)

add_executable(bookmark_model_tests BookmarkTreeModelTest.cpp)
target_link_libraries(
    bookmark_model_tests
    PRIVATE
        sdr_application_model
        Qt6::Test
)
sdr_enable_warnings(bookmark_model_tests)

add_test(NAME bookmark_model_tests COMMAND bookmark_model_tests)

add_executable(gain_slider_binding_tests GainSliderBindingTest.cpp)
set_source_files_properties(
    ../gui/assets/slopsdr-logo.png
    PROPERTIES
        QT_RESOURCE_ALIAS assets/slopsdr-logo.png
)
qt_add_resources(
    gain_slider_binding_tests
    slopsdr_test_assets
    PREFIX "/"
    FILES ../gui/assets/slopsdr-logo.png
)
target_link_libraries(
    gain_slider_binding_tests
    PRIVATE
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
)
sdr_enable_warnings(gain_slider_binding_tests)

add_test(NAME gain_slider_binding_tests COMMAND gain_slider_binding_tests)
set_tests_properties(
    gain_slider_binding_tests
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RENDER_LOOP=basic;QT_QUICK_BACKEND=software"
        TIMEOUT 20
)

add_executable(audio_output_tests AudioOutputTest.cpp)
target_link_libraries(
    audio_output_tests
    PRIVATE
        sdr_audio_dsp_support
        sdr_platform_audio
        Qt6::Test
)
sdr_enable_warnings(audio_output_tests)

add_test(NAME audio_output_tests COMMAND audio_output_tests)

add_executable(dsd_fme_process_tests DsdFmeProcessServiceTest.cpp)
target_link_libraries(
    dsd_fme_process_tests
    PRIVATE
        sdr_platform_audio
        Qt6::Test
)
sdr_enable_warnings(dsd_fme_process_tests)

add_test(NAME dsd_fme_process_tests COMMAND dsd_fme_process_tests)

if(ENABLE_AUDIO_HARDWARE_TESTING)
    add_executable(audio_output_smoke_test AudioOutputSmokeTest.cpp)
    target_link_libraries(
        audio_output_smoke_test
        PRIVATE
            sdr_platform_audio
            Qt6::Test
    )
    sdr_enable_warnings(audio_output_smoke_test)
    add_test(NAME audio_output_smoke_test COMMAND audio_output_smoke_test)
    set_tests_properties(
        audio_output_smoke_test
        PROPERTIES
            LABELS "audio-hardware"
            TIMEOUT 15
    )
endif()

add_executable(receiver_runtime_tests ReceiverRuntimeTest.cpp)
target_link_libraries(
    receiver_runtime_tests
    PRIVATE
        sdr_application_model
        Qt6::Test
)
sdr_enable_warnings(receiver_runtime_tests)

add_test(NAME receiver_runtime_tests COMMAND receiver_runtime_tests)
set_tests_properties(receiver_runtime_tests PROPERTIES TIMEOUT 20)

add_executable(
    frequency_digit_controller_tests
    FrequencyDigitControllerTest.cpp
)
target_link_libraries(
    frequency_digit_controller_tests
    PRIVATE
        sdr_application_model
        Qt6::Test
)
sdr_enable_warnings(frequency_digit_controller_tests)

add_test(
    NAME frequency_digit_controller_tests
    COMMAND frequency_digit_controller_tests
)

add_executable(receiver_domain_tests ReceiverDomainTest.cpp)
target_link_libraries(
    receiver_domain_tests
    PRIVATE
        sdr_radio_domain
        Qt6::Test
)
sdr_enable_warnings(receiver_domain_tests)

add_test(NAME receiver_domain_tests COMMAND receiver_domain_tests)

add_executable(spectrum_data_path_tests SpectrumDataPathTest.cpp)
target_link_libraries(
    spectrum_data_path_tests
    PRIVATE
        sdr_spectrum_data
        Qt6::Test
)
sdr_enable_warnings(spectrum_data_path_tests)

add_test(NAME spectrum_data_path_tests COMMAND spectrum_data_path_tests)

add_executable(waterfall_frame_delivery_tests WaterfallFrameDeliveryTest.cpp)
target_link_libraries(
    waterfall_frame_delivery_tests
    PRIVATE
        sdr_application_model
        Qt6::Test
)
sdr_enable_warnings(waterfall_frame_delivery_tests)

add_test(NAME waterfall_frame_delivery_tests COMMAND waterfall_frame_delivery_tests)

add_executable(
    spectrum_waterfall_item_tests
    SpectrumWaterfallItemTest.cpp
    ../gui/FrequencyAlignedDisplay.cpp
    ../gui/FrequencyAlignedDisplay.hpp
    ../gui/FilterIndicator.cpp
    ../gui/FilterIndicator.hpp
    ../gui/SpectrumAmplitudeScale.cpp
    ../gui/SpectrumAmplitudeScale.hpp
    ../gui/SpectrumDisplayReduction.cpp
    ../gui/SpectrumDisplayReduction.hpp
    ../gui/WaterfallPalette.cpp
    ../gui/WaterfallPalette.hpp
    ../gui/SpectrumWaterfallItem.cpp
    ../gui/SpectrumWaterfallItem.hpp
)
target_include_directories(spectrum_waterfall_item_tests PRIVATE ../gui)
target_link_libraries(
    spectrum_waterfall_item_tests
    PRIVATE
        sdr_application_model
        Qt6::Quick
        Qt6::Test
)
sdr_enable_warnings(spectrum_waterfall_item_tests)

add_test(NAME spectrum_waterfall_item_tests COMMAND spectrum_waterfall_item_tests)
set_tests_properties(
    spectrum_waterfall_item_tests
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RENDER_LOOP=basic;QT_QUICK_BACKEND=software"
        TIMEOUT 20
)

add_executable(device_controller_tests DeviceControllerTest.cpp)
target_link_libraries(
    device_controller_tests
    PRIVATE
        sdr_device_domain
        Qt6::Test
)
sdr_enable_warnings(device_controller_tests)

add_test(NAME device_controller_tests COMMAND device_controller_tests)

add_executable(ppm_calibration_tests PpmCalibrationTest.cpp)
target_link_libraries(
    ppm_calibration_tests
    PRIVATE
        sdr_device_domain
        Qt6::Test
)
sdr_enable_warnings(ppm_calibration_tests)

add_test(NAME ppm_calibration_tests COMMAND ppm_calibration_tests)

if(ENABLE_GNURADIO)
    add_executable(gnuradio_smoke_test GnuRadioSmokeTest.cpp)
    target_link_libraries(
        gnuradio_smoke_test
        PRIVATE
            sdr_gnuradio_backend
            Qt6::Core
    )
    sdr_enable_warnings(gnuradio_smoke_test)

    add_test(NAME gnuradio_smoke_test COMMAND gnuradio_smoke_test)

    add_executable(
        gnuradio_receiver_backend_tests
        GnuRadioReceiverBackendTest.cpp
    )
    target_link_libraries(
        gnuradio_receiver_backend_tests
        PRIVATE
            sdr_gnuradio_backend
            Qt6::Test
    )
    sdr_enable_warnings(gnuradio_receiver_backend_tests)

    add_test(
        NAME gnuradio_receiver_backend_tests
        COMMAND gnuradio_receiver_backend_tests
    )
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gnuradio-cache")
    set_tests_properties(
        gnuradio_receiver_backend_tests gnuradio_smoke_test
        PROPERTIES
            ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/gnuradio-cache"
            TIMEOUT 20
    )
endif()

if(ENABLE_GNURADIO AND ENABLE_SOAPYSDR)
    add_test(
        NAME gnuradio_diagnostics
        COMMAND $<TARGET_FILE:slopsdr> --diagnose-gnuradio
    )
    set_tests_properties(
        gnuradio_diagnostics
        PROPERTIES
            PASS_REGULAR_EXPRESSION "vmcircbuf_default_factory:"
            TIMEOUT 10
    )
endif()

if(ENABLE_HARDWARE_TESTING)
    if(NOT ENABLE_GNURADIO OR NOT ENABLE_SOAPYSDR)
        message(FATAL_ERROR "ENABLE_HARDWARE_TESTING requires GNU Radio and SoapySDR")
    endif()
    add_executable(hardware_receiver_smoke_test HardwareReceiverSmokeTest.cpp)
    target_link_libraries(
        hardware_receiver_smoke_test
        PRIVATE
            sdr_application_model
            sdr_gnuradio_backend
            sdr_soapy_device_provider
            Qt6::Test
    )
    sdr_enable_warnings(hardware_receiver_smoke_test)
    add_test(NAME hardware_receiver_smoke_test COMMAND hardware_receiver_smoke_test)
    set_tests_properties(
        hardware_receiver_smoke_test
        PROPERTIES
            LABELS "hardware"
            TIMEOUT 60
    )
endif()
