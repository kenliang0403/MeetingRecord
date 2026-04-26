# recorder-core — Phase 1 使用说明与测试文档

## 概述

`recorder-core` 是一个 H.323 录制服务，运行在 UOS Server 20 (<recorder_host>) 上，安装于 `/opt/recorder`。

- 向 VP9660 MCU 注册为 H.323 别名 `<recorder-alias>`
- 被叫模式：MCU 主动拨入，recorder 自动接听
- 接收视频（YUV420P）和音频（PCM）并录制为 MP4 文件

---

## 目录结构

```
/opt/recorder/
├── bin/
│   └── recorder-core        # 可执行程序
├── config/
│   └── config.json          # 配置文件
├── recordings/              # MP4 录制输出目录
├── logs/                    # 日志目录
└── scripts/
    ├── start.sh             # 启动脚本
    └── stop.sh              # 停止脚本
```

---

## 配置文件

`/opt/recorder/config/config.json`

```json
{
  "gk": {
    "host":  "10.84.100.XXX",   // ← 替换为 VP9660 MCU 的 IP 地址
    "port":  1719,
    "alias": "<recorder-alias>",
    "e164":  "",
    "ttl":   60
  },
  "recorder": {
    "output_dir":        "/opt/recorder/recordings",
    "video_width":       1920,
    "video_height":      1080,
    "video_fps":         25,
    "audio_sample_rate": 8000,
    "audio_channels":    1,
    "video_codec":       "libx264",
    "audio_codec":       "aac",
    "video_bitrate":     1500000,
    "audio_bitrate":     64000,
    "rtp_port_base":     20000
  },
  "tcp": {
    "bind_addr": "0.0.0.0",
    "port":      9001
  },
  "log_dir":   "/opt/recorder/logs",
  "log_level": "info"
}
```

**重要：** 修改 `gk.host` 为 VP9660 MCU 的实际 IP 地址。

---

## 启动与停止

### 手动启动

```bash
/opt/recorder/bin/recorder-core -c /opt/recorder/config/config.json
```

### 后台运行（推荐）

```bash
nohup /opt/recorder/bin/recorder-core -c /opt/recorder/config/config.json \
    > /opt/recorder/logs/stdout.log 2>&1 &
echo $! > /opt/recorder/recorder.pid
echo "Started PID $(cat /opt/recorder/recorder.pid)"
```

### 停止

```bash
kill $(cat /opt/recorder/recorder.pid)
# 或
pkill recorder-core
```

### 查看日志

```bash
# 实时日志
tail -f /opt/recorder/logs/recorder.log

# 标准输出
tail -f /opt/recorder/logs/stdout.log
```

---

## TCP 控制接口

监听端口：`9001`，协议：每行一个 JSON 对象（换行结束）

### 查询状态

```bash
echo '{"cmd":"status"}' | nc 127.0.0.1 9001
```

响应示例：
```json
{"ok":true,"data":{"alias":"<recorder-alias>","gk_host":"10.84.100.XXX","output_dir":"/opt/recorder/recordings"}}
```

### 挂断指定通话

```bash
echo '{"cmd":"clear_call","token":"<call_token>"}' | nc 127.0.0.1 9001
```

### 挂断所有通话

```bash
echo '{"cmd":"clear_call"}' | nc 127.0.0.1 9001
```

---

## 测试步骤

### 第一步：更新配置

```bash
# SSH 到服务器
ssh ftadmin@<recorder_host>

# 编辑配置
vi /opt/recorder/config/config.json
# 将 "host": "10.84.100.XXX" 改为 VP9660 MCU 实际 IP
```

### 第二步：启动服务

```bash
/opt/recorder/bin/recorder-core -c /opt/recorder/config/config.json
```

启动后日志应看到：
```
[info] ControlServer: listening on 0.0.0.0:9001
[info] RecorderEndpoint: listening on TCP:1720
[info] RecorderEndpoint: registered with GK 10.84.100.XXX alias='<recorder-alias>'
[info] recorder-core ready — waiting for calls on TCP:1720
```

如果 GK 注册失败，会显示 warning 并继续运行（可直接被叫）：
```
[warning] RecorderEndpoint: GK registration failed for ... — continuing without GK
```

### 第三步：验证 GK 注册

在 VP9660 MCU 管理界面（如华为 SMC）中查看：
- 终端管理 → 在线终端列表
- 应能看到 `<recorder-alias>` 已注册

### 第四步：测试录制

在 VP9660 MCU 上发起一路会议，将 `<recorder-alias>` 加入：

```
MCU 呼叫 → <recorder-alias>（或直接拨打 <recorder_host>:1720）
```

服务端日志应显示：
```
[info] RecorderEndpoint: incoming call from 'MCU_NAME'
[info] RecorderEndpoint: attaching video capture 1920x1080
[info] RecorderEndpoint: attaching audio capture 8000Hz
[info] FfmpegRecorder: video stream 1920x1080 @ 25fps codec=libx264
[info] FfmpegRecorder: audio stream 8000Hz 1ch codec=aac
[info] FfmpegRecorder: opened /opt/recorder/recordings/20260409_180246_<recorder-alias>.mp4
[info] RecorderEndpoint: recording started token=... file=...
```

### 第五步：验证录制文件

挂断通话后：

```bash
ls -lh /opt/recorder/recordings/
# 应出现类似 20260409_180246_<recorder-alias>.mp4

# 检查文件完整性
ffprobe /opt/recorder/recordings/*.mp4
```

使用 VLC 或其他播放器播放 MP4 文件，验证视频画面与音频正常。

---

## 常见问题排查

| 现象 | 原因 | 解决方法 |
|------|------|----------|
| GK 注册失败 | IP 配置错误或 MCU 防火墙 | 检查 `gk.host` 配置；确保 UDP/1719 可达 |
| 无法接收呼叫 | 端口 1720 未开放 | `firewall-cmd --add-port=1720/tcp --permanent` |
| MP4 无视频 | H.323 视频协商失败 | 检查日志中 `attaching video capture` 是否出现 |
| MP4 无音频 | 音频编解码不匹配 | MCU 确认支持 G.711 或 G.722；查看日志 |
| 程序崩溃 | 共享库缺失 | `ldd /opt/recorder/bin/recorder-core \| grep "not found"` |

### 防火墙配置（如需）

```bash
sudo firewall-cmd --permanent --add-port=1720/tcp   # H.323 信令
sudo firewall-cmd --permanent --add-port=1719/udp   # H.323 GK RAS
sudo firewall-cmd --permanent --add-port=9001/tcp   # TCP 控制
sudo firewall-cmd --permanent --add-port=20000-20100/udp  # RTP 媒体
sudo firewall-cmd --reload
```

---

## 构建说明（开发环境）

服务器已安装所有依赖库于 `/usr/local/`。

```bash
cd /opt/recorder/recorder-core
rm -rf build && mkdir build && cd build
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig cmake ..
make -j$(nproc)
sudo cp recorder-core /opt/recorder/bin/
```

依赖库版本：
- GCC 12 (UOS gcc-toolset-12)
- FFmpeg 6.1.1 (libx264 + AAC)
- PTLib 2.10.9
- H.323Plus 1.27.2
- spdlog 1.13.0
- nlohmann/json 3.11.3
- Boost 1.73.0
