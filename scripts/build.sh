#!/bin/bash
# build.sh â€?build recorder-core
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
JOBS=$(nproc)

echo "=== Building recorder-core ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH \
cmake "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/opt/recorder \
    -DCMAKE_PREFIX_PATH=/usr/local

cmake --build . -j"$JOBS"
if [ -w /opt/recorder ] 2>/dev/null; then
    cmake --install .
else
    if [ -n "$RECORDER_102_SUDO_PASSWORD" ]; then
        echo "$RECORDER_102_SUDO_PASSWORD" | sudo -S cmake --install .
    else
        sudo cmake --install .
    fi
fi
echo "=== Build+install successful: /opt/recorder/bin/recorder-core ==="
