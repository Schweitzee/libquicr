# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

add_subdirectory(examples)

# Transcode client with GStreamer support
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GSTREAMER gstreamer-1.0)
    pkg_check_modules(GSTREAMER_APP gstreamer-app-1.0)

    if(GSTREAMER_FOUND AND GSTREAMER_APP_FOUND)
        message(STATUS "Found GStreamer ${GSTREAMER_VERSION}")

        # Transcode library
        add_library(transcode_client STATIC
            transcode_client.cpp
            transcode_client.h
        )

        target_include_directories(transcode_client PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${GSTREAMER_INCLUDE_DIRS}
            ${GSTREAMER_APP_INCLUDE_DIRS}
        )

        target_link_libraries(transcode_client PUBLIC
            ${GSTREAMER_LIBRARIES}
            ${GSTREAMER_APP_LIBRARIES}
        )

        target_compile_options(transcode_client PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        )

        set_target_properties(transcode_client PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS OFF
        )

        # Transcode client executable
        add_executable(qc_transcode qc_transcode.cpp)

        target_link_libraries(qc_transcode PRIVATE
            quicr
            transcode_client
        )

        target_compile_options(qc_transcode PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        )

        set_target_properties(qc_transcode PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS OFF
        )

        target_compile_definitions(qc_transcode PRIVATE
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
        )

        message(STATUS "qc_transcode will be built with GStreamer support")
    else()
        message(WARNING "GStreamer or GStreamer-app not found. qc_transcode will not be built.")
        message(WARNING "Install with: apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev")
    endif()
else()
    message(WARNING "PkgConfig not found. Cannot search for GStreamer. qc_transcode will not be built.")
endif()
