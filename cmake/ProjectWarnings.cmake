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

function(sdr_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
       CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            "${target}"
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion
                -Wshadow
        )
        if(SDR_RECEIVER_WARNINGS_AS_ERRORS)
            target_compile_options("${target}" PRIVATE -Werror)
        endif()
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
        endif()
    endif()
endfunction()
