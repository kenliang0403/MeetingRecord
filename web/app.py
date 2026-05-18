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
  GET  /api/transcript/stream   SSE 推送 ASR 实时字幕（live 页面订阅）
  GET  /recordings/<m>/transcript.json   回放页字幕同步用（按 meeting timeline）
  POST /recordings/<m>/transcript/refine 调 LLM 保守纠错字幕→ transcript.refined.jsonl
  GET  /recordings/<m>/summary           读已生成的会议纪要 markdown
  POST /recordings/<m>/summary           调 LLM 生成会议纪要并保存 summary.md

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
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta
from pathlib import Path

from flask import (
    Flask, render_template, request, redirect, url_for,
    session, flash, jsonify, send_file, abort, Response,
)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from auth import authenticate, login_required, AUTH_FILE, load_users  # noqa: E402
from recorder_client import RecorderClient  # noqa: E402


# --- Config -----------------------------------------------------------

RECORDINGS_DIR = os.environ.get("RECORDINGS_DIR", "/opt/recorder/recordings")
CONFIG_PATH    = os.environ.get("CONFIG_PATH",    "/opt/recorder/config/config.json")
TRANSCRIPT_LIVE = Path(os.environ.get(
    "TRANSCRIPT_LIVE_FILE", "/opt/recorder/run/transcript.jsonl"))
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
    ("gk_alias",    "终端 Alias",    "text",     ("gk", "alias"),    "例如 <alias-1>（同时作为 E.164 号注册到 GK，e164 字段自动跟随）"),
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

    # ── 大模型（生成会议纪要用） ──────────────────────────────────
    ("__group_llm", "大模型 API（会议纪要后处理用）", None, None, None),
    ("llm_base_url", "Base URL",   "text",     ("llm", "base_url"), "例如 https://api.deepseek.com（OpenAI 兼容协议）"),
    ("llm_api_key",  "API Key",    "password", ("llm", "api_key"),  "留空保留原值；输入新值覆盖"),
    ("llm_model",    "模型名",     "text",     ("llm", "model"),    "例如 deepseek-chat"),

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

    # H.323 GK 注册要求 alias 和 e164 一致；UI 只暴露 alias，这里强制同步 e164
    # 防止只改 alias 不改 e164 导致 GK 拒绝注册（2026-05-07 踩过这个坑）。
    if "gk" in cfg and "alias" in cfg["gk"]:
        cfg["gk"]["e164"] = cfg["gk"]["alias"]

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


# --- ASR transcript SSE -----------------------------------------------------
# recorder-asr-bridge appends one JSONL line per ASR update (partial or final)
# to /opt/recorder/run/transcript.jsonl. We tail it and push each line as a
# Server-Sent Events 'data:' frame so live.js can render captions.
# New connections start at the *end* of the file — clients don't get history.

@app.route("/api/transcript/stream")
@login_required
def transcript_stream():
    def gen():
        path = TRANSCRIPT_LIVE
        # 文件可能还不存在（bridge 还没写过任何字幕）；轮询等待
        try:
            pos = path.stat().st_size
        except (OSError, FileNotFoundError):
            pos = 0

        last_keepalive = time.time()
        # 立即发一个 hello 让 EventSource onopen 触发，便于前端区分"连上"和"断开"
        yield ": hello\n\n"

        while True:
            try:
                size = path.stat().st_size
            except (OSError, FileNotFoundError):
                size = 0
                pos = 0   # 文件被删/轮转，从头开始
            if size < pos:
                # 文件被截断（清空）：从 0 重新开始
                pos = 0
            if size > pos:
                try:
                    with path.open("rb") as f:
                        f.seek(pos)
                        chunk = f.read(size - pos)
                    pos = size
                    for raw in chunk.splitlines():
                        line = raw.strip()
                        if line:
                            yield f"data: {line.decode('utf-8', 'replace')}\n\n"
                    last_keepalive = time.time()
                    continue
                except OSError:
                    pass
            # 没新内容：必要时发心跳
            if time.time() - last_keepalive > 25:
                yield ": ping\n\n"
                last_keepalive = time.time()
            time.sleep(0.25)

    headers = {
        "Cache-Control": "no-cache",
        "X-Accel-Buffering": "no",   # 防止反代 buffer 推送
    }
    return Response(gen(), mimetype="text/event-stream", headers=headers)


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


def _read_meeting_t0_ms(target_dir):
    """Earliest main_*.mp4 wall_start_ms from meeting.json (or None)."""
    mj = target_dir / "meeting.json"
    if not mj.exists():
        return None
    try:
        data = json.loads(mj.read_text(encoding="utf-8"))
        starts = [
            s.get("wall_start_ms")
            for s in (data.get("segments") or [])
            if (s.get("file") or "").startswith("main_") and s.get("wall_start_ms")
        ]
        return min(starts) if starts else None
    except Exception:
        return None


def _build_canonical_finals(target_dir):
    """Read transcript.jsonl, dedup punct vs raw, return ordered list of finals.

    For each segment we pair the original raw final (carries `timestamps`
    + per-char timing) with the optional later punct refinement (carries
    cleaner `text`). Output line per segment uses raw's timestamps with
    punct's text when available — gives the frontend both per-char timing
    AND punctuated text in one item, which player.js needs for incremental
    timestamp-driven caption display.

    Each item: {t, text, punct, segment, timestamps, start_time, t0_raw}
    """
    jsonl = target_dir / "transcript.jsonl"
    if not jsonl.exists():
        return []
    finals = []
    try:
        with jsonl.open(encoding="utf-8") as f:
            for line in f:
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                if not d.get("is_final"):
                    continue
                finals.append(d)
    except OSError:
        return []
    raw_by_seg = {}      # seg -> first raw final dict
    punct_by_seg = {}    # seg -> last punct dict
    for d in finals:
        if d.get("punct"):
            key = d.get("replaces_segment", d.get("segment"))
            punct_by_seg[key] = d
        else:
            seg = d.get("segment", -1)
            if seg not in raw_by_seg:
                raw_by_seg[seg] = d
    out = []
    for seg, raw in raw_by_seg.items():
        text = raw.get("text", "")
        is_punct = False
        if seg in punct_by_seg:
            ptext = punct_by_seg[seg].get("text", "")
            if ptext:
                text = ptext
                is_punct = True
        out.append({
            "t":          float(raw.get("t", 0.0)),
            "text":       text,
            "punct":      is_punct,
            "segment":    seg,
            "timestamps": raw.get("timestamps") or [],
            "start_time": raw.get("start_time"),
        })
    out.sort(key=lambda d: d["t"])
    return out


@app.route("/recordings/<meeting_dir>/transcript.json")
@login_required
def meeting_transcript(meeting_dir):
    """Return final ASR transcripts aligned to the meeting timeline.

    If transcript.refined.jsonl (LLM-polished) exists, prefer its `text`
    over the raw transcript.jsonl text; timing/segment metadata still comes
    from the raw file. Original transcript.jsonl is never modified.

    `meeting_offset_s` is anchored to the earliest main_*.mp4 wall_start_ms
    so captions line up with player.js's segment timeline (handles multi-
    segment meetings — break-and-rejoin produces multiple main_NN.mp4, each
    carrying its own meeting_offset_ms; this single transcript file with
    absolute t covers them all).
    """
    base = Path(RECORDINGS_DIR)
    target = (base / meeting_dir).resolve()
    if not str(target).startswith(str(base.resolve())):
        abort(404)
    if not target.exists() or not target.is_dir():
        abort(404)

    raw_finals = _build_canonical_finals(target)
    if not raw_finals:
        return jsonify({"ok": True, "items": [], "t0_ms": None,
                        "source": "none", "has_refined": False})

    # 优化版：跟 raw_finals 一一对应（同序、同 t），只 text 可能改了
    refined_path = target / "transcript.refined.jsonl"
    has_refined = refined_path.exists()
    refined_text_by_t = {}
    if has_refined:
        try:
            with refined_path.open(encoding="utf-8") as f:
                for line in f:
                    try:
                        d = json.loads(line)
                    except Exception:
                        continue
                    if "t" in d and "text" in d:
                        refined_text_by_t[round(float(d["t"]), 3)] = d["text"]
        except OSError:
            has_refined = False

    use_refined = bool(request.args.get("refined", "1") == "1" and has_refined)

    t0_ms = _read_meeting_t0_ms(target)
    first_t = raw_finals[0]["t"]
    items = []
    for d in raw_finals:
        t = d["t"]
        offset_s = (t - t0_ms / 1000.0) if t0_ms is not None else (t - first_t)
        text = d["text"]
        is_refined = False
        if use_refined:
            r = refined_text_by_t.get(round(t, 3))
            if r:
                text = r
                is_refined = True
        items.append({
            "t":                t,
            "meeting_offset_s": round(offset_s, 3),
            "text":             text,
            "punct":            d["punct"],
            "segment":          d["segment"],
            "refined":          is_refined,
            # per-char timing relative to segment start_time. Empty list if
            # bridge dropped them (rare). player.js uses this for
            # incremental "typing" caption display.
            "timestamps":       d.get("timestamps") or [],
            "start_time":       d.get("start_time"),
        })
    items.sort(key=lambda x: x["meeting_offset_s"])

    return jsonify({
        "ok": True,
        "items": items,
        "t0_ms": t0_ms,
        "source": "refined" if use_refined else "raw",
        "has_refined": has_refined,
    })


@app.route("/recordings/<meeting_dir>/transcript/refine", methods=["POST"])
@login_required
def refine_meeting_transcript(meeting_dir):
    """Run the configured LLM as a *conservative* speech-to-text fixer.

    Reads transcript.jsonl finals (post-punct dedup), asks the LLM to fix
    only obvious homophone errors and add light punctuation. Strict rules:
      - 输出句数 = 输入句数（一一对应，不合并/不拆分）
      - 不改数字、人名、专有名词（除非明显同音错）
      - 不增删内容、不修正发言人本身的口误事实
    Result written to transcript.refined.jsonl alongside the raw file
    (raw is never modified). Caller's GET will then prefer refined.text.
    """
    base = Path(RECORDINGS_DIR)
    target = (base / meeting_dir).resolve()
    if not str(target).startswith(str(base.resolve())):
        abort(404)
    if not target.exists() or not target.is_dir():
        abort(404)

    try:
        cfg = json.loads(Path(CONFIG_PATH).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        return jsonify({"ok": False, "error": f"读 config 失败：{e}"}), 500
    llm = cfg.get("llm") or {}
    base_url = (llm.get("base_url") or "").strip()
    api_key  = (llm.get("api_key")  or "").strip()
    model    = (llm.get("model")    or "").strip()
    if not (base_url and api_key and model):
        return jsonify({"ok": False,
                        "error": "请先在【配置】页填写 LLM Base URL / API Key / 模型名"}), 400

    finals = _build_canonical_finals(target)
    if not finals:
        return jsonify({"ok": False,
                        "error": "transcript.jsonl 不存在或没有 final 句"}), 400

    system_prompt = (
        "你是字幕保守纠错助手。会收到一段语音识别（ASR）的转录，每行一句，前面是序号 [N]。\n"
        "你的任务是仅做轻量纠错，绝对不做内容创作。\n\n"
        "【可以做的修改】\n"
        "- 修复明显的同音字错（如『基调一处』→『基础教育一处』、『社交委』→『市教委』）\n"
        "- 修复明显的形近字错\n"
        "- 补全或修正中文标点（句号、逗号、问号），让句子更易读\n\n"
        "【绝对不可以做的修改】\n"
        "- 不修改数字、金额、日期、时间\n"
        "- 不修改人名、机构名、专有名词（除非是明显的同音错字，如『市交委』→『市教委』）\n"
        "- 不修改语义，不改写句式\n"
        "- 不增加任何内容（哪怕是过渡词、连接词）\n"
        "- 不删除任何实词\n"
        "- 不合并多句、不拆分单句（保持每句一一对应）\n"
        "- 不修正发言人原本说错的事实（哪怕你认为他说错了）\n"
        "- 不输出会议总结或分析\n\n"
        "输出严格使用 JSON 对象格式：\n"
        '{"refined": ["第0句修订后", "第1句修订后", "..."]}\n'
        "数组长度必须等于输入的句数。每个元素是对应输入句的修订版本。\n"
        "如果某句无需修改，原样返回。"
    )

    # 分批处理：单批一次性发 240 句会让 LLM 输出 token 数超过 max_tokens
    # 默认 (4096) → 数组被截断 → 长度对不上 → 我们整个 abort。
    # 拆成 batch_size=50 一批后，每批输出 ~2-3k token，安全得多。
    # 任意一批失败就 abort（保证原子性：要么全 refined 要么不动）。
    batch_size = int(os.environ.get("ASR_REFINE_BATCH_SIZE", "50"))
    total_batches = (len(finals) + batch_size - 1) // batch_size
    all_refined = []

    for batch_idx in range(total_batches):
        batch = finals[batch_idx * batch_size : (batch_idx + 1) * batch_size]
        numbered = "\n".join(f"[{i}] {f['text']}" for i, f in enumerate(batch))
        user_prompt = (
            f"以下 {len(batch)} 句 ASR 转录"
            f"（第 {batch_idx + 1} / {total_batches} 批，共 {len(finals)} 句），"
            f"请逐句保守纠错（同音字 + 标点）：\n\n{numbered}"
        )
        text, err = _llm_chat_complete(
            base_url, api_key, model,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user",   "content": user_prompt},
            ],
            timeout=180,
            response_format={"type": "json_object"},
        )
        if err:
            return jsonify({
                "ok": False,
                "error": f"LLM 调用失败（第 {batch_idx+1}/{total_batches} 批）：{err}",
            }), 502
        try:
            data = json.loads(text)
            refined = data.get("refined")
            if not isinstance(refined, list) or len(refined) != len(batch):
                got = len(refined) if isinstance(refined, list) else "?"
                return jsonify({
                    "ok": False,
                    "error": f"LLM 第 {batch_idx+1}/{total_batches} 批输出长度 "
                             f"{got} != 输入 {len(batch)}，已放弃。原始字幕未改动。",
                }), 502
        except (json.JSONDecodeError, AttributeError) as e:
            return jsonify({
                "ok": False,
                "error": f"LLM 第 {batch_idx+1}/{total_batches} 批输出不是合法 JSON：{e}",
            }), 502
        all_refined.extend(refined)

    refined = all_refined

    # 写 transcript.refined.jsonl（保留原始 jsonl 不动）
    out = target / "transcript.refined.jsonl"
    try:
        tmp = str(out) + ".tmp"
        with Path(tmp).open("w", encoding="utf-8") as f:
            for d, new_text in zip(finals, refined):
                row = {
                    "t":       d["t"],
                    "text":    str(new_text or "").strip(),
                    "segment": d["segment"],
                    "refined": True,
                    "model":   model,
                }
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
        os.replace(tmp, str(out))
    except OSError as e:
        return jsonify({"ok": False, "error": f"写 refined 文件失败：{e}"}), 500

    # 统计有多少句被改了（提示用户）
    changed = sum(
        1 for d, new_text in zip(finals, refined)
        if str(new_text or "").strip() != d["text"].strip()
    )
    return jsonify({
        "ok": True,
        "lines_total":   len(finals),
        "lines_changed": changed,
        "model":         model,
    })


# --- LLM-driven meeting summary --------------------------------------------
# 通用 OpenAI 兼容协议 /v1/chat/completions，支持 DeepSeek / 通义 / 火山等。
# 不引入 requests 依赖，纯 stdlib urllib。

def _llm_chat_complete(base_url, api_key, model, messages, timeout=180, response_format=None):
    """Call OpenAI-compatible chat completions. Returns (text, error_str).

    response_format e.g. {"type": "json_object"} — DeepSeek/通义/openai
    都支持，强制模型输出合法 JSON。
    """
    url = base_url.rstrip("/") + "/v1/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "stream": False,
    }
    if response_format:
        payload["response_format"] = response_format
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        try:
            err_body = e.read().decode("utf-8", "replace")[:500]
        except Exception:
            err_body = ""
        return None, f"HTTP {e.code}: {err_body or e.reason}"
    except urllib.error.URLError as e:
        return None, f"网络错误：{e.reason}"
    except Exception as e:
        return None, f"{type(e).__name__}: {e}"
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None, f"响应不是 JSON：{raw[:300]}"
    if "error" in data:
        return None, f"API 返回错误：{data['error']}"
    try:
        return data["choices"][0]["message"]["content"], None
    except (KeyError, IndexError, TypeError):
        return None, f"响应字段缺失：{raw[:300]}"


def _read_meeting_finals(meeting_dir_path, prefer_refined=True):
    """Return (sentences, meeting_name). 复用 _build_canonical_finals。

    prefer_refined: 如果 transcript.refined.jsonl 存在，把对应 t 的句子换成
    refined 版本。raw 永不被改动；这只是给 summary endpoint 喂更准的输入。
    """
    finals = _build_canonical_finals(meeting_dir_path)
    if prefer_refined:
        refined_path = meeting_dir_path / "transcript.refined.jsonl"
        if refined_path.exists():
            rmap = {}
            try:
                with refined_path.open(encoding="utf-8") as f:
                    for line in f:
                        try:
                            d = json.loads(line)
                        except Exception:
                            continue
                        if "t" in d and "text" in d:
                            rmap[round(float(d["t"]), 3)] = d["text"]
            except OSError:
                rmap = {}
            for d in finals:
                r = rmap.get(round(d["t"], 3))
                if r:
                    d["text"] = r
    sentences = [d["text"].strip() for d in finals if d["text"].strip()]
    meeting_name = ""
    mj = meeting_dir_path / "meeting.json"
    if mj.exists():
        try:
            meeting_name = (json.loads(mj.read_text(encoding="utf-8"))
                            .get("meeting_name") or "")
        except Exception:
            pass
    return sentences, meeting_name


@app.route("/recordings/<meeting_dir>/summary", methods=["GET"])
@login_required
def get_meeting_summary(meeting_dir):
    base = Path(RECORDINGS_DIR)
    target = (base / meeting_dir).resolve()
    if not str(target).startswith(str(base.resolve())):
        abort(404)
    if not target.exists():
        abort(404)
    p = target / "summary.md"
    if not p.exists():
        return jsonify({"ok": True, "exists": False, "text": ""})
    try:
        return jsonify({"ok": True, "exists": True,
                        "text": p.read_text(encoding="utf-8"),
                        "mtime": p.stat().st_mtime})
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.route("/recordings/<meeting_dir>/summary", methods=["POST"])
@login_required
def generate_meeting_summary(meeting_dir):
    """Call configured LLM to produce a meeting summary from transcript.jsonl."""
    base = Path(RECORDINGS_DIR)
    target = (base / meeting_dir).resolve()
    if not str(target).startswith(str(base.resolve())):
        abort(404)
    if not target.exists() or not target.is_dir():
        abort(404)

    try:
        cfg = json.loads(Path(CONFIG_PATH).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        return jsonify({"ok": False, "error": f"读 config 失败：{e}"}), 500
    llm = cfg.get("llm", {}) or {}
    base_url = (llm.get("base_url") or "").strip()
    api_key  = (llm.get("api_key")  or "").strip()
    model    = (llm.get("model")    or "").strip()
    if not (base_url and api_key and model):
        return jsonify({
            "ok": False,
            "error": "请先在【配置】页填写 LLM Base URL / API Key / 模型名",
        }), 400

    sentences, meeting_name = _read_meeting_finals(target)
    if not sentences:
        return jsonify({"ok": False,
                        "error": "该会议没有可用的 ASR 转录（transcript.jsonl 不存在或为空）"}), 400
    full_text = "\n".join(sentences)

    system_prompt = (
        "你是会议纪要助手。用户会提供一份语音识别（ASR）的转录文本，"
        "请基于它生成结构化的会议纪要。\n\n"
        "注意：ASR 可能存在错字、缺字（如人名/术语的同音字错误），"
        "请基于上下文推断纠正后再写纪要，不要原样照抄明显错的字。\n\n"
        "输出严格使用 Markdown 格式，包含以下小节（无内容的小节直接省略）：\n\n"
        "## 会议主题\n"
        "## 参会人员\n"
        "## 议题与讨论要点\n"
        "## 决议与结论\n"
        "## 行动项（负责人 / 时限）\n"
        "## 备注\n"
    )
    user_prompt = (
        f"会议名称：{meeting_name or meeting_dir}\n\n"
        f"ASR 转录全文（按时间顺序，每行一句）：\n\n{full_text}"
    )

    text, err = _llm_chat_complete(
        base_url, api_key, model,
        messages=[
            {"role": "system", "content": system_prompt},
            {"role": "user",   "content": user_prompt},
        ],
        timeout=180,
    )
    if err:
        return jsonify({"ok": False, "error": f"LLM 调用失败：{err}"}), 502

    out = target / "summary.md"
    try:
        tmp = str(out) + ".tmp"
        Path(tmp).write_text(text or "", encoding="utf-8")
        os.replace(tmp, str(out))
    except OSError as e:
        return jsonify({"ok": False, "error": f"写 summary.md 失败：{e}"}), 500

    return jsonify({
        "ok": True,
        "text": text,
        "transcript_lines": len(sentences),
        "model": model,
    })


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
