#!/bin/bash
# redeploy.sh — atomic-install new binary, trigger systemd restart.
#
# Caller (upload_build.ps1) is expected to have already run
# `cmake --build . --target recorder-core` before invoking this script.
#
# Does NOT run `cmake --install` because that would overwrite the live
# /opt/recorder/config/config.json (alias / e164 / terminal_id are set
# per-host at runtime, not via the repo default).
#
# Does NOT kill or nohup-start processes — systemd (recorder-core.service)
# manages the lifecycle, and recorder-restart.path watches the trigger file
# /opt/recorder/run/restart-recorder.flag for restart requests.
set -e

PW="$1"
if [ -z "$PW" ]; then
    echo "usage: redeploy.sh <sudo_password>"
    exit 1
fi
SUDO() { echo "$PW" | sudo -S -p '' "$@"; }

# The user that owns /opt/recorder/run and runs the recorder services.
# Defaults to ftadmin to match our reference setup; override via env from
# the calling .ps1 wrapper if your deploy user is different.
RUN_USER="${RUN_USER:-ftadmin}"

BUILD_BIN=/opt/recorder/recorder-core/build/recorder-core
LIVE_BIN=/opt/recorder/bin/recorder-core
TRIGGER=/opt/recorder/run/restart-recorder.flag

if [ ! -x "$BUILD_BIN" ]; then
    echo "build binary missing: $BUILD_BIN — did cmake --build run?"
    exit 1
fi

# 1) atomic-replace the live binary. install(1) uses rename(2), so it
#    works even while the old binary is executing — the running process
#    keeps its old inode via its open fd, while new starts read the new
#    inode at the same path.
SUDO install -m 0755 "$BUILD_BIN" "$LIVE_BIN"
ls -l "$LIVE_BIN"

# 2) trigger systemd to restart via the path-unit. /opt/recorder/run/ is
#    chowned to the deploy user by install_web.sh, so writing the flag file
#    needs no sudo.
if [ ! -d "$(dirname "$TRIGGER")" ]; then
    SUDO mkdir -p "$(dirname "$TRIGGER")"
    SUDO chown "${RUN_USER}:${RUN_USER}" "$(dirname "$TRIGGER")"
fi
date '+%Y-%m-%d %H:%M:%S' > "$TRIGGER"

# 3) brief verify
sleep 3
echo "=== status ==="
SUDO systemctl status recorder-core --no-pager | head -8
echo
echo "=== last 6 log lines ==="
SUDO journalctl -u recorder-core -n 6 --no-pager
