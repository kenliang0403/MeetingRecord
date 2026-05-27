# MeetingRecord — H.323 会议录播 + 实时字幕 + LLM 纪要

基于 H.323Plus 的会议录播服务器，集成 H.323/H.239 录制、SRS 直播、sherpa-onnx 实时字幕和 LLM 字幕纠错/纪要生成。

## 核心能力

| 模块 | 说明 |
|---|---|
| **H.323 录播** | GK 注册 + 主动/被叫入会，主视频 (H.264) + 辅流 (H.239) 双路录 MP4，自动 faststart |
| **实时直播** | SRS RTMP/HLS 推流，浏览器 hls.js 播放 |
| **实时字幕** | sherpa-onnx zipformer-zh + 902 行热词 + 标点模型，SSE 推浏览器 |
| **会议纪要** | DeepSeek/兼容 LLM 生成结构化 Markdown 纪要 |
| **字幕优化** | 保守 LLM 后处理：同音字纠错 + 口头禅清理 + 标点补全 |
| **回放页** | 主辅流时间同步播放 + 字幕同步显示 + 字幕优化 toggle |
| **离线 rescue** | 任意会议 mp4 → 全量字幕回填脚本 (`asr_offline.py`) |

## 架构 60 秒概览

```
┌─────────────────┐  H.323/H.239  ┌─────────────────────────────────┐
│  MCU / GK / TE  │ ◄────────────►│      recorder-core (C++)        │
└─────────────────┘   RTP media   │  • H.323Plus 1.27.2 + PTLib     │
                                  │  • H.264/AAC encode → fmp4      │
                                  │  • H.239 aux stream             │
                                  │  • SrsStreamer (RTMP push)      │
                                  │  • TCP ControlServer :9001      │
                                  └────────┬────────────────────────┘
                                           │ RTMP push
                                           ▼
                                  ┌─────────────────┐
                                  │   SRS Server    │
                                  │   :1935/:8080   │  HLS / FLV
                                  └────┬───────┬────┘
                                       │       │
                              FLV pull │       │ HLS
                                       ▼       ▼
              ┌────────────────────────────┐  ┌──────────────────────┐
              │  recorder-asr-bridge (Py)  │  │ Browser (hls.js)     │
              │  ffmpeg → 16k PCM →        │  │ /live page           │
              │  sherpa websocket :6006    │  │ ▲                    │
              │  ↓                         │  │ │ SSE (字幕)         │
              │  /opt/recorder/run/        │  │ │                    │
              │   transcript.jsonl  ─────  │ ─┘                      │
              │  /opt/recorder/recordings/ │                         │
              │   <m>/transcript.jsonl     │                         │
              └────────────────────────────┘                         │
                                                                     │
              ┌────────────────────────────┐                         │
              │  recorder-web (Flask+gunic)│ ◄───────────────────────┘
              │  :8088                     │
              │  /login /config /live      │
              │  /recordings/<m>           │  ← 回放 + 字幕优化 + 纪要
              │  /api/transcript/stream    │  ← SSE
              │  /recordings/<m>/transcript/refine  ← LLM
              │  /recordings/<m>/summary            ← LLM
              └────────────────────────────┘
```

## 部署的 4 个 systemd 服务

```
recorder-core.service           # H.323 录播主进程
recorder-web.service            # Flask 管理页 / 回放页
recorder-asr.service            # sherpa-onnx websocket server
recorder-asr-bridge.service     # SRS audio → sherpa → transcript.jsonl
+ recorder-restart.path + .service  # sudoless restart trigger (web 用)
```

## 文档索引

| 文档 | 用途 |
|---|---|
| [docs/deployment.md](docs/deployment.md) | **部署指南 + 端口 + 防火墙 + 安全** ⭐ |
| [docs/services.md](docs/services.md) | **服务清单 / 端口 / 接口规划（按 4 个服务整理）** ⭐ |
| [docs/architecture.md](docs/architecture.md) | 详细架构 / 组件 / 数据流 / 关键设计决策 |
| [docs/api.md](docs/api.md) | HTTP / TCP / WebSocket 协议字节级文档 |
| [docs/development.md](docs/development.md) | 本地开发环境 / 编译 / 测试 |
| [docs/operations.md](docs/operations.md) | 日志 / 备份 / 故障排查 / 升级流程 |
| [docs/late-join-h239-fix.md](docs/late-join-h239-fix.md) | 历史：H.239 晚入会修复（v2.0） |

## 快速开始（已部署环境的运维视角）

```bash
# 检查 4 个服务
for s in recorder-core recorder-web recorder-asr recorder-asr-bridge; do
  echo "$s = $(systemctl is-active $s)"
done

# 浏览器打开 web 管理页（首次需登录）
# http://<server>:8088/

# 当前通话状态（直接调本机 TCP 9001）
echo '{"cmd":"status"}' | nc -q 1 127.0.0.1 9001

# 重启 recorder-core（path-unit 触发，无需 sudo）
echo "$(date)" > /opt/recorder/run/restart-recorder.flag
```

首次部署看 [docs/deployment.md](docs/deployment.md)。代码改动开发流程看 [docs/development.md](docs/development.md)。

## 生产部署位置（示例）

按业务需要部署 1 台或 2 台（HA / 备录）。每台在 GK 上用不同 alias 注册。

| 主机 | 角色 | alias |
|---|---|---|
| `<recorder_host>` | 主录播 | `<alias-1>` |
| `<recorder_host_secondary>` | 备录播 / 模拟器 | `<alias-2>` |

主机地址在 `.env`（见 `.env.example`）和 `/opt/recorder/config/config.json` 里填。

## 依赖

| 组件 | 版本 | 安装方式 |
|---|---|---|
| H.323Plus | 1.27.2 | 源码编译 (third_party/) |
| PTLib | 2.18.8 | 源码编译 |
| FFmpeg | 6.1 (libavformat.so.60) | 自编译到 `/usr/local/lib` |
| FFmpeg CLI | 4.2.4 | 系统 dnf install ffmpeg |
| sherpa-onnx | v1.13.0 | 预编译 release tarball |
| zipformer-zh-int8 | 2025-06-30 | release 模型 161MB |
| ct-transformer 标点 | int8 2024-04-12 | release 模型 62MB |
| Python | 3.7.9 (系统) | UOS Server 20 自带 |
| websockets (pip) | 11.0.3 | `/opt/recorder/asr/venv` |
| Flask + gunicorn | 已装 | recorder-web 用 |

---

## License

See [LICENSE](LICENSE) (if present). Technical details under [docs/](docs/).
