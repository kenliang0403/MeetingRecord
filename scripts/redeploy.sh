#!/bin/bash
# Redeploy on 102: hangup, install, restart.
set -e
cd /opt/recorder/recorder-core/build

# 1) graceful hangup if a call is active (best effort)
python3 /tmp/ctrl_query.py hangup 2>/dev/null || true
sleep 1

# 2) install the new binary into /opt/recorder/bin/
PW="$1"
if [ -z "$PW" ]; then
    echo "usage: redeploy.sh <sudo_password>"
    exit 1
fi
echo "$PW" | sudo -S -p '' cmake --install . 2>&1 | tail -10

# 3) stop old process
PID=$(pgrep -f "recorder-core -c /opt/recorder/config/config.json" || true)
if [ -n "$PID" ]; then
    echo "stopping pid=$PID"
    kill -TERM $PID
    for i in 1 2 3 4 5; do
        sleep 1
        if ! kill -0 $PID 2>/dev/null; then break; fi
    done
    if kill -0 $PID 2>/dev/null; then
        echo "force kill"
        kill -KILL $PID
    fi
fi

# 4) rotate stdout.log so we have a clean run
if [ -f /opt/recorder/logs/stdout.log ]; then
    mv /opt/recorder/logs/stdout.log /opt/recorder/logs/stdout.log.prev
fi

# 5) start new process via start.sh which sets LD_LIBRARY_PATH (gcc-toolset-12)
cd /home/ftadmin
nohup bash /opt/recorder/scripts/start.sh \
    > /opt/recorder/logs/stdout.log 2>&1 &
disown || true

sleep 2
NEWPID=$(pgrep -f "recorder-core -c /opt/recorder/config/config.json" || true)
echo "new pid=$NEWPID"
ls -l /opt/recorder/bin/recorder-core
echo
echo "=== first 30 log lines ==="
sleep 1
head -30 /opt/recorder/logs/stdout.log 2>/dev/null
