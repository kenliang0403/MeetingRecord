#!/bin/bash
# install_deps.sh â€?Install all build dependencies for recorder-core on UOS/CentOS 8
# Run as root or with sudo.
set -e

JOBS=$(nproc)
INSTALL_PREFIX="/opt/recorder"
BUILD_DIR="/tmp/recorder_build"
mkdir -p "$BUILD_DIR"

echo "=== [1/6] Installing system packages ==="
dnf install -y \
    gcc gcc-c++ make cmake3 git \
    autoconf automake libtool pkg-config \
    openssl-devel \
    alsa-lib-devel \
    libxml2-devel \
    expat-devel \
    bzip2-devel \
    zlib-devel \
    xz-devel \
    lzma-devel \
    boost-devel \
    nasm yasm \
    || yum install -y \
       gcc gcc-c++ make cmake3 git \
       autoconf automake libtool pkg-config \
       openssl-devel alsa-lib-devel libxml2-devel expat-devel \
       bzip2-devel zlib-devel xz-devel boost-devel nasm yasm

# Make cmake3 available as cmake if needed
if ! command -v cmake &>/dev/null && command -v cmake3 &>/dev/null; then
    ln -sfn "$(which cmake3)" /usr/local/bin/cmake
fi

echo "=== [2/6] Installing nlohmann/json (header-only) ==="
NLOHMANN_DIR="/opt/recorder/third_party/nlohmann"
mkdir -p "$NLOHMANN_DIR"
if [ ! -f "$NLOHMANN_DIR/json.hpp" ]; then
    curl -L "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" \
         -o "$NLOHMANN_DIR/json.hpp" 2>/dev/null || \
    curl -L "https://gitee.com/mirrors/nlohmann-json/raw/v3.11.3/single_include/nlohmann/json.hpp" \
         -o "$NLOHMANN_DIR/json.hpp"
fi
echo "nlohmann/json installed at $NLOHMANN_DIR/json.hpp"

echo "=== [3/6] Installing spdlog (header-only, v1.13) ==="
SPDLOG_DIR="/opt/recorder/third_party/spdlog"
if [ ! -d "$SPDLOG_DIR" ]; then
    cd "$BUILD_DIR"
    if [ -f /tmp/spdlog.tar.gz ]; then
        tar -xzf /tmp/spdlog.tar.gz
        SPDLOG_SRC=$(ls -d spdlog-* | head -1)
    else
        git clone --depth=1 --branch v1.13.0 \
            https://github.com/gabime/spdlog.git spdlog-1.13.0 2>/dev/null || \
        git clone --depth=1 \
            https://gitee.com/mirrors/spdlog.git spdlog-1.13.0
        SPDLOG_SRC="spdlog-1.13.0"
    fi
    cmake -S "$BUILD_DIR/$SPDLOG_SRC" -B "$BUILD_DIR/spdlog-build" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DSPDLOG_BUILD_SHARED=OFF
    cmake --build "$BUILD_DIR/spdlog-build" -j"$JOBS"
    cmake --install "$BUILD_DIR/spdlog-build"
fi

echo "=== [4/6] Installing FFmpeg 6.x with libx264 ==="
if ! pkg-config --exists libavformat; then
    # Install x264 first
    cd "$BUILD_DIR"
    if [ ! -d x264 ]; then
        git clone --depth=1 https://code.videolan.org/videolan/x264.git 2>/dev/null || \
        git clone --depth=1 https://gitee.com/mirrors/x264.git
    fi
    cd x264
    ./configure --prefix=/usr/local --enable-shared --enable-static --enable-pic
    make -j"$JOBS" && make install
    ldconfig

    # Build FFmpeg
    cd "$BUILD_DIR"
    FFMPEG_VER="6.1.1"
    if [ -f /tmp/ffmpeg-${FFMPEG_VER}.tar.bz2 ]; then
        tar -xjf /tmp/ffmpeg-${FFMPEG_VER}.tar.bz2
    else
        curl -L "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.bz2" \
             -o ffmpeg-${FFMPEG_VER}.tar.bz2
        tar -xjf ffmpeg-${FFMPEG_VER}.tar.bz2
    fi
    cd "ffmpeg-${FFMPEG_VER}"
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
    ./configure \
        --prefix=/usr/local \
        --enable-gpl \
        --enable-nonfree \
        --enable-libx264 \
        --enable-shared \
        --enable-static \
        --disable-doc \
        --disable-programs \
        --extra-cflags="-I/usr/local/include" \
        --extra-ldflags="-L/usr/local/lib"
    make -j"$JOBS" && make install
    ldconfig
    echo "FFmpeg installed"
fi

echo "=== [5/6] Compiling PTLib ==="
PTLIB_VERSION="2.18.8"
if ! pkg-config --exists ptlib; then
    cd "$BUILD_DIR"
    if [ -f /tmp/ptlib-${PTLIB_VERSION}.tar.bz2 ]; then
        tar -xjf /tmp/ptlib-${PTLIB_VERSION}.tar.bz2
        PTLIB_SRC="ptlib-${PTLIB_VERSION}"
    else
        git clone --depth=1 --branch v${PTLIB_VERSION} \
            https://github.com/willamowius/ptlib.git ptlib-${PTLIB_VERSION} 2>/dev/null || \
        git clone --depth=1 \
            https://github.com/willamowius/ptlib.git ptlib-${PTLIB_VERSION}
        PTLIB_SRC="ptlib-${PTLIB_VERSION}"
    fi
    cd "$BUILD_DIR/$PTLIB_SRC"
    ./configure --prefix=/usr/local \
        --enable-shared \
        --enable-ipv6 \
        --disable-lua \
        --disable-ruby \
        --disable-expat \
        CXXFLAGS="-std=c++14 -fPIC"
    make -j"$JOBS" && make install
    ldconfig
    echo "PTLib installed"
fi

echo "=== [6/6] Compiling H.323Plus ==="
H323PLUS_VERSION="1.27.0"
if ! pkg-config --exists h323plus; then
    cd "$BUILD_DIR"
    if [ -f /tmp/h323plus-${H323PLUS_VERSION}.tar.bz2 ]; then
        tar -xjf /tmp/h323plus-${H323PLUS_VERSION}.tar.bz2
        H323_SRC="h323plus-${H323PLUS_VERSION}"
    else
        git clone --depth=1 \
            https://github.com/willamowius/h323plus.git h323plus-${H323PLUS_VERSION} 2>/dev/null || \
        git clone --depth=1 \
            https://gitee.com/mirrors/h323plus.git h323plus-${H323PLUS_VERSION}
        H323_SRC="h323plus-${H323PLUS_VERSION}"
    fi
    cd "$BUILD_DIR/$H323_SRC"
    export PTLIBDIR=/usr/local
    ./configure --prefix=/usr/local \
        --enable-shared \
        CXXFLAGS="-std=c++14 -fPIC"
    make -j"$JOBS" && make install
    ldconfig
    echo "H.323Plus installed"
fi

echo "=== All dependencies installed successfully ==="
echo "You can now build recorder-core with: cd /opt/recorder/recorder-core && ./scripts/build.sh"
