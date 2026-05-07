"""recorder-core 管理页 — Flask 单进程，HTTP 8088。

Endpoints:
  GET  /                 dashboard (登录后)
  GET  /login            登录表单
  POST /login            校验
  GET  /logout
  GET  /config           配置编辑页（textarea + 保存 + 重启按钮）
  POST /config/save      保存 JSON 到 config.json
  POST /config/restart   重启 recorder-core 服务（写触发文件，systemd path unit 执行）
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
  - 配置 / 重启走 web 进程本地 — 重启用触发文件 + systemd path unit，web 不需要 sudo
"""
import json
import os
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


@app.route("/live")
@login_required
def live_page():
    return render_template(
        "live.html",
        srs_base=srs_base_url(),
        main_stream=MAIN_STREAM,
        aux_stream=AUX_STREAM,
    )


# --- 字段化配置：只暴露常用项；保存时 read-merge-write，其它字段原封不动 ---
#
# 每条 = (form name, 中文标签, 类型, JSON 路径, 提示文案/选项)
# 类型：text / password / int / float / checkbox / select
# 路径：tuple，允许嵌套，如 ("gk","host") 表示 cfg["gk"]["host"]
# password 字段 placeholder 显示 "******" 表示已设；提交空字符串= 不修改
EDITABLE_FIELDS = [
    # ── 网关与终端身份 ────────────────────────────────────────────
    ("__group_gk", "网关 GK / 终端身份", None, None, None),
    ("gk_host",     "GK 服务器地址", "text",     ("gk", "host"),     "例如 <gk_host>"),
    ("gk_alias",    "终端 Alias",    "text",     ("gk", "alias"),    "例如 <alias-1>"),
    ("gk_password", "GK 密码",       "password", ("gk", "password"), "留空保留原值；输入新值覆盖"),
    ("terminal_id", "终端显示名",    "text",     ("terminal_id",),   "MCU 名册中显示的中文名"),

    # ── 主动呼叫 ─────────────────────────────────────────────────
    ("__group_outgoing", "主动呼叫（开机自动拨号）", None, None, None),
    ("outgoing_enabled", "启用",       "checkbox", ("outgoing", "enabled"),     ""),
    ("outgoing_dial",    "默认呼叫号",  "text",     ("outgoing", "dial_number"), "例如 <dial-number>"),
    ("outgoing_mcu",     "MCU 直连 IP", "text",     ("outgoing", "mcu_host"),    "留空走 GK 路由"),

    # ── 视频录制质量 ─────────────────────────────────────────────
    ("__group_video", "视频录制", None, None, None),
    ("video_width",   "视频宽度",         "int", ("recorder", "video_width"),   "默认 1920"),
    ("video_height",  "视频高度",         "int", ("recorder", "video_height"),  "默认 1080"),
    ("video_fps",     "帧率 FPS",         "int", ("recorder", "video_fps"),     "默认 30"),
    ("video_bitrate", "视频比特率 (bps)", "int", ("recorder", "video_bitrate"), "默认 2000000"),

    # ── 音频录制质量 ─────────────────────────────────────────────
    ("__group_audio", "音频录制", None, None, None),
    ("audio_bitrate", "音频比特率 (bps)", "int",   ("recorder", "audio_bitrate"), "推荐 128000"),
    ("audio_gain",    "音量增益倍数",     "float", ("recorder", "audio_gain"),    "1.00 原始 / 1.10 提亮 +0.83 dB / 2.0=+6 dB"),

    # ── 直播推流 ─────────────────────────────────────────────────
    ("__group_stream", "直播推流（SRS）", None, None, None),
    ("stream_enabled",  "启用直播",     "checkbox", ("streaming", "enabled"),  ""),
    ("stream_push_aux", "包含辅流（演示）", "checkbox", ("streaming", "push_aux"), ""),

    # ── 杂项 ────────────────────────────────────────────────────
    ("__group_misc", "其它", None, None, None),
    ("auto_send_video", "自动发送主流视频（测试用）", "checkbox", ("auto_send_video",), "建立通话后立即推屏保画面"),
    ("log_level",       "日志级别",                  "select",   ("log_level",),       ["trace", "debug", "info", "warn", "error"]),
]


def _get_path(cfg, path):
    cur = cfg
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur


def _set_path(cfg, path, value):
    cur = cfg
    for k in path[:-1]:
        if k not in cur or not isinstance(cur[k], dict):
            cur[k] = {}
        cur = cur[k]
    cur[path[-1]] = value


def _coerce(typ, raw, options=None):
    """Form 字符串转目标类型；失败返回 None + 错误消息。"""
    try:
        if typ == "text":
            return raw, None
        if typ == "password":
            return raw, None
        if typ == "int":
            return int(raw), None
        if typ == "float":
            return float(raw), None
        if typ == "checkbox":
            return (raw == "on"), None
        if typ == "select":
            if options and raw not in options:
                return None, f"非法选项: {raw}"
            return raw, None
    except ValueError as e:
        return None, str(e)
    return raw, None


def _build_field_view(cfg):
    """填出当前值，渲染用。"""
    out = []
    for name, label, typ, path, extra in EDITABLE_FIELDS:
        if name.startswith("__group_"):
            out.append({"group": label})
            continue
        cur = _get_path(cfg, path)
        item = {
            "name": name, "label": label, "type": typ,
            "value": cur if cur is not None else "",
            "extra": extra,
        }
        if typ == "checkbox":
            item["checked"] = bool(cur)
        if typ == "select":
            item["options"] = extra or []
        if typ == "password":
            # 不回显原密码，仅以 placeholder 提示是否已设
            item["value"] = ""
            item["has_value"] = bool(cur)
        out.append(item)
    return out


@app.route("/config")
@login_required
def config_page():
    """字段化编辑：仅暴露常用项；其它字段透明保留。"""
    err = None
    raw_text = ""
    cfg = {}
    try:
        raw_text = Path(CONFIG_PATH).read_text(encoding="utf-8")
        cfg = json.loads(raw_text)
    except (OSError, json.JSONDecodeError) as e:
        err = f"读 {CONFIG_PATH} 失败: {e}"
    fields = _build_field_view(cfg) if not err else []
    return render_template(
        "config.html",
        fields=fields, err=err, raw_text=raw_text,
        config_path=CONFIG_PATH,
    )


@app.route("/config/save", methods=["POST"])
@login_required
def config_save():
    """字段化保存：read - merge - write。
    - 读完整 JSON
    - 仅覆盖 EDITABLE_FIELDS 中列出的路径
    - password 留空表示不修改
    - checkbox 不在 form 中表示 false
    其它任何字段一律不动，避免遗漏次要键导致 recorder 异常。
    """
    try:
        cfg = json.loads(Path(CONFIG_PATH).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        flash(f"读现有配置失败：{e}", "error")
        return redirect(url_for("config_page"))

    errors = []
    for name, label, typ, path, extra in EDITABLE_FIELDS:
        if name.startswith("__group_"):
            continue
        # checkbox 不在 form 时视为 false
        raw = request.form.get(name, "" if typ != "checkbox" else "")
        # password 留空 = 不修改
        if typ == "password" and raw == "":
            continue
        value, err = _coerce(typ, raw, extra if typ == "select" else None)
        if err:
            errors.append(f"{label}: {err}")
            continue
        _set_path(cfg, path, value)

    if errors:
        flash("保存失败：\n" + "\n".join(errors), "error")
        return redirect(url_for("config_page"))

    try:
        tmp = CONFIG_PATH + ".tmp"
        Path(tmp).write_text(
            json.dumps(cfg, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, CONFIG_PATH)
    except OSError as e:
        flash(f"写文件失败：{e}", "error")
        return redirect(url_for("config_page"))

    flash("配置已保存。改动需重启 recorder-core 才生效（点下方按钮）", "info")
    return redirect(url_for("config_page"))


@app.route("/config/save_advanced", methods=["POST"])
@login_required
def config_save_advanced():
    """高级模式：直接 textarea 改 JSON 全文（保留原 textarea 入口作后备）。"""
    text = request.form.get("payload", "")
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError as e:
        flash(f"高级模式 JSON 解析失败：{e}", "error")
        return redirect(url_for("config_page"))
    if not isinstance(parsed, dict):
        flash("JSON 顶层必须是对象 {}", "error")
        return redirect(url_for("config_page"))
    try:
        tmp = CONFIG_PATH + ".tmp"
        Path(tmp).write_text(
            json.dumps(parsed, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, CONFIG_PATH)
    except OSError as e:
        flash(f"写文件失败：{e}", "error")
        return redirect(url_for("config_page"))
    flash("高级模式：完整 JSON 已保存。改动需重启 recorder-core", "info")
    return redirect(url_for("config_page"))


RESTART_FLAG = Path(os.environ.get(
    "RECORDER_RESTART_FLAG", "/opt/recorder/run/restart-recorder.flag"))


@app.route("/config/restart", methods=["POST"])
@login_required
def config_restart():
    """触发 recorder-core 重启：写一个标志文件，root 跑的 systemd path unit
    监听到 close-after-write 后调用 `systemctl restart recorder-core`。
    web 进程因此不需要任何 sudo 权限。"""
    try:
        RESTART_FLAG.parent.mkdir(parents=True, exist_ok=True)
        RESTART_FLAG.write_text(datetime.now().isoformat(timespec="seconds"))
        flash(f"已请求重启 {RECORDER_UNIT}（约 2-3 秒后完成）", "info")
    except OSError as e:
        flash(f"重启请求失败：{e}", "error")
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

    def _sort_key(name):
        # main_NN.mp4 / aux_NN.mp4 — 取 NN 数字部分排序
        try:
            return int(name.split("_", 1)[1].split(".", 1)[0])
        except (IndexError, ValueError):
            return 0

    # 读 meeting.json 拿每段的 wall_start_ms / duration_ms（用于双流时间同步）
    meeting_json = None
    seg_meta = {}  # 文件名 → {wall_start_ms, duration_ms}
    j = target / "meeting.json"
    if j.exists():
        try:
            meeting_json = json.loads(j.read_text(encoding="utf-8"))
            for s in (meeting_json or {}).get("segments", []) or []:
                fname = s.get("file")
                if fname:
                    seg_meta[fname] = s
        except Exception:
            pass

    def _make_segments(prefix):
        segs = []
        for f in sorted(target.iterdir(), key=lambda p: _sort_key(p.name)):
            if f.is_file() and f.suffix == ".mp4" and f.name.startswith(prefix):
                meta = seg_meta.get(f.name, {})
                segs.append({
                    "name":          f.name,
                    "url":           url_for("play_file", meeting_dir=meeting_dir, fname=f.name),
                    "size_mb":       round(f.stat().st_size / (1024 * 1024), 1),
                    "wall_start_ms": meta.get("wall_start_ms"),
                    "duration_ms":   meta.get("duration_ms"),
                })
        return segs

    main_segments = _make_segments("main_")
    aux_segments  = _make_segments("aux_")

    # 计算 meeting timeline 起点 T0（取最早一段 main 的 wall_start_ms）
    main_starts = [s["wall_start_ms"] for s in main_segments if s["wall_start_ms"]]
    t0 = min(main_starts) if main_starts else None
    if t0 is not None:
        for s in main_segments + aux_segments:
            if s["wall_start_ms"] is not None:
                s["meeting_offset_ms"] = s["wall_start_ms"] - t0
            else:
                s["meeting_offset_ms"] = None

    return render_template(
        "meeting.html",
        meeting_dir=meeting_dir,
        main_segments=main_segments,
        aux_segments=aux_segments,
        meeting_t0_ms=t0,
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


# 直播页"启停本地发送"按钮的代理：白名单仅这 4 条 9001 命令
_CONTROL_WHITELIST = {
    "start_main_video", "stop_main_video",
    "start_presentation", "stop_presentation",
}


@app.route("/api/control/<cmd>", methods=["POST"])
@login_required
def api_control(cmd):
    if cmd not in _CONTROL_WHITELIST:
        return jsonify({"ok": False, "error": "command not allowed"}), 400
    return jsonify(client.call(cmd))


@app.route("/healthz")
def healthz():
    """无需登录：方便监控/反代健康检查。仅返回 web 自身存活，不查 recorder-core。"""
    return ("ok", 200)


# --- Main -------------------------------------------------------------

if __name__ == "__main__":
    # 开发模式 — 生产用 gunicorn
    port = int(os.environ.get("PORT", "8088"))
    app.run(host="0.0.0.0", port=port, debug=False)
