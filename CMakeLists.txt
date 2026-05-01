cmake_minimum_required(VERSION 3.14)
project(recorder-core VERSION 1.0.0 LANGUAGES CXX)

# ─── Use GCC 12 from UOS toolset (provides C++17 <filesystem>) ─────────────
if(EXISTS "/opt/UOS/gcc-toolset-12/root/usr/bin/gcc")
    set(CMAKE_C_COMPILER   "/opt/UOS/gcc-toolset-12/root/usr/bin/gcc")
    set(CMAKE_CXX_COMPILER "/opt/UOS/gcc-toolset-12/root/usr/bin/g++")
    # Also need the toolset's runtime libraries on the search path
    link_directories("/opt/UOS/gcc-toolset-12/root/usr/lib64")
    message(STATUS "Using GCC 12 toolset from UOS")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ─── Pkg-config paths (include /usr/local paths) ───────────────────────────
set(ENV{PKG_CONFIG_PATH}
    "/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)

# ─── FFmpeg ─────────────────────────────────────────────────────────────────
pkg_check_modules(AVFORMAT   REQUIRED libavformat)
pkg_check_modules(AVCODEC    REQUIRED libavcodec)
pkg_check_modules(AVUTIL     REQUIRED libavutil)
pkg_check_modules(SWSCALE    REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

# ─── PTLib ──────────────────────────────────────────────────────────────────
pkg_check_modules(PTLIB REQUIRED ptlib)

# ─── H.323Plus (no pkg-config file; find manually) ──────────────────────────
# H.323Plus installs headers to: /usr/local/include/openh323/
# H.323Plus installs library as: libh323_linux_x86_64_.so
set(H323PLUS_INCLUDE_DIR "/usr/local/include/openh323"
    CACHE PATH "H.323Plus include directory")
set(H323PLUS_LIB_DIR "/usr/local/lib"
    CACHE PATH "H.323Plus library directory")

find_library(H323PLUS_LIBRARY
    NAMES h323_linux_x86_64_ h323plus h323
    HINTS ${H323PLUS_LIB_DIR}
    REQUIRED)

if(NOT EXISTS "${H323PLUS_INCLUDE_DIR}/h323.h")
    message(FATAL_ERROR "H.323Plus headers not found at ${H323PLUS_INCLUDE_DIR}/h323.h")
endif()
message(STATUS "H.323Plus library: ${H323PLUS_LIBRARY}")
message(STATUS "H.323Plus include: ${H323PLUS_INCLUDE_DIR}")

# ─── Boost Asio (header-only) ────────────────────────────────────────────────
find_package(Boost 1.66 REQUIRED COMPONENTS system)

# ─── nlohmann/json ───────────────────────────────────────────────────────────
find_package(nlohmann_json 3.2 QUIET)
if(NOT nlohmann_json_FOUND)
    # json.hpp is at third_party/nlohmann/json.hpp
    # Code includes it as <nlohmann/json.hpp>, so include path = third_party/
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
        message(STATUS "Using vendored nlohmann/json from third_party/nlohmann/")
        add_library(nlohmann_json INTERFACE)
        target_include_directories(nlohmann_json INTERFACE
            "${CMAKE_SOURCE_DIR}/third_party")
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
        set(nlohmann_json_FOUND TRUE)
    else()
        message(FATAL_ERROR "nlohmann/json not found")
    endif()
endif()

# ─── spdlog ──────────────────────────────────────────────────────────────────
find_package(spdlog 1.8 QUIET HINTS /usr/local/lib64/cmake/spdlog /usr/local/lib/cmake/spdlog)
if(NOT spdlog_FOUND)
    pkg_check_modules(SPDLOG QUIET spdlog)
    if(NOT SPDLOG_FOUND)
        message(FATAL_ERROR "spdlog not found")
    endif()
endif()

# ─── Sources ─────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/config/ConfigLoader.cpp
    src/h323/RecorderEndpoint.cpp
    src/h323/RecorderConnection.cpp
    src/h323/H261RecvCodec.cpp
    src/h323/H264RecvCodec.cpp
    src/media/FfmpegRecorder.cpp
    src/media/SrsStreamer.cpp
    src/media/VideoSender.cpp
    src/media/VideoSourceChannel.cpp
    src/media/VideoCapturePChannel.cpp
    src/media/AudioCapturePChannel.cpp
    src/media/AudioLevelMeter.cpp
    src/meeting/MeetingRegistry.cpp
    src/tcp/ControlServer.cpp
)

add_executable(recorder-core ${SOURCES})

# ─── Include directories ─────────────────────────────────────────────────────
target_include_directories(recorder-core PRIVATE
    src/
    ${H323PLUS_INCLUDE_DIR}
    ${PTLIB_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    /usr/local/include
)

# ─── Compile definitions ─────────────────────────────────────────────────────
target_compile_definitions(recorder-core PRIVATE
    # H.323Plus feature flags
    H323_VIDEO
    H323_AUDIO_CODECS
    H323_H239
)

# PTLib pkg-config flags already contain -D prefix, use compile_options not definitions
# PTLib adds -fno-exceptions; re-enable exceptions for our code (spdlog/boost/json use them)
target_compile_options(recorder-core PRIVATE ${PTLIB_CFLAGS_OTHER} -fexceptions)

# ─── Link libraries ──────────────────────────────────────────────────────────
target_link_libraries(recorder-core PRIVATE
    ${H323PLUS_LIBRARY}
    ${PTLIB_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    Boost::system
    pthread
    dl
)

target_link_libraries(recorder-core PRIVATE nlohmann_json::nlohmann_json)

if(spdlog_FOUND)
    target_link_libraries(recorder-core PRIVATE spdlog::spdlog)
else()
    target_link_directories(recorder-core PRIVATE ${SPDLOG_LIBRARY_DIRS})
    target_link_libraries(recorder-core PRIVATE ${SPDLOG_LIBRARIES})
endif()

# ─── Linker flags: tell linker where to find /usr/local/lib at runtime ───────
set_target_properties(recorder-core PROPERTIES
    LINK_FLAGS "-Wl,-rpath,/usr/local/lib:/usr/local/lib64"
)

# ─── Compiler flags ──────────────────────────────────────────────────────────
target_compile_options(recorder-core PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
    $<$<CONFIG:Debug>:-g3 -O0>
    $<$<CONFIG:RelWithDebInfo>:-g -O2>
    $<$<CONFIG:Release>:-O3>
)

# ─── Install ─────────────────────────────────────────────────────────────────
install(TARGETS recorder-core DESTINATION /opt/recorder/bin)
install(FILES config/config.json DESTINATION /opt/recorder/config)
install(PROGRAMS scripts/start.sh scripts/stop.sh
        DESTINATION /opt/recorder/scripts)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
