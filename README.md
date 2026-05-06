# MeetingRecord — H.323 录播服务器

基于 H323Plus 的 H.323 视频会议录播服务器，支持自动录制主视频和 H.239 辅流（双流）。

## 功能

- **H.323 入会**：通过 GK 注册，MCU 呼叫入会或主动外呼
- **主视频录制**：H.264 视频 + AAC/G.722/G.711 音频 → MP4
- **辅流录制**：H.239 扩展视频（演示/屏幕共享）→ 独立 aux MP4 文件
- **晚入会辅流接收**：会议中已有演示时后入会，自动接收辅流（v2.0+）
- **RTMP 推流**：主视频和辅流分别推送到 SRS/nginx-rtmp
- **TCP 控制接口**：JSON 命令控制（拨号/挂断/开始演示/停止演示）
- **Web 管理界面**：会议录制状态查看、文件下载

## 快速开始

### 编译

```bash
mkdir build && cd build
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/opt/recorder \
  -DCMAKE_PREFIX_PATH=/usr/local
make -j recorder-core
```

### 配置

编辑 `config/config.json`：

```json
{
  "gk": {
    "host": "<gk_host>",
    "port": 1719,
    "alias": "<alias-1>"
  },
  "auto_send_video": true
}
```

关键配置项：

| 配置 | 说明 |
|------|------|
| `gk.alias` | GK 注册别名（E.164 号码） |
| `auto_send_video` | **晚入会辅流必需**，入会后自动发送主视频 |
| `outgoing.enabled` | 启动时自动外呼 |
| `streaming.enabled` | 是否启用 RTMP 推流 |

### 运行

```bash
/opt/recorder/bin/recorder-core -c /opt/recorder/config/config.json
```

### TCP 控制

```bash
# 拨号
python3 scripts/ctrl_query.py dial number=<dial-number> host=<mcu_host>

# 查看状态
python3 scripts/ctrl_query.py status

# 开始/停止演示
python3 scripts/ctrl_query.py start_presentation
python3 scripts/ctrl_query.py stop_presentation
```

## 晚入会辅流修复

MCU（华为 VP9660）在晚入会场景下不会向纯接收端推送已有辅流。通过以下修改解决：

1. **TCS 能力声明**：audio → receiveAndTransmit、注入 T.120 数据能力
2. **华为设备伪装**：nonStandard TCS 标识（h221 28/21/555 + 38/0/8209）+ H.225 vendor
3. **ECEC 回显**：回应华为私有 ECEC 探测，而非静默吸收
4. **主动查询**：发送 conferenceRequests + TE 风格 nonStandard 查询
5. **主视频发送**：`auto_send_video=true`，入会后立即打开返回视频通道

详见 `docs/late-join-h239-fix.md`。

## 目录结构

```
├── config/             # 配置文件
├── docs/               # 文档
│   └── late-join-h239-fix.md
├── scripts/            # 部署和测试脚本
├── src/
│   ├── h323/           # H.323 信令处理
│   │   ├── RecorderConnection.cpp/h   # 连接管理、TCS/OLC 处理
│   │   └── RecorderEndpoint.cpp/h     # 端点、能力注册
│   ├── media/          # 媒体处理（录制/推流/视频发送）
│   ├── meeting/        # 会议目录和分段管理
│   └── tcp/            # TCP JSON 控制服务器
├── third_party/        # 第三方库
└── web/                # Web 管理界面
```

## 依赖

- H323Plus 1.27.2+
- PTLib 2.18.8+
- FFmpeg 6.1+ (libavcodec/libavformat)
- x264
- spdlog
- nlohmann/json

## 部署

| 服务器 | IP | alias | 说明 |
|--------|-----|-------|------|
| 102 | <recorder_host> | <alias-1> | 录播设备 |
| 104 | <recorder_host_secondary> | <alias-2> | 录播设备（模拟器） |
