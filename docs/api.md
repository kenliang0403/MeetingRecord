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

**仅 127.0.0.1 监听**，无鉴权。所有命令都是单行 JSON + `\n`。

### 协议

```
client → server:  {"cmd":"<command>","key":"value",...}\n
server → client:  {"ok":true,"data":{...}}\n     # 成功
                  {"ok":false,"error":"..."}\n   # 失败
```

### 命令列表

| cmd | 参数 | 说明 |
|---|---|---|
| `status` | (无) | 返回完整运行状态（in_call / main_file / meeting_id / gk_registered / video / audio / streaming / ...） |
| `dial` | `number`(必填), `host`(可选 = MCU 直连绕开 GK) | 主动呼出 |
| `hangup` | (无) | 挂断当前通话 |
| `clear_call` | (无) | 强制清除 call_token（hangup 前未释放时备用）|
| `start_video` | (无) | 开始向 MCU 发主视频（默认 ScreenSaver 测试图）|
| `stop_video` | (无) | 停止 |
| `start_presentation` | (无) | 触发 H.239 主动演示（presentationTokenRequest → grant → OLC）|
| `stop_presentation` | (无) | CLC session=10 + presentationTokenRelease |
| `audio_levels` | (无) | 实时 RMS + Peak dBFS（dashboard VU 用） |
| `config` | (无) | 返回当前运行配置 |
| `faststart_one` | `path` | 对某个 mp4 跑 faststart 重写（路径必须在 `output_dir` 内） |
| `faststart_all` | (无) | 后台扫整库做 faststart |

### Python 客户端示例

```python
# /opt/recorder/recorder-core/scripts/ctrl_query.py
import socket, json
def send(cmd_dict):
    s = socket.socket(); s.settimeout(5)
    s.connect(("127.0.0.1", 9001))
    s.sendall((json.dumps(cmd_dict) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(8192)
        if not chunk: break
        buf += chunk
    s.close()
    return json.loads(buf.decode())

print(send({"cmd": "status"}))
print(send({"cmd": "dial", "number": "<dial-number>"}))
```

CLI 包装：

```bash
python3 /opt/recorder/recorder-core/scripts/ctrl_query.py status
python3 /opt/recorder/recorder-core/scripts/ctrl_query.py dial number=<dial-number>
python3 /opt/recorder/recorder-core/scripts/ctrl_query.py start_presentation
```

### 重要：必须发 JSON，不要发纯文本

**踩坑历史**：bridge.py 早期用 `s.sendall(b"status\n")`，被 ControlServer 当成 invalid command 返回 `{"ok":false}`。务必发 JSON 格式。详见 commit `4576d4d`。

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
| `<meeting_id>` | 会议目录名，如 `20260520_820695` |
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
MID=20260520_820695

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
