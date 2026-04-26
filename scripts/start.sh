#!/bin/bash
# start.sh â€?Start recorder-core
INSTALL_DIR="/opt/recorder"
CONFIG="$INSTALL_DIR/config/config.json"
BIN="$INSTALL_DIR/bin/recorder-core"
PID_FILE="$INSTALL_DIR/run/recorder-core.pid"

if [ ! -f "$BIN" ]; then
    echo "ERROR: $BIN not found. Run build.sh first."
    exit 1
fi

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    echo "recorder-core is already running (PID $(cat "$PID_FILE"))"
    exit 0
fi

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/recordings" "$INSTALL_DIR/run"

TOOLSET_LIB64="/opt/UOS/gcc-toolset-12/root/usr/lib64"
if [ -d "$TOOLSET_LIB64" ]; then
    export LD_LIBRARY_PATH="$TOOLSET_LIB64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

export PTLIBPLUGINDIR="/usr/local/lib/opal-1.27.2/codecs:/usr/local/lib/pwlib/codecs:/usr/lib/pwlib/codecs"

nohup "$BIN" -c "$CONFIG" >> "$INSTALL_DIR/logs/stdout.log" 2>&1 &
echo $! > "$PID_FILE"
echo "recorder-core started (PID $(cat "$PID_FILE"))"
echo "Logs: $INSTALL_DIR/logs/"
