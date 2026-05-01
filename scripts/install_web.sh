#!/bin/bash
# install_web.sh — first-time setup of the recorder admin web on a server.
# Also installs / refreshes the recorder-core systemd unit and a sudoers
# drop-in that lets the web process restart the recorder service.
#
# Usage:  bash install_web.sh <sudo_password>
#
# Idempotent: re-run after pulling new code; will sync files, re-pip,
# reload systemd and restart units. User passwords (auth.json) are never
# touched.

set -e
PW="$1"
if [ -z "$PW" ]; then
  echo "usage: install_web.sh <sudo_password>"
  exit 1
fi
SUDO() { echo "$PW" | sudo -S -p '' "$@"; }

WEB_SRC="/opt/recorder/recorder-core/web"
WEB_DST="/opt/recorder/web"
SCRIPT_SRC="/opt/recorder/recorder-core/scripts"

echo "[1/8] sync web files to ${WEB_DST}"
SUDO mkdir -p "${WEB_DST}/templates" "${WEB_DST}/static"
SUDO rsync -a --delete \
  --exclude 'auth.json' --exclude '.flask_secret' \
  "${WEB_SRC}/" "${WEB_DST}/"
SUDO chown -R ftadmin:ftadmin "${WEB_DST}"

echo "[2/8] python deps"
if ! python3 -c 'import flask, gunicorn' 2>/dev/null; then
  SUDO pip3 install -r "${WEB_DST}/requirements.txt"
else
  echo "    flask + gunicorn already present, skip"
fi

echo "[3/8] install start-foreground.sh"
SUDO install -m 0755 -o root -g root \
  "${SCRIPT_SRC}/start-foreground.sh" /opt/recorder/scripts/start-foreground.sh

echo "[4/8] install / refresh recorder-core.service"
SUDO cp "${WEB_DST}/recorder-core.service" /etc/systemd/system/recorder-core.service

echo "[5/8] install / refresh recorder-web.service"
SUDO cp "${WEB_DST}/recorder-web.service"  /etc/systemd/system/recorder-web.service

echo "[6/8] install sudoers drop-in (allow web → systemctl restart recorder-core)"
SUDO install -m 0440 -o root -g root \
  "${WEB_DST}/recorder-web.sudoers" /etc/sudoers.d/recorder-web

SUDO systemctl daemon-reload

echo "[7/8] migrate any running nohup recorder-core into systemd"
# If there's a non-systemd recorder-core (started via plain start.sh),
# kill it so systemctl start can take over without port conflicts.
if pgrep -af 'recorder-core -c /opt/recorder/config/config.json' \
     | grep -v systemctl | grep -v 'systemd\|gunicorn' >/dev/null 2>&1; then
  if ! SUDO systemctl is-active --quiet recorder-core; then
    echo "    found nohup-managed recorder-core; killing before systemd takeover"
    PIDS=$(pgrep -f 'recorder-core -c /opt/recorder/config/config.json' || true)
    for p in $PIDS; do SUDO kill -TERM "$p" 2>/dev/null || true; done
    sleep 2
    for p in $PIDS; do SUDO kill -KILL "$p" 2>/dev/null || true; done
    SUDO rm -f /opt/recorder/run/recorder-core.pid
  fi
fi

echo "[8/8] enable + start units"
SUDO systemctl enable recorder-core
SUDO systemctl restart recorder-core
SUDO systemctl enable recorder-web
SUDO systemctl restart recorder-web

# auth.json bootstrap — only when missing
if [ ! -f "${WEB_DST}/auth.json" ]; then
  echo '{"users":{}}' | SUDO tee "${WEB_DST}/auth.json" >/dev/null
  SUDO chmod 600 "${WEB_DST}/auth.json"
  SUDO chown ftadmin:ftadmin "${WEB_DST}/auth.json"
  echo ""
  echo "  ⚠ auth.json was missing; created empty. Add a user with:"
  echo "      sudo python3 ${WEB_DST}/setup_user.py admin"
fi

sleep 1
echo ""
echo "=== status ==="
SUDO systemctl status recorder-core --no-pager | head -5
SUDO systemctl status recorder-web  --no-pager | head -5
echo ""
echo "Done. Manage page → http://<server>:8088/"
