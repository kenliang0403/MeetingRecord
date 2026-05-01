#!/bin/bash
# start-foreground.sh — used by systemd unit recorder-core.service.
# Unlike start.sh (which nohups + writes pidfile), this exec()s the binary
# in foreground so systemd can supervise it directly.
INSTALL_DIR="/opt/recorder"
CONFIG="$INSTALL_DIR/config/config.json"
BIN="$INSTALL_DIR/bin/recorder-core"

TOOLSET_LIB64="/opt/UOS/gcc-toolset-12/root/usr/lib64"
if [ -d "$TOOLSET_LIB64" ]; then
    export LD_LIBRARY_PATH="$TOOLSET_LIB64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

export PTLIBPLUGINDIR="/usr/local/lib/opal-1.27.2/codecs:/usr/local/lib/pwlib/codecs:/usr/lib/pwlib/codecs"

exec "$BIN" -c "$CONFIG"
