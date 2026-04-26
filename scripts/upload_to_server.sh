#!/bin/bash
# upload_to_server.sh â€?Upload recorder-core source + tarballs to the server
# Run from the recorder-core/ directory on Windows/local machine.
# Usage: bash scripts/upload_to_server.sh

set -e
SERVER="ftadmin@<recorder_host>"
REMOTE_BASE="/opt/recorder"
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export DISPLAY=:0
export SSH_ASKPASS="$LOCAL_DIR/scripts/askpass.sh"
export SSH_ASKPASS_REQUIRE=force

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=no -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no -o NumberOfPasswordPrompts=1"
SCP="scp $SSH_OPTS"
SSH="ssh $SSH_OPTS"
SUDO_PW="${RECORDER_102_SUDO_PASSWORD:-$RECORDER_102_PASSWORD}"
if [ -z "$SUDO_PW" ]; then
    echo "ERROR: set RECORDER_102_PASSWORD (SSH) and optionally RECORDER_102_SUDO_PASSWORD (sudo)."
    exit 1
fi

echo "=== Creating remote directories ==="
$SSH "$SERVER" "echo '$SUDO_PW' | sudo -S mkdir -p $REMOTE_BASE/{bin,config,logs,recordings,run,recorder-core,tarballs,third_party/nlohmann}
echo '$SUDO_PW' | sudo -S chown -R ftadmin:ftadmin $REMOTE_BASE"

echo "=== Uploading source code ==="
$SCP -r "$LOCAL_DIR/src"       "$SERVER:$REMOTE_BASE/recorder-core/"
$SCP    "$LOCAL_DIR/CMakeLists.txt" "$SERVER:$REMOTE_BASE/recorder-core/"

echo "=== Uploading config ==="
$SCP "$LOCAL_DIR/config/config.json" "$SERVER:$REMOTE_BASE/config/"

echo "=== Uploading scripts ==="
$SCP "$LOCAL_DIR/scripts/"*.sh "$SERVER:$REMOTE_BASE/recorder-core/scripts/"
$SSH "$SERVER" "chmod +x $REMOTE_BASE/recorder-core/scripts/*.sh"

echo "=== Uploading tarballs ==="
for f in "$LOCAL_DIR/third_party/tarballs/"*; do
    [ -f "$f" ] && $SCP "$f" "$SERVER:$REMOTE_BASE/tarballs/"
done

echo "=== Uploading nlohmann/json header ==="
$SCP "$LOCAL_DIR/third_party/tarballs/json.hpp" \
     "$SERVER:$REMOTE_BASE/third_party/nlohmann/json.hpp"

echo "=== Upload complete ==="
echo "Next steps on server:"
echo "  1. Edit $REMOTE_BASE/config/config.json â€?set gk.host to VP9660 IP"
echo "  2. sudo bash $REMOTE_BASE/recorder-core/scripts/install_deps_server.sh"
echo "  3. bash $REMOTE_BASE/recorder-core/scripts/build.sh"
