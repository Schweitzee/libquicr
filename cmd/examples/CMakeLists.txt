# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

include(FetchContent)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
        gstreamer-1.0>=1.18
        gstreamer-app-1.0
        gstreamer-video-1.0
        gstreamer-audio-1.0
        gobject-2.0
        glib-2.0
        gstreamer-1.0 gstreamer-app-1.0 gstreamer-pbutils-1.0
)
add_library(gstreamer_import INTERFACE)
target_link_libraries(gstreamer_import INTERFACE PkgConfig::GSTREAMER)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale)

if (NOT FFMPEG_FOUND)
        message(FATAL_ERROR
                "System FFmpeg nem található (pkg-config). "
                "Állítsd USE_SYSTEM_FFMPEG=OFF-ra, hogy a dependencies/ffmpeg forrásból épüljön.")
endif()

add_library(ffmpeg_import INTERFACE)
target_link_libraries(ffmpeg_import INTERFACE PkgConfig::FFMPEG)

add_executable(qclient client.cpp
        subscriber_util.h
)
target_link_libraries(qclient PRIVATE quicr nlohmann_json::nlohmann_json)
target_include_directories(qclient PRIVATE ../dependencies)

target_compile_options(qclient
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(qclient
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_definitions(qclient PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

add_executable(qc_video
        qc_video.cpp
        media.h
        media.cpp
        catalog.hpp
        ffmpeg_cmaf_splitter.cpp
        ffmpeg_cmaf_splitter.hpp
        ffmpeg_moq_adapter.cpp
        ffmpeg_moq_adapter.h
        subscriber_util.cpp
        VideoSubscribeTrackHandler.h
        base64_tool.h
        helper_functions.h)

target_link_libraries(qc_video PRIVATE
        quicr
        nlohmann_json::nlohmann_json
        ffmpeg_import
        gstreamer_import)

target_include_directories(qc_video PRIVATE
        ../dependencies
        ${FFMPEG_INSTALL_DIR}/include
        ${CMAKE_SOURCE_DIR}/dependencies/concurrentqueue   # itt van a concurrentqueue.h
        ${CMAKE_SOURCE_DIR}/dependencies)

target_compile_options(qc_video
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(qc_video
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_definitions(qc_video PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

add_executable(qc_request
        qc_request.cpp
        media.h
        media.cpp
        catalog.hpp
        ffmpeg_cmaf_splitter.cpp
        ffmpeg_cmaf_splitter.hpp
        ffmpeg_moq_adapter.cpp
        ffmpeg_moq_adapter.h
        subscriber_util.cpp
        VideoSubscribeTrackHandler.h
        base64_tool.h
        helper_functions.h
        transcode_request.h)

target_link_libraries(qc_request PRIVATE
        quicr
        nlohmann_json::nlohmann_json
        ffmpeg_import
        gstreamer_import)

target_include_directories(qc_request PRIVATE
        ../dependencies
        ${FFMPEG_INSTALL_DIR}/include
        ${CMAKE_SOURCE_DIR}/dependencies/concurrentqueue   # itt van a concurrentqueue.h
        ${CMAKE_SOURCE_DIR}/dependencies)

target_compile_options(qc_request
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(qc_request
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_definitions(qc_request PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

add_executable(qc_transcode
        qc_transcode.cpp
        media.h
        catalog.hpp
        subscriber_util.cpp
        VideoSubscribeTrackHandler.h
        CatalogSubscribeTrackHandler.h
        base64_tool.h
        helper_functions.h
        transcode_request.h
        transcode_client.cpp
        transcode_client.h)

target_link_libraries(qc_transcode PRIVATE
        quicr
        nlohmann_json::nlohmann_json
        ffmpeg_import
        gstreamer_import)

target_include_directories(qc_transcode PRIVATE
        ../dependencies
        ${FFMPEG_INSTALL_DIR}/include
        ${CMAKE_SOURCE_DIR}/dependencies/concurrentqueue   # itt van a concurrentqueue.h
        ${CMAKE_SOURCE_DIR}/dependencies)

target_compile_options(qc_transcode
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(qc_transcode
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_definitions(qc_transcode PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

add_executable(qserver
        server.cpp
)
target_link_libraries(qserver PRIVATE quicr)
target_include_directories(qserver PRIVATE ../dependencies )

target_compile_options(qserver
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(qserver
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)

target_compile_definitions(qserver PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

if(LINT)
        include(Lint)
        Lint(qserver)
        Lint(qclient)
        Lint(qc_video)
endif()