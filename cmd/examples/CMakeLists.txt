# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

include(FetchContent)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)
find_package(ZLIB REQUIRED)
include(ExternalProject)
set(FFMPEG_SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/ffmpeg)
set(FFMPEG_BUILD_DIR  ${CMAKE_BINARY_DIR}/ffmpeg_build)
set(FFMPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/ffmpeg_install)

ExternalProject_Add(ffmpeg_external
        SOURCE_DIR ${FFMPEG_SOURCE_DIR}
        BINARY_DIR ${FFMPEG_BUILD_DIR}
        CONFIGURE_COMMAND
        ${FFMPEG_SOURCE_DIR}/configure
        --prefix=${FFMPEG_INSTALL_DIR}
        --disable-programs
        --disable-doc
        --disable-debug
        --disable-network
        --disable-everything
        --enable-avformat
        --enable-avcodec
        --enable-avutil
        --enable-protocol=file
        --enable-demuxer=mov,mp4,m4a
        --enable-muxer=mp4
        --enable-parser=h264,aac
        BUILD_COMMAND $(MAKE) -j
        INSTALL_COMMAND $(MAKE) install
)

add_library(ffmpeg_import INTERFACE)
add_dependencies(ffmpeg_import ffmpeg_external)
target_include_directories(ffmpeg_import INTERFACE ${FFMPEG_INSTALL_DIR}/include)
target_link_directories(ffmpeg_import INTERFACE ${FFMPEG_INSTALL_DIR}/lib)
target_link_libraries(ffmpeg_import INTERFACE avformat avcodec avutil ZLIB::ZLIB)

add_executable(qclient client.cpp
        subscriber_util.h)
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

add_executable(qclient2
        client2.cpp
        media.h
        media.cpp
        catalog.hpp
        async_stdin_reader.cpp
        ffmpeg_cmaf_splitter.cpp
        ffmpeg_cmaf_splitter.hpp
        ffmpeg_moq_adapter.cpp
        ffmpeg_moq_adapter.h)
target_link_libraries(qclient2 PRIVATE quicr nlohmann_json::nlohmann_json ffmpeg_import)
target_include_directories(qclient2 PRIVATE
        ../dependencies
        ${FFMPEG_INSTALL_DIR}/include
        ${CMAKE_SOURCE_DIR}/dependencies/concurrentqueue   # itt van a concurrentqueue.h
        ${CMAKE_SOURCE_DIR}/dependencies)

target_compile_options(qclient2
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(qclient2
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_definitions(qclient2 PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

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
        Lint(qclient2)
endif()