#!/bin/bash
# install_asr.sh — install/refresh recorder-asr (sherpa-onnx server) and
# recorder-asr-bridge (Python ffmpeg→sherpa→transcript) services on a host
# that already has the sherpa-onnx binary + Chinese model under
# /opt/recorder/asr/.
#
# Idempotent: re-run after pulling new code; copies bridge.py and
# units, daemon-reloads, restarts services.
#
# Usage:  bash install_asr.sh <sudo_password>
set -e

PW="$1"
if [ -z "$PW" ]; then
    echo "usage: install_asr.sh <sudo_password>"
    exit 1
fi
SUDO() { echo "$PW" | sudo -S -p '' "$@"; }

ASR_SRC="/opt/recorder/recorder-core/scripts/asr"
ASR_BIN_DIR="/opt/recorder/asr/bridge"
SHERPA_BIN_DIR="/opt/recorder/asr/tmp/sherpa-onnx-v1.13.0-linux-x64-shared/bin"
MODEL_DIR="/opt/recorder/asr/models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30"
VENV_DIR="/opt/recorder/asr/venv"

echo "[1/6] sanity checks"
for path in "$SHERPA_BIN_DIR/sherpa-onnx-online-websocket-server" \
            "$MODEL_DIR/encoder.int8.onnx" \
            "$VENV_DIR/bin/python"; do
    if [ ! -e "$path" ]; then
        echo "MISSING prerequisite: $path"
        exit 1
    fi
done
$VENV_DIR/bin/python -c "import websockets" || {
    echo "venv missing 'websockets' — run: $VENV_DIR/bin/pip install websockets"
    exit 1
}

echo "[2/6] sync bridge.py to $ASR_BIN_DIR"
SUDO mkdir -p "$ASR_BIN_DIR"
SUDO chown ftadmin:ftadmin "$ASR_BIN_DIR"
cp -f "$ASR_SRC/recorder-asr-bridge.py" "$ASR_BIN_DIR/recorder-asr-bridge.py"
chmod +x "$ASR_BIN_DIR/recorder-asr-bridge.py"

echo "[3/6] install systemd units"
SUDO cp "$ASR_SRC/recorder-asr.service" /etc/systemd/system/recorder-asr.service
SUDO cp "$ASR_SRC/recorder-asr-bridge.service" /etc/systemd/system/recorder-asr-bridge.service

echo "[4/6] ensure /opt/recorder/run is writable by ftadmin"
SUDO mkdir -p /opt/recorder/run
SUDO chown ftadmin:ftadmin /opt/recorder/run

echo "[5/6] daemon-reload + enable + (re)start"
SUDO systemctl daemon-reload
SUDO systemctl enable recorder-asr recorder-asr-bridge
SUDO systemctl restart recorder-asr
sleep 2
SUDO systemctl restart recorder-asr-bridge

echo "[6/6] status"
sleep 2
SUDO systemctl status recorder-asr        --no-pager | head -8
echo
SUDO systemctl status recorder-asr-bridge --no-pager | head -8
echo
echo "tail recorder-asr-bridge log:"
journalctl -u recorder-asr-bridge -n 15 --no-pager
echo
echo "Done. Live transcript tail will be at /opt/recorder/run/transcript.jsonl"
