#!/bin/bash
# install_web.sh — first-time setup of the recorder admin web on a server.
# Also installs / refreshes the recorder-core systemd unit and a path/service
# unit pair that lets the web process trigger a recorder-core restart by
# writing a flag file (no sudo needed for the web process).
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

# The user that owns /opt/recorder/web, /opt/recorder/run, auth.json, etc.
# and runs recorder-core / recorder-web. Defaults to ftadmin to match our
# reference setup; .ps1 wrappers pass RUN_USER='<user>' to override.
RUN_USER="${RUN_USER:-ftadmin}"

WEB_SRC="/opt/recorder/recorder-core/web"
WEB_DST="/opt/recorder/web"
SCRIPT_SRC="/opt/recorder/recorder-core/scripts"

echo "[1/8] sync web files to ${WEB_DST}"
# Capture Python source hash before sync so we can decide later whether the
# Flask app actually needs a restart. Static / templates / unit files alone
# (typical "frontend tweak" deploy) shouldn't kick gunicorn — refreshing
# them is enough.
_py_hash() {
    if [ -d "${WEB_DST}" ]; then
        find "${WEB_DST}" -maxdepth 1 -name '*.py' -print0 2>/dev/null \
            | sort -z | xargs -0 -r md5sum 2>/dev/null | md5sum | awk '{print $1}'
    else
        echo "none"
    fi
}
PY_HASH_BEFORE=$(_py_hash)

SUDO mkdir -p "${WEB_DST}/templates" "${WEB_DST}/static"
SUDO rsync -a --delete \
  --exclude 'auth.json' --exclude '.flask_secret' \
  "${WEB_SRC}/" "${WEB_DST}/"
SUDO chown -R ${RUN_USER}:${RUN_USER} "${WEB_DST}"

PY_HASH_AFTER=$(_py_hash)
NEED_WEB_RESTART=0
if [ "$PY_HASH_BEFORE" != "$PY_HASH_AFTER" ]; then
    echo "    web/*.py changed → will restart recorder-web"
    NEED_WEB_RESTART=1
else
    echo "    web/*.py unchanged → keep recorder-web running (no SSE drop)"
fi

echo "[2/8] python deps"
if ! python3 -c 'import flask, gunicorn' 2>/dev/null; then
  SUDO pip3 install -r "${WEB_DST}/requirements.txt"
else
  echo "    flask + gunicorn already present, skip"
fi

echo "[3/8] install start-foreground.sh"
SUDO install -m 0755 -o root -g root \
  "${SCRIPT_SRC}/start-foreground.sh" /opt/recorder/scripts/start-foreground.sh

# Service unit files in the repo are templated with User=ftadmin /
# /home/ftadmin. If the deploy user is different, materialise a renamed
# copy under /tmp before installing.
_render_unit() {
    local src="$1" out="$2"
    if [ "${RUN_USER}" = "ftadmin" ]; then
        cp "$src" "$out"
    else
        sed -e "s/^User=ftadmin/User=${RUN_USER}/" \
            -e "s/^Group=ftadmin/Group=${RUN_USER}/" \
            -e "s|/home/ftadmin|/home/${RUN_USER}|g" \
            "$src" > "$out"
    fi
}

echo "[4/8] install / refresh recorder-core.service"
NEED_CORE_RESTART=0
CORE_UNIT_DST=/etc/systemd/system/recorder-core.service
CORE_UNIT_TMP=/tmp/recorder-core.service.rendered
_render_unit "${WEB_DST}/recorder-core.service" "$CORE_UNIT_TMP"
if [ ! -f "$CORE_UNIT_DST" ] || ! cmp -s "$CORE_UNIT_TMP" "$CORE_UNIT_DST"; then
    SUDO cp "$CORE_UNIT_TMP" "$CORE_UNIT_DST"
    NEED_CORE_RESTART=1
    echo "    unit changed → will restart recorder-core (active call WILL drop)"
else
    echo "    unit unchanged → keep recorder-core running (active call safe)"
fi
rm -f "$CORE_UNIT_TMP"

echo "[5/8] install / refresh recorder-web.service"
WEB_UNIT_DST=/etc/systemd/system/recorder-web.service
WEB_UNIT_TMP=/tmp/recorder-web.service.rendered
_render_unit "${WEB_DST}/recorder-web.service" "$WEB_UNIT_TMP"
if [ ! -f "$WEB_UNIT_DST" ] || ! cmp -s "$WEB_UNIT_TMP" "$WEB_UNIT_DST"; then
    SUDO cp "$WEB_UNIT_TMP" "$WEB_UNIT_DST"
    NEED_WEB_RESTART=1
    echo "    unit changed → will restart recorder-web"
fi
rm -f "$WEB_UNIT_TMP"

echo "[6/8] install recorder-restart path/service units (sudoless restart trigger)"
# web 写 /opt/recorder/run/restart-recorder.flag → systemd path unit (root)
# 监听 close-after-write → 触发 service unit 跑 `systemctl restart recorder-core`
# web 进程因此不需要任何 sudo / sudoers 权限。
SUDO cp "${WEB_DST}/recorder-restart.path"    /etc/systemd/system/recorder-restart.path
SUDO cp "${WEB_DST}/recorder-restart.service" /etc/systemd/system/recorder-restart.service
SUDO mkdir -p /opt/recorder/run
SUDO chown ${RUN_USER}:${RUN_USER} /opt/recorder/run
# 清理早期版本遗留的 sudoers drop-in（已废弃，曾因 CRLF 导致 sudo 锁死）
SUDO rm -f /etc/sudoers.d/recorder-web

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

echo "[8/8] enable + conditional restart"
SUDO systemctl enable recorder-core 2>/dev/null
if [ "$NEED_CORE_RESTART" = "1" ]; then
    SUDO systemctl restart recorder-core
    echo "    recorder-core restarted"
else
    echo "    recorder-core left running — active H.323 call (if any) untouched"
fi
SUDO systemctl enable recorder-web 2>/dev/null
if [ "$NEED_WEB_RESTART" = "1" ]; then
    SUDO systemctl restart recorder-web
    echo "    recorder-web restarted"
else
    echo "    recorder-web left running — SSE / live caption stream untouched"
fi
SUDO systemctl enable --now recorder-restart.path 2>/dev/null

# auth.json bootstrap — only when missing
if [ ! -f "${WEB_DST}/auth.json" ]; then
  echo '{"users":{}}' | SUDO tee "${WEB_DST}/auth.json" >/dev/null
  SUDO chmod 600 "${WEB_DST}/auth.json"
  SUDO chown ${RUN_USER}:${RUN_USER} "${WEB_DST}/auth.json"
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
