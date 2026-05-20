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

# Deploy user (owns /opt/recorder/asr/bridge and /opt/recorder/run).
# Defaults to ftadmin; .ps1 wrapper passes RUN_USER='<user>' to override.
RUN_USER="${RUN_USER:-ftadmin}"

ASR_SRC="/opt/recorder/recorder-core/scripts/asr"
ASR_BIN_DIR="/opt/recorder/asr/bridge"
SHERPA_BIN_DIR="/opt/recorder/asr/tmp/sherpa-onnx-v1.13.0-linux-x64-shared/bin"
MODEL_DIR="/opt/recorder/asr/models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30"
VENV_DIR="/opt/recorder/asr/venv"

echo "[1/6] sanity checks"
for path in "$SHERPA_BIN_DIR/sherpa-onnx-online-websocket-server" \
            "$SHERPA_BIN_DIR/sherpa-onnx-offline-punctuation" \
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

echo "[2/6] sync bridge.py + hotwords to /opt/recorder/asr/"
SUDO mkdir -p "$ASR_BIN_DIR"
SUDO chown ${RUN_USER}:${RUN_USER} "$ASR_BIN_DIR"
cp -f "$ASR_SRC/recorder-asr-bridge.py" "$ASR_BIN_DIR/recorder-asr-bridge.py"
chmod +x "$ASR_BIN_DIR/recorder-asr-bridge.py"
# hotwords file: install default only if operator hasn't customised it yet
HOTWORDS_DST=/opt/recorder/asr/models/hotwords.txt
if [ ! -f "$HOTWORDS_DST" ]; then
    SUDO cp "$ASR_SRC/hotwords_default.txt" "$HOTWORDS_DST"
    SUDO chown ${RUN_USER}:${RUN_USER} "$HOTWORDS_DST"
    echo "    → installed default hotwords ($(wc -l < $HOTWORDS_DST) entries)"
else
    echo "    → hotwords.txt already present, keeping operator version"
fi

echo "[3/6] install systemd units"
_render_asr_unit() {
    local src="$1" out="$2"
    if [ "${RUN_USER}" = "ftadmin" ]; then
        cp "$src" "$out"
    else
        sed -e "s/^User=ftadmin/User=${RUN_USER}/" \
            -e "s/^Group=ftadmin/Group=${RUN_USER}/" \
            "$src" > "$out"
    fi
}
_render_asr_unit "$ASR_SRC/recorder-asr.service"        /tmp/recorder-asr.service.rendered
_render_asr_unit "$ASR_SRC/recorder-asr-bridge.service" /tmp/recorder-asr-bridge.service.rendered
SUDO cp /tmp/recorder-asr.service.rendered        /etc/systemd/system/recorder-asr.service
SUDO cp /tmp/recorder-asr-bridge.service.rendered /etc/systemd/system/recorder-asr-bridge.service
rm -f /tmp/recorder-asr.service.rendered /tmp/recorder-asr-bridge.service.rendered

echo "[4/6] ensure /opt/recorder/run is writable by ftadmin"
SUDO mkdir -p /opt/recorder/run
SUDO chown ${RUN_USER}:${RUN_USER} /opt/recorder/run

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
