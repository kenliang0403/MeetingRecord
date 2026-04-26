#!/bin/bash
# install_deps_server.sh �?Install all build dependencies using pre-uploaded tarballs
# Run as: sudo bash /opt/recorder/recorder-core/scripts/install_deps_server.sh
set -e

JOBS=$(nproc)
INSTALL_PREFIX="/opt/recorder"
TARBALLS="/opt/recorder/tarballs"
BUILD_DIR="/tmp/recorder_build"
mkdir -p "$BUILD_DIR"

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[$(date '+%H:%M:%S')] $*${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARN: $*${NC}"; }
die()  { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $*${NC}"; exit 1; }

# ── 1. System packages ───────────────────────────────────────────────────────
log "=== [1/7] Installing system packages ==="
dnf install -y \
    gcc gcc-c++ make autoconf automake libtool pkg-config \
    openssl-devel alsa-lib-devel libxml2-devel expat-devel \
    bzip2-devel zlib-devel xz nasm yasm wget curl \
    2>&1 | grep -E '(Installing|Already|Error|错误)' || true

# cmake: UOS uses cmake3 name �?install and alias
if ! command -v cmake &>/dev/null; then
    dnf install -y cmake3 2>&1 | grep -E '(Installing|Already|Error)' || true
    if command -v cmake3 &>/dev/null; then
        ln -sfn "$(command -v cmake3)" /usr/local/bin/cmake
        log "cmake aliased to cmake3"
    fi
fi
cmake --version | head -1

# git: needed for H.323Plus configure script
if ! command -v git &>/dev/null; then
    dnf install -y git 2>&1 | grep -E '(Installing|Already)' || true
fi

log "System packages done."

# ── 2. spdlog ────────────────────────────────────────────────────────────────
log "=== [2/7] Installing spdlog ==="
if ! pkg-config --exists spdlog 2>/dev/null; then
    SPDLOG_TARBALL=$(ls "$TARBALLS"/spdlog*.tar.gz 2>/dev/null | head -1)
    [ -z "$SPDLOG_TARBALL" ] && die "spdlog tarball not found in $TARBALLS"
    cd "$BUILD_DIR"
    tar -xzf "$SPDLOG_TARBALL"
    SPDLOG_DIR=$(tar -tzf "$SPDLOG_TARBALL" | head -1 | cut -d/ -f1)
    cmake -S "$BUILD_DIR/$SPDLOG_DIR" -B "$BUILD_DIR/spdlog-build" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DSPDLOG_BUILD_SHARED=OFF \
          -DSPDLOG_INSTALL=ON \
          -DSPDLOG_BUILD_EXAMPLE=OFF \
          -DSPDLOG_BUILD_TESTS=OFF 2>&1 | tail -3
    cmake --build "$BUILD_DIR/spdlog-build" -j"$JOBS" 2>&1 | tail -3
    cmake --install "$BUILD_DIR/spdlog-build" 2>&1 | tail -3
    log "spdlog installed"
else
    log "spdlog already installed"
fi

# ── 3. x264 ──────────────────────────────────────────────────────────────────
log "=== [3/7] Building x264 ==="
if ! pkg-config --exists x264 2>/dev/null; then
    X264_TARBALL=$(ls "$TARBALLS"/x264*.tar.bz2 2>/dev/null | head -1)
    [ -z "$X264_TARBALL" ] && die "x264 tarball not found in $TARBALLS"
    cd "$BUILD_DIR"
    tar -xjf "$X264_TARBALL"
    X264_DIR=$(tar -tjf "$X264_TARBALL" | head -1 | cut -d/ -f1)
    cd "$BUILD_DIR/$X264_DIR"
    ./configure --prefix=/usr/local \
        --enable-shared --enable-static --enable-pic \
        --disable-opencl 2>&1 | tail -5
    make -j"$JOBS" 2>&1 | tail -3
    make install 2>&1 | tail -3
    ldconfig
    log "x264 installed"
else
    log "x264 already installed"
fi

# ── 4. FFmpeg 6.1 ────────────────────────────────────────────────────────────
log "=== [4/7] Building FFmpeg 6.1.1 ==="
if ! pkg-config --exists libavformat 2>/dev/null; then
    FFMPEG_TARBALL=$(ls "$TARBALLS"/ffmpeg*.tar.bz2 2>/dev/null | head -1)
    [ -z "$FFMPEG_TARBALL" ] && die "FFmpeg tarball not found in $TARBALLS"
    cd "$BUILD_DIR"
    log "Extracting FFmpeg (may take a minute)..."
    tar -xjf "$FFMPEG_TARBALL"
    FFMPEG_DIR=$(tar -tjf "$FFMPEG_TARBALL" | head -1 | cut -d/ -f1)
    cd "$BUILD_DIR/$FFMPEG_DIR"
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
    ./configure \
        --prefix=/usr/local \
        --enable-gpl \
        --enable-libx264 \
        --enable-shared \
        --enable-static \
        --disable-doc \
        --disable-programs \
        --disable-debug \
        --extra-cflags="-I/usr/local/include" \
        --extra-ldflags="-L/usr/local/lib" \
        2>&1 | tail -10
    make -j"$JOBS" 2>&1 | tail -3
    make install 2>&1 | tail -3
    ldconfig
    log "FFmpeg installed"
else
    log "FFmpeg already installed"
fi

# ── 5. PTLib ─────────────────────────────────────────────────────────────────
log "=== [5/7] Building PTLib ==="
if ! pkg-config --exists ptlib 2>/dev/null; then
    PTLIB_TARBALL=$(ls "$TARBALLS"/ptlib*.tar.gz 2>/dev/null | head -1)
    [ -z "$PTLIB_TARBALL" ] && die "PTLib tarball not found in $TARBALLS"
    cd "$BUILD_DIR"
    log "Extracting PTLib..."
    tar -xzf "$PTLIB_TARBALL"
    PTLIB_DIR=$(tar -tzf "$PTLIB_TARBALL" | head -1 | cut -d/ -f1)
    cd "$BUILD_DIR/$PTLIB_DIR"

    # PTLib uses autoconf
    if [ -f configure ]; then
        true
    elif [ -f autogen.sh ]; then
        ./autogen.sh 2>&1 | tail -5
    else
        autoreconf -fiv 2>&1 | tail -5
    fi

    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
    ./configure \
        --prefix=/usr/local \
        --enable-shared \
        --disable-lua \
        --disable-ruby \
        --disable-expat \
        --without-openldap \
        CXXFLAGS="-std=c++14 -fPIC -O2" \
        CFLAGS="-fPIC -O2" \
        2>&1 | tail -10

    make -j"$JOBS" 2>&1 | tail -5
    make install 2>&1 | tail -3
    ldconfig
    log "PTLib installed"
else
    log "PTLib already installed"
fi

# ── 6. H.323Plus ─────────────────────────────────────────────────────────────
log "=== [6/7] Building H.323Plus ==="
if ! pkg-config --exists h323plus 2>/dev/null; then
    H323_TARBALL=$(ls "$TARBALLS"/h323plus*.tar.gz 2>/dev/null | head -1)
    [ -z "$H323_TARBALL" ] && die "H.323Plus tarball not found in $TARBALLS"
    cd "$BUILD_DIR"
    log "Extracting H.323Plus..."
    tar -xzf "$H323_TARBALL"
    H323_DIR=$(tar -tzf "$H323_TARBALL" | head -1 | cut -d/ -f1)
    cd "$BUILD_DIR/$H323_DIR"

    export PTLIBDIR=/usr/local
    export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

    if [ -f configure ]; then
        true
    elif [ -f autogen.sh ]; then
        ./autogen.sh 2>&1 | tail -5
    else
        autoreconf -fiv 2>&1 | tail -5
    fi

    ./configure \
        --prefix=/usr/local \
        --enable-shared \
        CXXFLAGS="-std=c++14 -fPIC -O2" \
        CFLAGS="-fPIC -O2" \
        2>&1 | tail -10

    make -j"$JOBS" 2>&1 | tail -5
    make install 2>&1 | tail -3
    ldconfig
    log "H.323Plus installed"
else
    log "H.323Plus already installed"
fi

# ── 7. Create directories ─────────────────────────────────────────────────────
log "=== [7/7] Setting up /opt/recorder directories ==="
mkdir -p "$INSTALL_PREFIX"/{bin,config,logs,recordings,tarballs}
mkdir -p "$INSTALL_PREFIX"/third_party/nlohmann

# Copy nlohmann/json header
if [ -f "$TARBALLS/json.hpp" ]; then
    cp "$TARBALLS/json.hpp" "$INSTALL_PREFIX/third_party/nlohmann/json.hpp"
fi

log "=== All dependencies installed successfully ==="
echo ""
echo "Summary:"
cmake --version | head -1
gcc --version | head -1
pkg-config --modversion libavformat 2>/dev/null && echo "FFmpeg: OK" || echo "FFmpeg: MISSING"
pkg-config --modversion ptlib    2>/dev/null && echo "PTLib: OK"   || echo "PTLib: MISSING"
pkg-config --modversion h323plus 2>/dev/null && echo "H.323Plus: OK" || echo "H.323Plus: MISSING"
pkg-config --modversion spdlog   2>/dev/null && echo "spdlog: OK"   || echo "spdlog: MISSING"
echo ""
echo "Next: bash /opt/recorder/recorder-core/scripts/build.sh"
