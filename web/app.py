"""recorder-core 管理页 — Flask 单进程，HTTP 8088。

Endpoints:
  GET  /                 dashboard (登录后)
  GET  /login            登录表单
  POST /login            校验
  GET  /logout
  GET  /config           配置页
  GET  /recordings       回放索引
  GET  /recordings/<m>   单会议回放页
  GET  /play/<m>/<f>     文件 (Range supported)

API (login required):
  GET  /api/status
  GET  /api/levels
  GET  /api/config
  GET  /api/recordings

设计要点：
  - 30 分钟 idle 自动登出 (PERMANENT_SESSION_LIFETIME)
  - 密码哈希存 /opt/recorder/web/auth.json，浏览器只见 session cookie
  - HLS 流不经 Flask 反代，前端 JS 直连 SRS:8080，省带宽
"""
import os
import sys
import socket
from datetime import timedelta
from pathlib import Path

from flask import (
    Flask, render_template, request, redirect, url_for,
    session, flash, jsonify, send_file, abort,
)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from auth import authenticate, login_required, AUTH_FILE, load_users  # noqa: E402
from recorder_client import RecorderClient  # noqa: E402


# --- Config -----------------------------------------------------------

RECORDINGS_DIR = os.environ.get("RECORDINGS_DIR", "/opt/recorder/recordings")
SRS_HOST       = os.environ.get("SRS_HOST", "")  # 空字符串=用 request.host (浏览器视角)
SRS_PORT       = int(os.environ.get("SRS_PORT", "8080"))
MAIN_STREAM    = os.environ.get("STREAM_MAIN", "recorder-main")
AUX_STREAM     = os.environ.get("STREAM_AUX",  "recorder-aux")

SESSION_TIMEOUT_MIN = int(os.environ.get("SESSION_TIMEOUT_MIN", "30"))


# --- App --------------------------------------------------------------

app = Flask(__name__, template_folder="templates", static_folder="static")
# SECRET_KEY 必须稳定（重启会让所有人登出但不影响安全），从文件读或随机生成
_secret_path = "/opt/recorder/web/.flask_secret"
if os.path.exists(_secret_path):
    app.secret_key = Path(_secret_path).read_bytes()
else:
    app.secret_key = os.urandom(32)
    try:
        Path(_secret_path).write_bytes(app.secret_key)
        os.chmod(_secret_path, 0o600)
    except OSError:
        pass  # not writable, fall back to in-memory key

app.permanent_session_lifetime = timedelta(minutes=SESSION_TIMEOUT_MIN)
app.config["SESSION_COOKIE_HTTPONLY"] = True
app.config["SESSION_COOKIE_SAMESITE"] = "Lax"

client = RecorderClient(host="127.0.0.1", port=9001)


# --- Helpers ----------------------------------------------------------

def srs_base_url() -> str:
    """浏览器侧拉 HLS 用的 base URL。SRS_HOST 为空则取 request.host 的主机部分。"""
    host = SRS_HOST
    if not host:
        host = request.host.split(":")[0]
    return f"http://{host}:{SRS_PORT}"


# --- Auth routes ------------------------------------------------------

@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        if authenticate(username, password):
            session.clear()
            session.permanent = True
            session["user"] = username
            nxt = request.args.get("next") or url_for("dashboard")
            return redirect(nxt)
        flash("用户名或密码错误", "error")
    return render_template("login.html")


@app.route("/logout")
def logout():
    session.clear()
    flash("已登出", "info")
    return redirect(url_for("login"))


# --- Pages ------------------------------------------------------------

@app.route("/")
@login_required
def dashboard():
    return render_template(
        "dashboard.html",
        srs_base=srs_base_url(),
        main_stream=MAIN_STREAM,
        aux_stream=AUX_STREAM,
    )


@app.route("/config")
@login_required
def config_page():
    cfg = client.config()
    return render_template("config.html", cfg=cfg)


@app.route("/recordings")
@login_required
def recordings_page():
    res = client.recordings()
    items = res.get("data", []) if res.get("ok") else []
    return render_template("recordings.html", items=items, error=res.get("error"))


@app.route("/recordings/<meeting_dir>")
@login_required
def meeting_page(meeting_dir):
    base = Path(RECORDINGS_DIR)
    target = (base / meeting_dir).resolve()
    if not str(target).startswith(str(base.resolve())):
        abort(404)
    if not target.exists():
        abort(404)

    files = sorted(
        [f.name for f in target.iterdir() if f.suffix == ".mp4"],
        key=lambda n: (
            0 if n.startswith("main_") else 1 if n.startswith("aux_") else 2,
            n,
        ),
    )
    main_files = [f for f in files if f.startswith("main_")]
    aux_files  = [f for f in files if f.startswith("aux_")]

    meeting_json = None
    j = target / "meeting.json"
    if j.exists():
        import json
        try:
            meeting_json = json.loads(j.read_text(encoding="utf-8"))
        except Exception:
            pass

    return render_template(
        "meeting.html",
        meeting_dir=meeting_dir,
        main_files=main_files,
        aux_files=aux_files,
        meeting_json=meeting_json,
    )


@app.route("/play/<meeting_dir>/<fname>")
@login_required
def play_file(meeting_dir, fname):
    base = Path(RECORDINGS_DIR)
    target = (base / meeting_dir / fname).resolve()
    if not str(target).startswith(str(base.resolve())):
        abort(404)
    if not target.exists() or target.suffix != ".mp4":
        abort(404)
    return send_file(str(target), mimetype="video/mp4", conditional=True)


# --- JSON API ---------------------------------------------------------

@app.route("/api/status")
@login_required
def api_status():
    return jsonify(client.status())


@app.route("/api/levels")
@login_required
def api_levels():
    return jsonify(client.audio_levels())


@app.route("/api/config")
@login_required
def api_config():
    return jsonify(client.config())


@app.route("/api/recordings")
@login_required
def api_recordings():
    return jsonify(client.recordings())


@app.route("/healthz")
def healthz():
    """无需登录：方便监控/反代健康检查。仅返回 web 自身存活，不查 recorder-core。"""
    return ("ok", 200)


# --- Main -------------------------------------------------------------

if __name__ == "__main__":
    # 开发模式 — 生产用 gunicorn
    port = int(os.environ.get("PORT", "8088"))
    app.run(host="0.0.0.0", port=port, debug=False)
