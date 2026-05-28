# 服务清单与接口规划

> 按服务（进程）整理的端口、暴露接口、依赖接口、数据出口。
> 协议字节级细节见 [api.md](api.md)；架构图与设计动机见 [architecture.md](architecture.md)。

## 0. 整体规划

系统由 **4 个核心 systemd 服务** + **SRS 流媒体服务** + **外部依赖（GK / MCU / LLM）** 组成。所有进程跑在一台 Linux 主机（多机 HA 时每台都跑一整套）。

```
┌─────────┐  H.323 1720/RTP   ┌──────────────┐   RTMP 1935   ┌──────────┐
│ MCU/GK  │ ◄────────────────►│ recorder-core│ ─────────────►│   SRS    │
└─────────┘                   │  (C++)       │               │ 1935/8080│
                              │ TCP 9001 ◄─┐ │               │ HTTP 1985│
                              └──────┬─────┼─┘               └────┬─────┘
                                     │     │                      │
                              writes │     │ TCP 9001       RTMP  │ HLS
                                     ▼     │ (status)       拉流   │ 拉流
                    /opt/recorder/recordings/<m>/                  │
                    /opt/recorder/run/transcript.jsonl ◄──┐        │
                                                          │        │
                ┌─────────────────────┐  WS :6006   ┌─────┴────────┴───────┐
                │ recorder-asr        │ ◄──────────►│ recorder-asr-bridge  │
                │ (sherpa-onnx WS)    │             │ (Python asyncio)     │
                └─────────────────────┘             └──────────────────────┘
                                                          ▲
                       HTTP :8088                         │ TCP 9001
                ┌─────────────────────┐  TCP 9001         │ + writes
                │ recorder-web        │ ─────────────────►│
                │ (Flask + gunicorn)  │                   │
                │ ◄── 浏览器 (HTTPS反代) │                  ▼
                │ ──► LLM (HTTPS)      │            transcript.refined.jsonl
                └─────────────────────┘             summary.md
```

### 调用关系矩阵

行=调用方，列=被调方；交叉格写**端口/协议**。

| 调用方 ↓ \ 被调方 → | recorder-core | recorder-web | recorder-asr | recorder-asr-bridge | SRS | 外部 |
|---|---|---|---|---|---|---|
| **recorder-core**       | —              | —            | —            | —                   | RTMP 1935 push | GK 1719/udp、MCU 1720/tcp + RTP |
| **recorder-web**        | TCP 9001 JSON | —            | —            | —                   | —              | LLM HTTPS (DeepSeek/通义/...) |
| **recorder-asr**        | —              | —            | —            | —                   | —              | （仅读模型文件） |
| **recorder-asr-bridge** | TCP 9001 JSON | —            | WS 6006      | —                   | RTMP 1935 拉流 | — |
| **浏览器**              | —              | HTTP 8088 (含 SSE) | — | — | HLS 8080 / FLV 8080 | — |
| **运维 / ctrl_query.py** | TCP 9001 JSON | HTTP 8088    | —            | —                   | HTTP 1985 (诊断) | — |

### 端口总览（按监听方）

| 端口 | 协议 | 绑定 | 服务 | 用途 | 是否对外 |
|---|---|---|---|---|---|
| 1720 | TCP | 0.0.0.0 | recorder-core | H.225 call signaling | ✅ 公开 |
| 20000-21000 | UDP | 0.0.0.0 | recorder-core | H.245 RTP/RTCP（动态分配） | ✅ 公开 |
| 9001 | TCP | **127.0.0.1** | recorder-core | ControlServer JSON | ❌ 仅本机 |
| 8088 | TCP | 0.0.0.0 | recorder-web | Flask HTTP | ⚠️ 应反代 HTTPS |
| 6006 | TCP | **127.0.0.1** | recorder-asr | sherpa-onnx WebSocket | ❌ 仅本机 |
| 1935 | TCP | 0.0.0.0 | SRS | RTMP publish / play | ⚠️ 内网，对外可关 |
| 8080 | TCP | 0.0.0.0 | SRS | HLS / HTTP-FLV | ⚠️ 浏览器拉流用，可走反代 |
| 1985 | TCP | **127.0.0.1** | SRS | HTTP API（运维） | ❌ 仅本机 |
| —    | —   | — | recorder-asr-bridge | 无监听（纯客户端进程） | — |

> 防火墙建议详见 [deployment.md](deployment.md)。最小化对外只需 1720/tcp + 20000-21000/udp + 443/tcp（反代后的 web）。

---

## 1. recorder-core

H.323 录播主进程。是整个系统**唯一**直接跟 MCU/GK 通信的进程。

### 身份

| 项 | 值 |
|---|---|
| systemd unit | `recorder-core.service` |
| binary | `/opt/recorder/bin/recorder-core` |
| 启动命令 | `start-foreground.sh`（设 LD_LIBRARY_PATH 后 exec） |
| 运行用户 | `ftadmin`（生产建议改 `recorder`） |
| 配置文件 | `/opt/recorder/config/config.json` |
| 工作目录 | `/opt/recorder` |
| 录像输出 | `/opt/recorder/recordings/<meeting_id>/` |

### 监听端口

| 端口 | 协议 | 绑定 | 用途 |
|---|---|---|---|
| 1720 | TCP | 0.0.0.0 | H.225 Q.931 信令（MCU/GK 主呼） |
| 20000-21000 | UDP | 0.0.0.0 | RTP/RTCP（H.245 OLC 协商时动态分配） |
| 9001 | TCP | 127.0.0.1 | ControlServer — 单行 JSON 命令 |

### 暴露的接口

#### A. ControlServer @ TCP 127.0.0.1:9001

**协议**：单行 JSON 命令 + 单行 JSON 响应，`\n` 终止。无鉴权（127.0.0.1 only）。

完整列表（**15 个 cmd**）：

| cmd | 调用方 | 用途 |
|---|---|---|
| `status` | recorder-web `/api/status`（轮询 2s）<br>recorder-asr-bridge（查 `meeting_id`，10s cache）<br>ctrl_query.py | 完整运行状态（18 字段，详 api.md §2.3） |
| `audio_levels` | recorder-web `/api/levels`（~10Hz）| 实时 RMS/Peak dBFS + age_ms |
| `config` | recorder-web `/config` 页 / 运维 | 返回完整运行配置（password 字段 `"***"`） |
| `recordings` | 运维 | 列 output_dir 顶层 .mp4（不递归子目录） |
| `version` | 运维 | binary 版本 + 配置路径 |
| `dial` | recorder-web（管理员主动呼出） | 参数 `number`(必), `host`(可选 = 绕 GK 直连 MCU) |
| `clear_call` | recorder-web | **挂断**（没有 `hangup`！）；参数 `token`(可选) — 缺省挂断全部 |
| `start_main_video` / `stop_main_video` | recorder-web `/api/control/start_main_video` 等 | 控制本端主流屏保发送 |
| `start_presentation` / `stop_presentation` | recorder-web `/api/control/start_presentation` 等 | H.239 主动演示 |
| `reload` | 运维 | 重读 config.json；GK 字段变了返回 warning 提示需 restart |
| `set` | 运维 | dot-notation 改运行时配置（**不写回文件**）；详 api.md §2.5 |
| `faststart_one` | 运维 / web | 参数 `path`(必) — 路径必须在 output_dir 内 |
| `faststart_all` | 运维 | 后台扫整库 |

**完整字段、错误码、调用方矩阵、Python/netcat 示例** → [api.md §2](api.md#2-recorder-core-controlserver-tcp-9001)

> ⚠️ web `/api/control/<cmd>` 实际白名单只有 4 条：`start_main_video` / `stop_main_video` / `start_presentation` / `stop_presentation`。其他 cmd 不通过 web 暴露给浏览器。

#### B. H.323/H.225 服务 @ TCP 1720（外部）

- 接受 MCU / GK / 任意 H.323 终端的呼入
- 协商 H.245 → 打开 H.264/AAC RTP 流 → 录制 mp4
- H.239 辅流（演示）通过 extended-video session=10 协商
- 详见 [late-join-h239-fix.md](late-join-h239-fix.md)

### 依赖的接口（出站）

| 目标 | 端口/协议 | 用途 | 必需？ |
|---|---|---|---|
| **GK** | UDP `:1719`（RAS） | alias 注册 / 主呼路由（gatekeeper discovery → RRQ） | 配 GK 时必需 |
| **MCU** | TCP `:1720` + RTP（动态） | H.225/H.245 信令 + 媒体 | 主呼时必需 |
| **SRS** | RTMP `rtmp://127.0.0.1:1935/live/recorder-main` | 推主流（直播 + 字幕拉流源） | ✅ |
| **SRS** | RTMP `rtmp://127.0.0.1:1935/live/recorder-aux` | 推 H.239 辅流 | H.239 会议时 |
| 外部 LLM | — | 不调用（LLM 调用都在 web 进程） | — |

### 数据出口（文件系统）

| 路径 | 写入时机 | 内容 |
|---|---|---|
| `/opt/recorder/recordings/<meeting_id>/main_NN.mp4` | 通话期间持续写，每段一个文件 | H.264 + AAC fragmented mp4 |
| `/opt/recorder/recordings/<meeting_id>/aux_NN.mp4` | H.239 演示期间 | 同上，辅流 |
| `/opt/recorder/recordings/<meeting_id>/meeting.json` | 通话开始 + 段切换时更新 | `caller_id`/`start_wall_ms`/`segments[]` |
| stdout（systemd journal） | 全程 | spdlog 日志，`journalctl -u recorder-core` |

### 入站触发（其他进程影响 recorder-core 的方式）

| 方式 | 触发源 | 效果 |
|---|---|---|
| 写 `/opt/recorder/run/restart-recorder.flag` | recorder-web `/config/restart` 或运维 `echo $(date) > ...` | systemd path-unit 检测到 close-after-write → `systemctl restart recorder-core` |
| `systemctl restart recorder-core` | 直接运维 | — |

> path-unit 触发机制是 sudoless 设计的关键，见 [architecture.md §1](architecture.md#1-为什么-sudoless-restart-triggerpath-unit--flag-file)。

---

## 2. recorder-web

Flask 管理页 + 回放页 + LLM 调用代理。是浏览器和 recorder-core 之间的桥梁。

### 身份

| 项 | 值 |
|---|---|
| systemd unit | `recorder-web.service` |
| 入口 | `gunicorn -w 2 -k gthread --threads 4 --timeout 300 web.app:app` |
| 源码 | `/opt/recorder/recorder-core/web/app.py` |
| 运行用户 | `ftadmin` |
| 用户哈希文件 | `/opt/recorder/web/auth.json`（bcrypt） |

### 监听端口

| 端口 | 协议 | 绑定 | 用途 |
|---|---|---|---|
| 8088 | TCP | 0.0.0.0 | HTTP/HTML/JSON/SSE |

> 生产**应**前置 nginx/caddy 反代提供 HTTPS。直接对外裸 HTTP 会暴露登录 session cookie。

### 暴露的接口（HTTP @ :8088）

按功能分组。所有 `/api/*` `/recordings/*` `/config/*` 需 session cookie 登录。

#### 鉴权 / 健康
| Method | Path | 调用方 | 用途 |
|---|---|---|---|
| GET/POST | `/login` `/logout` | 浏览器 | 表单登录 |
| GET | `/health` | LB / 运维 | 无鉴权，`{"ok":true}` |

#### 仪表盘 / 直播
| Method | Path | 内部走向 | 说明 |
|---|---|---|---|
| GET | `/` | → recorder-core status / audio_levels | 仪表盘 |
| GET | `/api/status` | → recorder-core `{"cmd":"status"}` | JSON 通话状态 |
| GET | `/api/levels` | → recorder-core `{"cmd":"audio_levels"}` | VU 表（每秒轮询） |
| GET | `/live` | — | 直播页（hls.js + SSE 字幕） |
| GET | `/api/control/<cmd>` | → recorder-core（**严格白名单**） | `start_video` / `stop_video` / `start_presentation` / `stop_presentation` |
| GET | `/api/transcript/stream` | tail `/opt/recorder/run/transcript.jsonl` | **SSE 流**，每行一个 `data:` event |

#### 配置
| Method | Path | 内部走向 | 说明 |
|---|---|---|---|
| GET | `/config` | 读 config.json | 字段化编辑页 |
| POST | `/config/save` | 写 config.json | 字段化保存（自动 `gk.e164 = gk.alias`） |
| POST | `/config/save_advanced` | 写 config.json | 高级模式整段 JSON 替换 |
| POST | `/config/restart` | 写 `/opt/recorder/run/restart-recorder.flag` | 触发 recorder-core 重启 |

#### 录像 / 回放
| Method | Path | 走向 | 说明 |
|---|---|---|---|
| GET | `/recordings` | 扫文件系统 | 列表 |
| GET | `/recordings/<m>` | — | 回放页 |
| GET | `/play/<m>/<f>` | send_file（**Range 支持**） | 浏览器 video tag 拉 mp4 |
| GET | `/recordings/<m>/transcript.json` | 读 transcript.jsonl(+.refined) | 字幕 + meeting-timeline 对齐 |
| **POST** | `/recordings/<m>/transcript/refine` | → 外部 LLM HTTPS（5 批×50 句） | 写 `transcript.refined.jsonl` |
| GET/POST | `/recordings/<m>/summary` | → 外部 LLM | 读/写 `summary.md` |

完整字段、Range 行为、SSE 心跳等 → [api.md §1](api.md#1-web-http-api-recorder-web-:8088)。

### 依赖的接口（出站）

| 目标 | 协议/端口 | 用途 |
|---|---|---|
| recorder-core | TCP `127.0.0.1:9001` | 所有 `/api/status` `/api/levels` `/api/control/*` 都走这条 |
| LLM provider | HTTPS（DeepSeek `api.deepseek.com` / 通义 `dashscope.aliyuncs.com` / 自部署 vLLM） | refine + summary |
| 文件系统 | — | 读/写录像 + 字幕 + 纪要 + 配置 + 用户哈希 |

### 数据出口

| 路径 | 时机 | 内容 |
|---|---|---|
| `/opt/recorder/recordings/<m>/transcript.refined.jsonl` | 用户点"字幕优化" | LLM 纠错后的字幕 |
| `/opt/recorder/recordings/<m>/summary.md` | 用户点"生成纪要" | LLM 总结 |
| `/opt/recorder/run/restart-recorder.flag` | `/config/restart` | 触发文件 |
| `/opt/recorder/config/config.json` | `/config/save*` | 配置修改 |
| `/opt/recorder/web/auth.json` | （未实现 web 端，手动 hash 添加） | 用户哈希 |
| stdout | 全程 | `journalctl -u recorder-web` |

---

## 3. recorder-asr

sherpa-onnx 流式 ASR 服务。是个**通用 WebSocket 服务**，本项目只用它做实时识别。

### 身份

| 项 | 值 |
|---|---|
| systemd unit | `recorder-asr.service` |
| binary | `/opt/recorder/asr/sherpa-onnx/bin/sherpa-onnx-online-websocket-server` |
| ExecStart 关键参数 | `--port=6006`<br>`--encoder/--decoder/--joiner=zipformer-zh-int8/*.onnx`<br>`--tokens=tokens.txt`<br>`--hotwords-file=/opt/recorder/asr/bridge/hotwords_default.txt`<br>`--hotwords-score=2.0`<br>`--decoding-method=modified_beam_search` |
| 运行用户 | `ftadmin` |
| 模型 | `/opt/recorder/asr/models/streaming-zipformer-zh-int8-2025-06-30/` |
| 热词 | `/opt/recorder/asr/bridge/hotwords_default.txt`（902 行教育领域，已过滤词表外字符） |

### 监听端口

| 端口 | 协议 | 绑定 | 用途 |
|---|---|---|---|
| 6006 | TCP (WebSocket) | 127.0.0.1 | sherpa 流式协议 |

> 协议是 sherpa-onnx 上游开源约定（**非本项目自定义**），见 sherpa-onnx 文档 + [api.md §3](api.md#3-asr-bridge--sherpa-onnx-websocket-6006)。

### 暴露的接口（sherpa-onnx 流式协议）

**单连接 = 一段连续音频**，二进制帧 + 文本消息混合：

```
client → server:
   每 100ms 一帧：
     4B int32 LE: num_samples (固定 1600)
     1600 × 4B float32 LE: PCM 16kHz mono samples
   结束：text message "Done"

server → client:
   每个 chunk 一条文本 JSON：
     {"text":..., "tokens":[...], "timestamps":[...],
      "segment":N, "is_final":bool, "is_eof":bool}
```

- `text=""` + `is_final=false`：keep-alive（VAD silence）
- `text!=""` + `is_final=false`：partial
- `is_final=true`：VAD endpoint，segment N 结束 → 下一帧属 segment N+1

### 调用方

| 进程 | 说明 |
|---|---|
| **recorder-asr-bridge** | 长连接 + 实时推 | 主要用户 |
| **asr_offline.py**（CLI） | 每个 mp4 一次连接 + 推完整音频 + `"Done"` | rescue 用 |
| 任意 sherpa-onnx 兼容客户端 | — | 协议开放 |

### 依赖的接口（出站）

无网络出站。只读模型文件。

### 数据出口

| 路径 | 内容 |
|---|---|
| stdout | 启动日志、token 错误（"Cannot find ID for token X" 应该 0 条） |

---

## 4. recorder-asr-bridge

把 SRS 直播流转成 sherpa ASR + 写字幕的"中间人"进程。**纯客户端，不监听任何端口。**

### 身份

| 项 | 值 |
|---|---|
| systemd unit | `recorder-asr-bridge.service` |
| 入口 | `/opt/recorder/asr/venv/bin/python /opt/recorder/asr/bridge/recorder-asr-bridge.py` |
| 源码 | `/opt/recorder/asr/bridge/recorder-asr-bridge.py` |
| 运行用户 | `ftadmin` |
| 主要依赖 | Python 3.7 + `websockets==11.0.3` + 系统 ffmpeg 4.2.4 + sherpa-onnx-offline-punctuation binary |
| 标点模型 | `/opt/recorder/asr/models/ct-transformer-zh-en/`（被 fork 出去的 punct 子进程读） |

### 监听端口

无。

### 暴露的接口

**没有入站接口** — bridge 不接受任何外部调用。

唯一的"对外可见"是它**写入的文件**和**输出到 journal 的 heartbeat**：

```
heartbeat: audio_pushed=120s msgs=370 finals=8 idle=0s
```
- `audio_pushed`：累计推给 sherpa 的音频秒数
- `msgs` / `finals`：累计 ws message / final 数
- `idle`：距上次收到 ws msg 的秒数（>60s → watchdog 强制重连）

监控这条 → `journalctl -u recorder-asr-bridge -f --since '5 min ago' | grep heartbeat`。

### 依赖的接口（出站）

| 目标 | 协议/端口 | 用途 | 频率 |
|---|---|---|---|
| **SRS** | RTMP `rtmp://127.0.0.1:1935/live/recorder-main` | subprocess ffmpeg 拉流 → 解码 16kHz mono PCM | 持续 |
| **recorder-asr** (sherpa) | WS `ws://127.0.0.1:6006` | 推 PCM chunk，收字幕 JSON | 每 100ms 一帧 |
| **recorder-core** | TCP `127.0.0.1:9001` `{"cmd":"status"}` | 查当前 `meeting_id` 写 per-meeting jsonl | **10s cache**（避免阻塞 asyncio） |
| `sherpa-onnx-offline-punctuation` | fork subprocess | 给每条 final 加标点 | 每次 final |

> 关键陷阱：调 9001 时**必须**发 JSON `{"cmd":"status"}\n`，**不能**发 `b"status\n"`（旧 bug，commit `4576d4d`）。

### 数据出口（文件系统）

| 路径 | 写入时机 | 内容 |
|---|---|---|
| `/opt/recorder/run/transcript.jsonl` | 收到任意非空 text 时 | 实时字幕流（SSE tail 源） |
| `/opt/recorder/recordings/<meeting_id>/transcript.jsonl` | 同上，且 `meeting_id` 非空时 | 持久化字幕，回放页用 |
| stdout | 全程 | heartbeat + 错误，`journalctl -u recorder-asr-bridge` |

每条 jsonl 一行格式：

```json
{"t": 1779254714.196, "text": "...", "segment": 0, "is_final": true,
 "timestamps": [0.32, ...], "punct": false, "meeting_id": "20260520_xxx"}
```

详 [api.md §3](api.md#3-asr-bridge--sherpa-onnx-websocket-6006)。

---

## 5. SRS Server

流媒体中转（RTMP publish → HLS / HTTP-FLV play）。本项目不写源码，配置见 deployment 文档。

### 身份

| 项 | 值 |
|---|---|
| 进程 | `srs` (Simple Realtime Server 5.x) |
| 配置 | `/opt/recorder/srs/conf/srs.conf` |
| HLS 输出 | `/opt/recorder/srs/objs/nginx/html/live/*.m3u8 + *.ts` |

### 监听端口

| 端口 | 协议 | 绑定 | 用途 | 对外? |
|---|---|---|---|---|
| 1935 | TCP (RTMP) | 0.0.0.0 | publish + play | ⚠️ 内网（recorder-core push + bridge pull） |
| 8080 | TCP (HTTP) | 0.0.0.0 | HLS .m3u8/.ts、HTTP-FLV | ⚠️ 浏览器直接拉，建议反代 |
| 1985 | TCP (HTTP) | 127.0.0.1 | API（运维诊断） | ❌ 仅本机 |

### 暴露的接口

#### RTMP @ :1935
- Publish：`rtmp://<host>:1935/live/<stream>`
  - `recorder-main`（recorder-core 推主流）
  - `recorder-aux`（H.239 辅流）
- Play：同 URL 拉（bridge / 任意客户端）

#### HLS / HTTP-FLV @ :8080
- HLS：`http://<host>:8080/live/<stream>.m3u8` + `.ts`
- HTTP-FLV：`http://<host>:8080/live/<stream>.flv`
- 浏览器 hls.js 走 HLS

#### Admin API @ 127.0.0.1:1985
仅运维诊断（无鉴权）：

| Path | 用途 |
|---|---|
| `/api/v1/summaries` | 系统总览 |
| `/api/v1/streams/` | 所有流（含 publisher 状态、frames、kbps、video.codec/profile/width/height） |
| `/api/v1/clients/` | 所有连接（拉流客户端） |

完整字段 → [api.md §5](api.md#5-srs-api-仅-1271001985)。

### 依赖

只读本机文件系统 + 监听端口。无出站调用。

---

## 6. 外部依赖

### 6.1 H.323 Gatekeeper

| 项 | 值 |
|---|---|
| 协议 | H.225 RAS / UDP 1719 |
| 配置位置 | `config.json` 的 `gk` 节（`host`/`port`/`alias`/`e164`） |
| 关系 | recorder-core 启动时发 GRQ→RRQ 注册；MCU 用 alias 寻路到 recorder |

### 6.2 MCU（媒体网控器）

| 项 | 值 |
|---|---|
| 协议 | H.323 1720/tcp + 动态 RTP UDP |
| 关系 | recorder-core 既可主呼 MCU（dial），也可被叫 |
| 本项目兼容 | Polycom RMX, VP9660；vendor 伪装为华为 TE52 让 SMC 070E 兼容（见 [late-join-h239-fix.md](late-join-h239-fix.md)） |

### 6.3 LLM Provider

| 项 | 值 |
|---|---|
| 协议 | HTTPS POST `<base_url>/v1/chat/completions`（OpenAI 兼容） |
| 配置位置 | `config.json` 的 `llm` 节（`base_url`/`api_key`/`model`） |
| 调用方 | **仅** recorder-web（refine + summary） |
| 推荐 | DeepSeek `deepseek-chat`；通义 `qwen-plus`；自部署 vLLM/Ollama 都行 |

完整请求体格式 → [api.md §7](api.md#7-llm-provider-接口约定)。

---

## 7. 常见运维场景对照

| 场景 | 操作的服务 | 端口/接口 |
|---|---|---|
| 看会议状态 | recorder-core | `echo '{"cmd":"status"}' \| nc 127.0.0.1 9001` |
| 重启 recorder-core | （path-unit 间接） | `echo $(date) > /opt/recorder/run/restart-recorder.flag` |
| 浏览器看直播 | recorder-web + SRS | `http://<host>:8088/live` |
| 改 GK 配置 | recorder-web | `http://<host>:8088/config` → 改 → 保存 + 触发重启 |
| 看实时字幕流（debug） | recorder-asr-bridge | `tail -f /opt/recorder/run/transcript.jsonl` |
| 离线回填字幕 | asr_offline.py + recorder-asr | `python asr_offline.py <meeting_id>` |
| 看 SRS 收到啥分辨率 | SRS | `curl 127.0.0.1:1985/api/v1/streams/` |
| 改 LLM provider | recorder-web | 改 `config.json` → 不需重启 recorder-core（web 每次请求都重读） |

---

## 8. 安全检查清单（基于接口暴露面）

| 风险 | 当前防护 | 状态 |
|---|---|---|
| 9001 ControlServer 无鉴权 | 仅 127.0.0.1 监听 | ✅ 默认 |
| 6006 sherpa 无鉴权 | 仅 127.0.0.1 监听 | ✅ 默认 |
| 1985 SRS API 无鉴权 | 仅 127.0.0.1 监听 | ✅ 默认 |
| 8088 Web HTTP 明文 | `recorder-web.service` 默认绑 127.0.0.1 + `SESSION_COOKIE_SECURE=1` + 提供 [nginx 反代模板](../scripts/nginx-recorder.conf.example) + ProxyFix middleware | ✅ v3.2 |
| CSRF（跨站请求伪造） | Flask-WTF `CSRFProtect`：所有 POST 强制带 `csrf_token` / `X-CSRFToken` header | ✅ v3.2 |
| 登录暴力破解 | Flask-Limiter：`/login` POST 5/min/IP，超限 429 | ✅ v3.2 |
| 操作可追溯 | 审计日志 `AUDIT user=... ip=... action=...`：login_success/fail、logout、config_save、recorder_restart、control、transcript_refine、summary_generate；`journalctl -u recorder-web \| grep AUDIT` 查 | ✅ v3.2 |
| 1720 H.323 公开 | 协议本身（H.225 RAS / Q.931 没鉴权设计） | 防火墙限制源 IP 为 GK + MCU 网段（详 deployment.md） |
| 1935 RTMP 公开 | — | 建议防火墙只允许本机 + 内网 |
| LLM API Key 泄漏 | `config.json` chmod 0600 | 单独的 secrets 管理（如环境变量、Vault） |
| 角色区分（admin/operator/viewer） | 未实现 — 所有登录用户权限相同 | TODO（参考 deployment.md §安全） |

完整防火墙模板 + HTTPS 反代步骤 → [deployment.md §安全](deployment.md)。
