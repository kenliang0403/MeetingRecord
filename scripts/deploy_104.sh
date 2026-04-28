#!/bin/bash
# 104 deploy: build only, replace binary manually, do NOT run cmake install
# (cmake install would overwrite /opt/recorder/config/config.json with the
# repo's default which has alias=<alias-1> — 104 needs alias=<alias-2>.)
set -e

PW="$1"
if [ -z "$PW" ]; then
  echo "usage: deploy_104.sh <sudo_password>"
  exit 1
fi

cd /opt/recorder/recorder-core/build
echo "$PW" | sudo -S -p '' cmake --build . --target recorder-core -j 2>&1 | tail -25

# Best-effort: clear any active call before stopping
python3 /opt/recorder/recorder-core/scripts/ctrl_query.py clear_call 2>/dev/null || true
sleep 1

# Stop running process
PID=$(pgrep -f "recorder-core -c /opt/recorder/config/config.json" || true)
if [ -n "$PID" ]; then
  echo "stopping pid=$PID"
  kill -TERM $PID || true
  for i in 1 2 3 4 5; do
    sleep 1
    if ! kill -0 $PID 2>/dev/null; then break; fi
  done
  if kill -0 $PID 2>/dev/null; then
    echo "force kill"
    kill -KILL $PID
  fi
fi

# Replace binary manually (no cmake install, keeps existing config.json)
echo "$PW" | sudo -S -p '' cp /opt/recorder/recorder-core/build/recorder-core /opt/recorder/bin/recorder-core
echo "$PW" | sudo -S -p '' chmod +x /opt/recorder/bin/recorder-core
ls -l /opt/recorder/bin/recorder-core

# Rotate log
if [ -f /opt/recorder/logs/stdout.log ]; then
  mv /opt/recorder/logs/stdout.log /opt/recorder/logs/stdout.log.prev
fi

# Start via existing start.sh (sets LD_LIBRARY_PATH for gcc-toolset-12)
cd /home/ftadmin
nohup bash /opt/recorder/scripts/start.sh > /opt/recorder/logs/stdout.log 2>&1 &
disown || true
sleep 2

NEWPID=$(pgrep -f "recorder-core -c /opt/recorder/config/config.json" || true)
echo "new pid=$NEWPID"
echo "=== first 15 log lines ==="
sleep 1
head -15 /opt/recorder/logs/stdout.log 2>/dev/null
