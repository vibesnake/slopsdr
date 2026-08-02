# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 vibesnake

option(
    SDR_RECEIVER_WARNINGS_AS_ERRORS
    "Treat warnings from slopSDR targets as errors"
    OFF
)

option(
    SDR_RECEIVER_ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer for slopSDR targets"
    OFF
)

option(
    SDR_RECEIVER_ENABLE_THREAD_SANITIZER
    "Enable ThreadSanitizer for focused slopSDR targets"
    OFF
)

if(SDR_RECEIVER_ENABLE_SANITIZERS AND SDR_RECEIVER_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "Address/Undefined sanitizers cannot be combined with ThreadSanitizer")
endif()

function(sdr_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
       CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(
            warning_options
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
        )
        if(SDR_RECEIVER_WARNINGS_AS_ERRORS)
            list(APPEND warning_options -Werror)
        endif()

        get_target_property(target_sources "${target}" SOURCES)
        foreach(source IN LISTS target_sources)
            get_source_file_property(
                source_is_generated
                "${source}"
                TARGET_DIRECTORY "${target}"
                GENERATED
            )
            if(NOT source_is_generated)
                set_property(
                    SOURCE "${source}"
                    TARGET_DIRECTORY "${target}"
                    APPEND PROPERTY COMPILE_OPTIONS ${warning_options}
                )
            endif()
        endforeach()

        if(SDR_RECEIVER_ENABLE_SANITIZERS)
            target_compile_options(
                "${target}"
                PRIVATE
                    -fsanitize=address,undefined
                    -fno-omit-frame-pointer
            )
            target_link_options(
                "${target}"
                PRIVATE
                    -fsanitize=address,undefined
                -fno-omit-frame-pointer
            )
        elseif(SDR_RECEIVER_ENABLE_THREAD_SANITIZER)
            target_compile_options(
                "${target}"
                PRIVATE
                    -fsanitize=thread
                    -fno-omit-frame-pointer
            )
            target_link_options(
                "${target}"
                PRIVATE
                    -fsanitize=thread
                    -fno-omit-frame-pointer
            )
        endif()
    endif()
endfunction()
