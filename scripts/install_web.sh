#!/bin/bash
# install_web.sh — first-time setup of the recorder admin web on a server
#
# Usage:  bash install_web.sh <sudo_password>
#
# Idempotent: re-run after pulling new code; will sync files, re-pip,
# reload systemd and restart the unit. User passwords (auth.json) are
# never touched.

set -e
PW="$1"
if [ -z "$PW" ]; then
  echo "usage: install_web.sh <sudo_password>"
  exit 1
fi
SUDO() { echo "$PW" | sudo -S -p '' "$@"; }

WEB_SRC="/opt/recorder/recorder-core/web"
WEB_DST="/opt/recorder/web"

echo "[1/5] sync web files to ${WEB_DST}"
SUDO mkdir -p "${WEB_DST}/templates" "${WEB_DST}/static"
SUDO rsync -a --delete \
  --exclude 'auth.json' --exclude '.flask_secret' \
  "${WEB_SRC}/" "${WEB_DST}/"
SUDO chown -R ftadmin:ftadmin "${WEB_DST}"

echo "[2/5] python deps"
if ! python3 -c 'import flask, gunicorn' 2>/dev/null; then
  SUDO pip3 install -r "${WEB_DST}/requirements.txt"
else
  echo "    flask + gunicorn already present, skip"
fi

echo "[3/5] install systemd unit"
SUDO cp "${WEB_DST}/recorder-web.service" /etc/systemd/system/recorder-web.service
SUDO systemctl daemon-reload

echo "[4/5] ensure auth.json exists"
if [ ! -f "${WEB_DST}/auth.json" ]; then
  echo '{"users":{}}' | SUDO tee "${WEB_DST}/auth.json" >/dev/null
  SUDO chmod 600 "${WEB_DST}/auth.json"
  SUDO chown ftadmin:ftadmin "${WEB_DST}/auth.json"
  echo ""
  echo "  ⚠ auth.json is empty. Create the first user:"
  echo "      cd ${WEB_DST} && python3 setup_user.py admin"
  echo ""
fi

echo "[5/5] restart service"
SUDO systemctl enable recorder-web
SUDO systemctl restart recorder-web
sleep 1
SUDO systemctl status recorder-web --no-pager | head -15
echo ""
echo "Done. Manage page → http://<server>:8088/"
