"""recorder-core 管理页 — Flask 单进程，HTTP 8088。

Endpoints:
  GET  /                 dashboard (登录后)
  GET  /login            登录表单
  POST /login            校验
  GET  /logout
  GET  /config           配置编辑页（textarea + 保存 + 重启按钮）
  POST /config/save      保存 JSON 到 config.json
  POST /config/restart   重启 recorder-core 服务（sudo systemctl）
  GET  /recordings       回放索引（直接扫盘）
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
  - 配置 / 重启走 web 进程本地 — sudoers 只放行单条 systemctl restart
"""
import json
import os
import subprocess
import sys
from datetime import datetime, timedelta
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
CONFIG_PATH    = os.environ.get("CONFIG_PATH",    "/opt/recorder/config/config.json")
RECORDER_UNIT  = os.environ.get("RECORDER_UNIT",  "recorder-core.service")
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
    """读 /opt/recorder/config/config.json 原文交给前端 textarea 编辑。
    若文件不存在或无权限，回退到 9001 的只读视图。"""
    raw = ""
    err = None
    runtime_view = None
    try:
        raw = Path(CONFIG_PATH).read_text(encoding="utf-8")
    except OSError as e:
        err = f"读 {CONFIG_PATH} 失败: {e} （走 9001 只读）"
        runtime_view = client.config()
    return render_template(
        "config.html",
        raw=raw, err=err, runtime_view=runtime_view,
        config_path=CONFIG_PATH,
    )


@app.route("/config/save", methods=["POST"])
@login_required
def config_save():
    """把表单提交的 JSON 文本写回 config.json。
    校验：必须是合法 JSON object。失败时仅返回错误，不动文件。"""
    text = request.form.get("payload", "")
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError as e:
        flash(f"JSON 解析失败：{e}", "error")
        return redirect(url_for("config_page"))
    if not isinstance(parsed, dict):
        flash("JSON 顶层必须是对象 {}", "error")
        return redirect(url_for("config_page"))
    try:
        # 写临时文件再 rename — 写一半断电不会破坏原文件
        tmp = CONFIG_PATH + ".tmp"
        Path(tmp).write_text(
            json.dumps(parsed, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, CONFIG_PATH)
    except OSError as e:
        flash(f"写文件失败：{e}", "error")
        return redirect(url_for("config_page"))
    flash("配置已保存。改动需重启 recorder-core 才生效（点下方按钮）", "info")
    return redirect(url_for("config_page"))


@app.route("/config/restart", methods=["POST"])
@login_required
def config_restart():
    """通过 sudo systemctl 重启 recorder-core 服务。sudoers 必须放行：
       ftadmin ALL=(root) NOPASSWD: /bin/systemctl restart recorder-core.service
    """
    # --no-block：systemctl 把请求交给 PID 1 立即返回，不等 unit 真正起来。
    # 加上 5s timeout 防御，正常情况下 < 100 ms 返回。
    try:
        proc = subprocess.run(
            ["sudo", "-n", "/bin/systemctl", "--no-block", "restart", RECORDER_UNIT],
            capture_output=True, text=True, timeout=5,
        )
        if proc.returncode != 0:
            flash(
                f"重启请求失败 (rc={proc.returncode}): "
                f"{(proc.stderr or proc.stdout).strip()}",
                "error",
            )
        else:
            flash(f"已请求重启 {RECORDER_UNIT}（约 2-3 秒后完成）", "info")
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        flash(f"重启异常：{e}", "error")
    return redirect(url_for("config_page"))


# --- Recordings: scan filesystem directly (independent of recorder-core) ----

def _scan_recordings():
    """扫 /opt/recorder/recordings/ 列出所有会议目录 + meeting.json 摘要。
    不依赖 recorder-core 9001，进程挂了/重启历史照常可见。
    """
    base = Path(RECORDINGS_DIR)
    items = []
    if not base.exists():
        return items
    for d in sorted(base.iterdir()):
        if not d.is_dir():
            continue
        info = {
            "dir_name":     d.name,
            "meeting_id":   d.name,   # fallback: 用目录名
            "meeting_name": "",
            "first_start_ts": "",
            "segments_count": 0,
            "main_count":   0,
            "aux_count":    0,
            "total_size_mb": 0,
            "mtime":        d.stat().st_mtime,
        }
        # meeting.json 优先
        j = d / "meeting.json"
        if j.exists():
            try:
                meta = json.loads(j.read_text(encoding="utf-8"))
                info["meeting_id"]     = meta.get("meeting_id")    or info["meeting_id"]
                info["meeting_name"]   = meta.get("meeting_name")  or ""
                info["first_start_ts"] = meta.get("first_start_ts") or meta.get("created_ts") or ""
                segs = meta.get("segments") or []
                info["segments_count"] = len(segs)
            except Exception:
                pass
        # 文件统计
        total_bytes = 0
        for f in d.iterdir():
            if f.is_file() and f.suffix == ".mp4":
                if f.name.startswith("main_"):
                    info["main_count"] += 1
                elif f.name.startswith("aux_"):
                    info["aux_count"] += 1
                try:
                    total_bytes += f.stat().st_size
                except OSError:
                    pass
        info["total_size_mb"] = round(total_bytes / (1024 * 1024), 1)
        if not info["first_start_ts"]:
            # 退到目录修改时间
            info["first_start_ts"] = datetime.fromtimestamp(info["mtime"]).strftime("%Y-%m-%d %H:%M:%S")
        items.append(info)
    # 最近的会议排前面
    items.sort(key=lambda x: x["mtime"], reverse=True)
    return items


@app.route("/recordings")
@login_required
def recordings_page():
    items = _scan_recordings()
    return render_template("recordings.html", items=items, error=None)


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
    return jsonify({"ok": True, "data": _scan_recordings()})


@app.route("/healthz")
def healthz():
    """无需登录：方便监控/反代健康检查。仅返回 web 自身存活，不查 recorder-core。"""
    return ("ok", 200)


# --- Main -------------------------------------------------------------

if __name__ == "__main__":
    # 开发模式 — 生产用 gunicorn
    port = int(os.environ.get("PORT", "8088"))
    app.run(host="0.0.0.0", port=port, debug=False)
