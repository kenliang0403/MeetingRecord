#!/bin/bash
# deploy_104.sh — build, atomic-install binary, trigger systemd restart.
#
# Same shape as redeploy.sh but does the cmake --build itself (the .ps1
# wrapper for 104 doesn't pre-build).
#
# Does NOT run `cmake --install` because that would overwrite
# /opt/recorder/config/config.json with the repo default (alias=<alias-1>),
# while 104 needs its own alias (e.g. <alias-2>).
#
# Does NOT kill or nohup-start processes — systemd manages the lifecycle.
set -e

PW="$1"
if [ -z "$PW" ]; then
    echo "usage: deploy_104.sh <sudo_password>"
    exit 1
fi
SUDO() { echo "$PW" | sudo -S -p '' "$@"; }

BUILD_DIR=/opt/recorder/recorder-core/build
BUILD_BIN="$BUILD_DIR/recorder-core"
LIVE_BIN=/opt/recorder/bin/recorder-core
TRIGGER=/opt/recorder/run/restart-recorder.flag

# 1) build
cd "$BUILD_DIR"
SUDO cmake --build . --target recorder-core -j 2>&1 | tail -10

if [ ! -x "$BUILD_BIN" ]; then
    echo "build binary missing after build: $BUILD_BIN"
    exit 1
fi

# 2) atomic-replace the live binary (rename(2); safe while old binary runs)
SUDO install -m 0755 "$BUILD_BIN" "$LIVE_BIN"
ls -l "$LIVE_BIN"

# 3) trigger systemd restart via path-unit. /opt/recorder/run/ should be
#    chowned to ftadmin by install_web.sh; create+chown defensively here
#    in case install_web.sh hasn't run on this box yet.
if [ ! -d "$(dirname "$TRIGGER")" ]; then
    SUDO mkdir -p "$(dirname "$TRIGGER")"
    SUDO chown ftadmin:ftadmin "$(dirname "$TRIGGER")"
fi
date '+%Y-%m-%d %H:%M:%S' > "$TRIGGER"

# 4) brief verify
sleep 3
echo "=== status ==="
SUDO systemctl status recorder-core --no-pager | head -8
echo
echo "=== last 6 log lines ==="
SUDO journalctl -u recorder-core -n 6 --no-pager
