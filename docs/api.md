# 接口文档

## 1. Web HTTP API（recorder-web @ :8088）

所有 `/api/*` + `/recordings/*` 需要登录（session cookie）。`/login` 之外的请求未登录会 302 到 `/login`。

### 1.1 鉴权

| Method | Path | 说明 |
|---|---|---|
| GET | `/login` | 登录页 |
| POST | `/login` | 表单 `username=&password=` → set session cookie |
| GET | `/logout` | 销毁 session |
| GET | `/health` | 不需登录，返回 `{"ok":true}` 用于 LB 健康检查 |

Session cookie 配置：30 分钟 idle 自动登出，HttpOnly + SameSite=Lax。

### 1.2 仪表盘 / 配置 / 直播

| Method | Path | 说明 |
|---|---|---|
| GET | `/` | 仪表盘（设备状态、4 路实时状态卡、RMS/Peak VU） |
| GET | `/api/status` | JSON：当前通话状态（in_call / main_sending / main_file / has_presentation / aux_recording / meeting_id / caller_id / gk_registered / ...） |
| GET | `/api/levels` | JSON：实时音频电平（rms/peak dBFS） |
| GET | `/config` | 配置编辑页（EDITABLE_FIELDS 列表 + 高级 JSON 模式） |
| POST | `/config/save` | 字段化保存：read-merge-write `/opt/recorder/config/config.json`（**自动同步 `gk.e164 = gk.alias`**） |
| POST | `/config/save_advanced` | 高级模式：整段 JSON 替换 |
| POST | `/config/restart` | 写触发文件 `/opt/recorder/run/restart-recorder.flag` → systemd path-unit 触发 `systemctl restart recorder-core`（web 进程零 sudo 权限） |
| GET | `/live` | 直播页（hls.js + SSE 字幕条 + 字幕开关 + 延迟应用按钮） |
| GET | `/api/control/<cmd>` | 严格白名单：`start_video` / `stop_video` / `start_presentation` / `stop_presentation` — 内部转 TCP 9001 命令 |
| GET | `/api/transcript/stream` | **SSE 流**：tail `/opt/recorder/run/transcript.jsonl`，每行作为一个 `data:` event 推给浏览器；启动时不重播历史，emit `ping` 心跳每 25s |

### 1.3 录像 / 回放

| Method | Path | 说明 |
|---|---|---|
| GET | `/recordings` | 录像列表（直接扫 `/opt/recorder/recordings/`） |
| GET | `/api/recordings` | JSON 形式同上 |
| GET | `/recordings/<m>` | 单会议回放页（4 布局 + 主辅流同步 + 字幕同步 + 字幕优化按钮 + 纪要按钮） |
| GET | `/play/<m>/<f>` | 直接 send_file mp4（**Range 支持**，浏览器 video tag seek 用） |
| GET | `/recordings/<m>/transcript.json` | 字幕 JSON，按 meeting timeline 对齐。query 参数 `?refined=0` 强制返回 raw 版本；默认或 `?refined=1` 优先返回 `transcript.refined.jsonl`（如果存在） |
| **POST** | `/recordings/<m>/transcript/refine` | **LLM 保守纠错**：读 transcript.jsonl 全部 finals → 分批（默认 50 句/批）调 LLM → 写 `transcript.refined.jsonl`。原始 `transcript.jsonl` 永不修改。返回 `{ok, lines_total, lines_changed, model}` |
| GET | `/recordings/<m>/summary` | 读 `summary.md` 内容 + mtime |
| **POST** | `/recordings/<m>/summary` | **LLM 生成纪要**：拼 transcript 全文调 LLM → 写 `summary.md`。返回 `{ok, text, transcript_lines, model}` |

#### transcript.json 返回格式

```json
{
  "ok": true,
  "items": [
    {
      "t": 1779254714.196,
      "meeting_offset_s": 0.0,
      "text": "样本校的领导老师们，咱们的督学们，大家下午好啊。",
      "punct": true,
      "segment": 0,
      "refined": true,
      "timestamps": [0.32, 0.64, 0.76, ...],   // 每字相对 segment start 的秒数
      "start_time": 0.0
    },
    ...
  ],
  "t0_ms": 1779254714196,        // meeting 起点 wall clock
  "source": "refined",            // 或 "raw"
  "has_refined": true             // refined 文件是否存在
}
```

`meeting_offset_s` 是该 final 相对最早 `main_*.mp4` 的 `wall_start_ms` 的秒偏移。player.js 用 `mainSeg.meeting_offset_ms + currentTime * 1000` 算"当前在 meeting timeline 上的位置"，再 binary-search items 找当前应显示的字幕。

#### refine POST 响应

```json
{
  "ok": true,
  "lines_total": 240,
  "lines_changed": 211,
  "model": "deepseek-chat"
}
```

失败：

```json
{
  "ok": false,
  "error": "LLM 第 3/5 批输出长度 47 != 输入 50，已放弃。原始字幕未改动。"
}
```

**原子性**：任一批失败整个 abort，`transcript.refined.jsonl` 不创建（如果之前有就保留旧版）。

#### summary POST 响应

```json
{
  "ok": true,
  "text": "## 会议主题\n...\n## 议题与讨论要点\n...",
  "transcript_lines": 240,
  "model": "deepseek-chat"
}
```

### 1.4 静态文件

| Path | 说明 |
|---|---|
| `/static/*` | Flask 默认 static_folder = `web/static/` |
| `/static/style.css` `/static/player.js` `/static/live.js` `/static/dashboard.js` `/static/vu.js` | 前端资源 |

## 2. recorder-core ControlServer（TCP :9001）

**仅 127.0.0.1 监听**，无鉴权 — 任何能登本机 ssh 的用户都能操作通话。源码：`src/tcp/ControlServer.cpp` + `src/main.cpp` 各 `registerHandler(...)` 块。

### 2.1 协议

```
client → server:  {"cmd":"<command>","<arg>":"<value>",...}\n
server → client:  {"ok":true, "data":{...}}\n           # 成功带数据
                  {"ok":true, "message":"..."}\n        # 成功仅消息
                  {"ok":true, "token":"..."}\n          # dial 返回 call token
                  {"ok":false,"error":"..."}\n          # 失败
```

每个 cmd 一行 JSON，`\n` 终止。连接可复用（长连接），也可一次性 close。

**关键：必须发 JSON。** 早期 bridge 用 `b"status\n"` 被当 invalid 返回 `{"ok":false}`，silent 失败（commit `4576d4d`）。

### 2.2 命令清单（共 15 个）

按用途分组。

#### 只读 / 查询类

| cmd | 参数 | 返回 data 字段 | 说明 |
|---|---|---|---|
| `status` | — | 见 [2.3](#23-status-完整返回字段) | 完整运行状态（dashboard / bridge 查 meeting_id 都用这个） |
| `audio_levels` | — | `peak_dbfs`, `rms_dbfs`, `age_ms` | 瞬时电平（dBFS [-120, 0]）；`age_ms > 500` 表示无音频；VU 表 ~10Hz 拉取 |
| `config` | — | 见 [2.4](#24-config-返回字段) | 当前运行配置（密码字段返回 `"***"`） |
| `recordings` | — | `output_dir`, `total_files`, `recording_now`, `aux_recording`, `files[]` | **仅扫 output_dir 顶层 .mp4**，不递归进 `<meeting_id>/` 子目录；files 按 mtime 倒序 |
| `version` | — | `name`, `version`, `config` | 当前 binary 版本（硬编 `"3.1.0"`）+ 配置路径 |

#### 通话控制类

| cmd | 参数 | 行为 | 失败条件 |
|---|---|---|---|
| `dial` | `number` (**必填**), `host` (可选 — MCU 直连，绕过 GK 路由) | `endpoint.dialTo(number, host)`；返回 `{ok:true, token:"<call-token>"}` | `number` 空 → `"number is required"`；MakeCall 失败 → `"MakeCall failed — check GK registration and number"` |
| `clear_call` | `token` (可选) | 有 token：`ClearCall(token, EndedByLocalUser)`；无 token：`ClearAllCalls(...)` | — |
| `start_main_video` | — | 手动启主流屏保发送（外呼时已自动启）；调 `endpoint.startMainVideo()` | 不在通话中 → `"not in call"` |
| `stop_main_video` | — | 停主流发送 | — |
| `start_presentation` | — | H.239 主动演示：presentationTokenRequest → grant → OLC session=10 + AuxStream VideoSender | 不在通话中 → `"not in call"` |
| `stop_presentation` | — | CLC session=10 + presentationTokenRelease + 停 AuxStream | — |

> ⚠️ **没有 `hangup` 命令**。要挂断用 `clear_call`（无参数 = 挂断当前所有 call）。

#### 配置 / 维护类

| cmd | 参数 | 行为 |
|---|---|---|
| `reload` | — | 重读 `config.json`；GK 字段变了返回带 `warning` 提示需 restart |
| `set` | `key` (必), `value` (必) | 修改单个运行时配置（dot-notation key，见 [2.5](#25-set-支持的-key)）；**不写回 config.json**，只改进程内存 |
| `faststart_one` | `path` (必) | 同步重写一个 mp4 为 +faststart；路径必须以 `recorder.output_dir` 开头且不含 `..` |
| `faststart_all` | — | 后台 detached thread 扫整库（递归），仅处理 `main_*.mp4` / `aux_*.mp4`；立即返回，进度看 journal |

### 2.3 status 完整返回字段

```json
{
  "ok": true,
  "data": {
    // ─ 静态配置（来自 config.json，进程启动时加载）─
    "alias":      "<your-alias>",         // GK alias
    "gk_host":    "<gk-host>",
    "gk_port":    1719,
    "e164":       "<your-alias>",          // 通常 == alias（web 保存时自动同步）
    "output_dir": "/opt/recorder/recordings",

    // ─ 运行时（任何时候都返回）─
    "gk_registered":   true,                // RAS RRQ 是否已 ACK
    "in_call":         true,
    "call_token":      "OpalCon_xxx_xxx",   // 当前 call PTrace token；无通话 = ""
    "reconnect_count": 0,                   // outgoing 模式被重连次数

    // ─ 通话中字段（无通话时这些都是空/false/0）─
    "meeting_name":     "MCU/RemoteName",   // H.245 TerminalLabel 解析
    "caller_id":        "820715",           // 远端拨号号或 alias
    "recording":        true,               // FfmpegRecorder 是否在写
    "main_file":        "/opt/.../main_03.mp4",
    "main_sending":     true,               // 本端是否在发主视频
    "h239_received":    false,              // 远端是否在 H.239 演示
    "has_presentation": false,              // 本端是否在演示（H.239 主动）
    "aux_recording":    false,
    "aux_file":         "/opt/.../aux_02.mp4",
    "meeting_id":       "20260526_820715",  // meeting 目录名（bridge 用这个写 per-meeting jsonl）
    "meeting_dir":      "/opt/recorder/recordings/20260526_820715",
    "connection_idx":   4                   // 同一 meeting 内的连接序号（断开重连 +1）
  }
}
```

### 2.4 config 返回字段

```json
{
  "ok": true,
  "data": {
    "gk":        { "host", "port", "alias", "e164", "ttl", "username", "password" },   // password 永远 "***"
    "recorder":  { "output_dir", "video_width", "video_height", "video_fps",
                   "audio_sample_rate", "audio_channels",
                   "video_codec", "audio_codec",
                   "video_bitrate", "audio_bitrate", "audio_gain",
                   "rtp_port_base" },
    "streaming": { "enabled", "rtmp_server", "main_key_tpl", "aux_key_tpl", "push_aux" },
    "outgoing":  { "enabled", "dial_number", "mcu_host",
                   "reconnect", "reconnect_delay_s", "max_reconnects" },
    "tcp":       { "bind_addr", "port" },
    "auto_send_video": true,
    "log_dir":   "/var/log/recorder",
    "log_level": "info"
  }
}
```

字段含义详见 `src/config/AppConfig.h`。

### 2.5 set 支持的 key

dot-notation，**仅改进程内存（不写回 config.json）**：

| key | value 类型 | 立即生效？ |
|---|---|---|
| `gk.host` / `gk.port` / `gk.alias` / `gk.e164` / `gk.password` | string | ❌ 仅记录，提示 `"restart required to re-register"` |
| `outgoing.dial_number` / `outgoing.mcu_host` | string | ✅ 下次 dial 起 |
| `outgoing.enabled` / `outgoing.reconnect` | `"true"`/`"false"`/`"1"`/`"0"` | ✅ |
| `log_level` | string (`"debug"`/`"info"`/...) | ✅ 立即 re-init logger |
| `recorder.output_dir` | string | ✅ 立即 mkdir + 下次录像起 |
| `auto_send_video` | `"true"`/`"false"`/`"1"`/`"0"` | ✅ 下次通话起 |

其他 key → `"unsupported key: <key>"`。要持久化必须通过 web `/config/save`（写 config.json）然后 `reload` 或 restart。

### 2.6 调用方矩阵

| 调用方 | 用到的 cmd | 频率 |
|---|---|---|
| recorder-web `/api/status` | `status` | dashboard 轮询每 2s |
| recorder-web `/api/levels` | `audio_levels` | VU 表 ~10Hz |
| recorder-web `/api/control/<cmd>` | **白名单**：`start_main_video` / `stop_main_video` / `start_presentation` / `stop_presentation` | 用户点按钮 |
| recorder-web `/config` 页 | `config` (读) | 加载页面时（写配置是直接改 config.json 文件，不走 9001） |
| recorder-asr-bridge | `status`（只看 `meeting_id`） | **10s cache** — 避免阻塞 asyncio |
| ctrl_query.py CLI | 全部 | 运维诊断 |

### 2.7 客户端示例

#### Python

```python
import socket, json

def call(cmd_dict, host="127.0.0.1", port=9001, timeout=5):
    s = socket.socket(); s.settimeout(timeout)
    s.connect((host, port))
    s.sendall((json.dumps(cmd_dict) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(8192)
        if not chunk: break
        buf += chunk
    s.close()
    return json.loads(buf.decode())

print(call({"cmd": "status"}))
print(call({"cmd": "version"}))
print(call({"cmd": "dial", "number": "<mcu-number>"}))
print(call({"cmd": "set", "key": "log_level", "value": "debug"}))
print(call({"cmd": "faststart_one",
            "path": "/opt/recorder/recordings/20260526_820715/main_01.mp4"}))
```

#### Shell (netcat)

```bash
echo '{"cmd":"status"}'                                | nc -q 1 127.0.0.1 9001
echo '{"cmd":"version"}'                               | nc -q 1 127.0.0.1 9001
echo '{"cmd":"recordings"}'                            | nc -q 1 127.0.0.1 9001
echo '{"cmd":"dial","number":"<mcu-number>"}'          | nc -q 1 127.0.0.1 9001
echo '{"cmd":"clear_call"}'                            | nc -q 1 127.0.0.1 9001
echo '{"cmd":"set","key":"log_level","value":"debug"}' | nc -q 1 127.0.0.1 9001
```

或直接：`python3 /opt/recorder/recorder-core/scripts/ctrl_query.py status`

## 3. ASR Bridge → sherpa-onnx WebSocket（:6006）

bridge (`recorder-asr-bridge.py`) 作为 WebSocket client 连本机 sherpa server。**仅本机连接，无鉴权**。

### sherpa-onnx 协议（client → server）

```
Per audio chunk:
  4 bytes  int32 LE  num_samples
  N×4 bytes float32 LE samples (16kHz mono PCM)

End of stream:
  text message "Done"
```

bridge 每 100ms 推一帧（1600 samples）。

### sherpa-onnx 响应（server → client）

文本 JSON，每个 chunk 都返回一次：

```json
{
  "text": "对我做了介绍啊那么我想说的是呢",
  "tokens": ["对", "我", "做", "了", ...],
  "timestamps": [0.32, 0.64, ...],
  "ys_probs": [-0.27, -0.15, ...],
  "lm_probs": [],
  "context_scores": [],
  "segment": 0,
  "is_final": false,
  "is_eof": false
}
```

- `text=""` + `is_final=false`：keep-alive（无语音活动），bridge 忽略
- `text!=""` + `is_final=false`：partial（说话中）
- `is_final=true`：VAD endpoint 触发，segment 结束

bridge 处理逻辑：
- 任意非空 text → 写 `/opt/recorder/run/transcript.jsonl`
- final → 异步 fork `sherpa-onnx-offline-punctuation` 加标点，写第二条 `punct=true` 记录
- 每条都带 `meeting_id`（10s cache）+ 同时写 per-meeting `/opt/recorder/recordings/<m>/transcript.jsonl`

### Bridge heartbeat 输出（journalctl）

```
heartbeat: audio_pushed=120s msgs=370 finals=8 idle=0s
```

- `audio_pushed`：累计推给 sherpa 的音频秒数
- `msgs`：累计收到的 ws message 数
- `finals`：累计 final 数
- `idle`：距上次收到 ws msg 的秒数（超过 60s → watchdog 强制重连）

## 4. systemd path-unit 触发文件接口

`recorder-restart.path` 监听 `/opt/recorder/run/restart-recorder.flag` 的 close-after-write 事件，触发 `recorder-restart.service` 跑 `systemctl restart recorder-core.service`。

### 任何拥有 ftadmin 写入权限的进程都能触发：

```bash
echo "$(date)" > /opt/recorder/run/restart-recorder.flag
```

或在 Python 里：

```python
from pathlib import Path
Path("/opt/recorder/run/restart-recorder.flag").write_text(str(time.time()))
```

Flask web 的 POST `/config/restart` 内部就是这一行。

## 5. SRS API（仅 127.0.0.1:1985）

SRS 5.x 自带，无鉴权，**禁止对外**。运维诊断用：

| Method | Path | 说明 |
|---|---|---|
| GET | `/api/v1/summaries` | 系统概览 |
| GET | `/api/v1/streams/` | 所有流（含 publisher 状态、frames、kbps）|
| GET | `/api/v1/clients/` | 所有连接（拉流客户端） |
| GET | `/api/v1/vhosts/` | vhost 列表 |

例：

```bash
curl -s http://127.0.0.1:1985/api/v1/streams/ | python3 -m json.tool
```

## 6. 离线 ASR rescue 命令行接口

`asr_offline.py` 是独立 CLI（不在 systemd 里），用于补救字幕：

```bash
/opt/recorder/asr/venv/bin/python /opt/recorder/asr/bridge/asr_offline.py \
    <meeting_id> [--mp4 main_NN.mp4] [--overwrite | --append]
```

| 参数 | 说明 |
|---|---|
| `<meeting_id>` | 会议目录名，如 `20260520_<dial-number>` |
| `--mp4 <file>` | 处理指定 mp4（默认 main_01.mp4） |
| `--overwrite` | 覆盖现有 `transcript.jsonl` |
| `--append` | 追加（多段 mp4 串联用） |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `ASR_WS_URL` | `ws://127.0.0.1:6006` | sherpa server |
| `ASR_OFFLINE_THROTTLE` | `0.01` | chunk 间隔（防压垮 sherpa） |
| `ASR_OFFLINE_NO_PUNCT` | `0` | 设 `1` 跳过 punct |
| `RECORDINGS_DIR` | `/opt/recorder/recordings` | 录像根 |

多段会议典型用法：

```bash
SCR=/opt/recorder/asr/bridge/asr_offline.py
PY=/opt/recorder/asr/venv/bin/python
MID=20260520_<dial-number>

$PY $SCR $MID --mp4 main_01.mp4 --overwrite
for n in 02 03 04 05 06 07 08 09; do
  $PY $SCR $MID --mp4 main_${n}.mp4 --append
done
```

总时间 ≈ 会议音频时长 × RTF (0.21) + 9 × punct 时间。3 小时会议约 35-40 分钟。

## 7. LLM Provider 接口约定

`web/app.py` 的 `_llm_chat_complete()` 调 OpenAI 兼容的 `POST <base_url>/v1/chat/completions`：

请求体：

```json
{
  "model": "deepseek-chat",
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user",   "content": "..."}
  ],
  "stream": false,
  "response_format": {"type": "json_object"}   // 仅 refine 用
}
```

期望响应：

```json
{
  "choices": [
    {"message": {"content": "<reply>"}, ...}
  ],
  ...
}
```

兼容的 provider：
- **DeepSeek**：`https://api.deepseek.com`，model `deepseek-chat` / `deepseek-reasoner`
- 通义千问：`https://dashscope.aliyuncs.com/compatible-mode`，model `qwen-plus` / `qwen-max`
- 任何 OpenAI 兼容（自部署 vLLM / sglang / Ollama 也可）

切换 provider 只需改 `/opt/recorder/config/config.json` 的 `llm.{base_url, api_key, model}` 三个字段。
