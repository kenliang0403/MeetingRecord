# 部署指南

> 完整的从零部署到生产可用的步骤。包含 **端口 / 防火墙 / 安全** 三块运维必读内容。

## 目标系统

- **操作系统**：UOS Server 20（CentOS / Anolis 衍生，dnf 包管理）
- **架构**：x86_64
- **最低硬件**：4 vCPU / 8GB RAM / 100GB disk（单路录像 + ASR + LLM 调用）
- **建议硬件**：8 vCPU / 16GB RAM / 200GB+ disk
- **网络**：能访问 GK + MCU；可选访问 LLM API（DeepSeek/通义/兼容 OpenAI）

## 端口规划

### 服务监听端口表

| 端口 | 协议 | 服务 | 用途 | **是否对外开放** |
|---|---|---|---|---|
| **1720** | TCP | recorder-core | H.323 H.225 信令（监听 GK / MCU 来的 SETUP） | ✅ 必须对 GK/MCU 段开放 |
| **20000-20200** | UDP | recorder-core | RTP 媒体端口（动态分配，主+辅+音频） | ✅ 必须对 GK/MCU 段开放 |
| 1719 | UDP | (出向) | GK 注册 (RAS) | ❌ 出向不需开放入向 |
| **8088** | TCP | recorder-web (gunicorn) | Web 管理 / 回放 / 直播页面 | ⚠️ 内网开放，**外网建议反代 + HTTPS** |
| 1935 | TCP | SRS | RTMP（recorder-core 推流给 SRS） | ❌ **仅 127.0.0.1 用**，不要对外 |
| 8080 | TCP | SRS | HTTP-FLV / HLS（浏览器 + bridge 拉流） | ❌ **仅本机 / 内网**，对外走 8088 反代 |
| 1985 | TCP | SRS | SRS API（统计/状态查询） | ❌ **绝不对外**（无鉴权） |
| 9001 | TCP | recorder-core ControlServer | JSON 控制命令（拨号/挂断/状态） | ❌ **仅 127.0.0.1** |
| 6006 | TCP | sherpa-onnx websocket server | ASR WebSocket | ❌ **仅 127.0.0.1**（bridge 用） |

### 防火墙最小配置（firewalld）

```bash
# 假定 GK/MCU 在 <gk_network>.0/24
sudo firewall-cmd --permanent --new-zone=h323
sudo firewall-cmd --permanent --zone=h323 --add-source=<gk_network>.0/24
sudo firewall-cmd --permanent --zone=h323 --add-port=1720/tcp
sudo firewall-cmd --permanent --zone=h323 --add-port=20000-20200/udp

# 内网（教委办公网段）能访问 web 管理页
sudo firewall-cmd --permanent --new-zone=office
sudo firewall-cmd --permanent --zone=office --add-source=<office_network>.0/24
sudo firewall-cmd --permanent --zone=office --add-port=8088/tcp

# 默认拒绝其它（包括 1935 / 8080 / 1985 / 9001 / 6006）
sudo firewall-cmd --permanent --set-default-zone=drop

sudo firewall-cmd --reload
```

**关键：** 1935 / 8080 / 1985 / 9001 / 6006 **永远不要对外**，它们都是无鉴权的内部接口，对外暴露 = 任何人都能 publish 流 / 查全部录像 / 控制录播。

### iptables 等价配置

```bash
# H.323 信令
iptables -A INPUT -p tcp --dport 1720 -s <gk_network>.0/24 -j ACCEPT
iptables -A INPUT -p udp --dport 20000:20200 -s <gk_network>.0/24 -j ACCEPT

# Web 管理
iptables -A INPUT -p tcp --dport 8088 -s <office_network>.0/24 -j ACCEPT

# 本机内部回环（必须）
iptables -A INPUT -i lo -j ACCEPT

# 默认拒绝
iptables -P INPUT DROP
```

## 目录布局

```
/opt/recorder/
├── bin/
│   └── recorder-core              # C++ binary（cmake install 出）
├── config/
│   └── config.json                # 运行时配置（alias / e164 / LLM / etc.）
├── recordings/                    # 会议录像（mp4 + meeting.json + transcript.jsonl）
│   └── <yyyymmdd>_<caller>/
│       ├── main_NN.mp4            # 主流分段
│       ├── aux_NN.mp4             # 辅流分段
│       ├── meeting.json           # 段元数据
│       ├── transcript.jsonl       # ASR 字幕（raw + punct）
│       ├── transcript.refined.jsonl  # LLM 优化版（如已生成）
│       └── summary.md             # LLM 会议纪要（如已生成）
├── run/
│   ├── transcript.jsonl           # live tail（SSE 读这个）
│   └── restart-recorder.flag      # path-unit 触发文件
├── logs/
│   └── stdout.log                 # 早期 nohup 时代日志（已废弃）
├── web/                           # Flask app deploy 后位置
│   ├── app.py
│   ├── auth.py
│   ├── auth.json                  # 用户哈希（chmod 0600，运维管）
│   ├── .flask_secret              # session 加密 key（持久化）
│   ├── static/ templates/
│   └── recorder-*.service / .path # systemd unit 模板（install_web.sh 用）
├── asr/
│   ├── venv/                      # Python 3.7 venv，装 websockets
│   ├── tmp/sherpa-onnx-v1.13.0-linux-x64-shared/   # ASR binary + libs
│   ├── models/
│   │   ├── sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/
│   │   ├── sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/
│   │   └── hotwords.txt           # 902 行教育领域热词（运维可编辑）
│   └── bridge/
│       ├── recorder-asr-bridge.py
│       └── asr_offline.py
├── scripts/                       # 启动/控制脚本（早期）
│   ├── start.sh / start-foreground.sh
│   └── ctrl_query.py              # TCP 9001 客户端
└── recorder-core/                 # **源码仓库 clone 位置**
    ├── src/ web/ scripts/ ...
    └── build/                     # cmake 输出
```

## 首次部署流程

### 1. 操作系统准备

```bash
# 1.1 装基础工具（root 或 sudo）
sudo dnf install -y gcc gcc-c++ cmake3 make pkgconfig git python3 python3-pip
sudo dnf install -y ffmpeg  # 4.2.4 from UOS repo
sudo dnf install -y openssh-server  # 远程运维用

# 1.2 创建运行账户（如果还没有）
sudo useradd -m -s /bin/bash ftadmin
sudo passwd ftadmin   # 设密码

# 1.3 准备目录
sudo mkdir -p /opt/recorder/{bin,config,recordings,run,logs,asr/{tmp,models,bridge,venv}}
sudo chown -R ftadmin:ftadmin /opt/recorder
```

### 2. 编译 recorder-core

```bash
# 2.1 clone 仓库到 /opt/recorder/recorder-core
cd /opt/recorder
git clone <repo-url> recorder-core
cd recorder-core

# 2.2 编译 H.323Plus + PTLib + FFmpeg 依赖
# (一次性，详细见 docs/development.md)
bash scripts/build.sh   # 构建第三方依赖（需 ~30min）

# 2.3 编译 recorder-core
mkdir -p build && cd build
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/opt/recorder \
  -DCMAKE_PREFIX_PATH=/usr/local
make -j recorder-core
sudo make install

# 2.4 安装初始配置
sudo cp ../config/config.json /opt/recorder/config/config.json
# 编辑 alias / e164 / gk.host 等
sudo vim /opt/recorder/config/config.json
```

### 3. 部署 SRS（外部组件）

SRS 安装文档见 https://ossrs.net/lts/zh-cn/docs/v5/doc/getting-started 。
关键配置（`/opt/recorder/srs/conf/srs.conf`）：

```nginx
listen              1935;
http_api { enabled on; listen 1985; }
http_server { enabled on; listen 8080; }

vhost __defaultVhost__ {
    hls { enabled on; hls_fragment 2; hls_window 30; }
    http_remux { enabled on; mount [vhost]/[app]/[stream].flv; }
    # 关键：允许 127.0.0.1 publish（recorder-core 推流）
    security {
        enabled on;
        allow publish all;   # 5.x 用 all，绑定到 127.0.0.1 由防火墙保证
    }
}
```

启动：`/opt/recorder/srs/objs/srs -c /opt/recorder/srs/conf/srs.conf`（可写 systemd unit）。

### 4. 部署 sherpa-onnx ASR

```bash
# 4.1 从 GitHub release 下载 binary
cd /opt/recorder/asr/tmp
curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.0/sherpa-onnx-v1.13.0-linux-x64-shared.tar.bz2
tar -xjf sherpa-onnx-v1.13.0-linux-x64-shared.tar.bz2
ls sherpa-onnx-v1.13.0-linux-x64-shared/bin/  # 应该有 sherpa-onnx-online-websocket-server

# 4.2 下载流式中文 ASR 模型（161MB）
cd /opt/recorder/asr/models
curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30.tar.bz2
tar -xjf sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30.tar.bz2

# 4.3 下载标点模型（62MB）
curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/punctuation-models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8.tar.bz2
tar -xjf sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8.tar.bz2

# 4.4 安装初始热词（902 行教育领域，已 filter 过模型词表）
cp /opt/recorder/recorder-core/scripts/asr/hotwords_default.txt /opt/recorder/asr/models/hotwords.txt

# 4.5 创建 Python venv，装 websockets
python3 -m venv /opt/recorder/asr/venv
/opt/recorder/asr/venv/bin/pip install -i https://pypi.tuna.tsinghua.edu.cn/simple websockets
/opt/recorder/asr/venv/bin/python -c "import websockets; print(websockets.__version__)"   # 11.0.3 OK
```

**102 国内访问 GitHub release 大文件不稳定**，建议先在 Windows 本机下载再 scp 到 102。

### 5. 部署 recorder-web + 4 个 systemd 服务

```bash
# 5.1 从 Windows 本机用 scripts/upload_web.ps1 一键部署
# (脚本会 scp web/ 全部文件 + 跑 install_web.sh)
.\scripts\upload_web.ps1 <recorder_host>

# 5.2 在 102 上：跑 install_asr.sh 安装 ASR 两个服务
.\scripts\install_asr.ps1 <recorder_host>

# 5.3 init web 用户（首次必须）
sudo python3 /opt/recorder/web/setup_user.py admin   # 设密码

# 5.4 配置 LLM（在 web 管理页 /config 填，或直接编辑 /opt/recorder/config/config.json）
{
  "llm": {
    "base_url": "https://api.deepseek.com",
    "api_key":  "sk-xxxxxxxxxxxxxxxx",
    "model":    "deepseek-chat"
  }
}
```

`install_web.sh` 安装的服务：

| 服务 | 用途 |
|---|---|
| `recorder-core.service` | H.323 录播主进程 |
| `recorder-web.service` | gunicorn + Flask :8088 |
| `recorder-restart.path` | 监听触发文件 |
| `recorder-restart.service` | 收到触发后 `systemctl restart recorder-core` |
| `recorder-asr.service` | sherpa-onnx websocket :6006 |
| `recorder-asr-bridge.service` | Python bridge |

### 6. 验证部署

```bash
# 6.1 4 个服务都 active
for s in recorder-core recorder-web recorder-asr recorder-asr-bridge; do
  systemctl is-active $s
done   # 期望全 active

# 6.2 GK 注册成功
journalctl -u recorder-core -n 20 | grep 'registered with GK'

# 6.3 浏览器登录管理页
# http://<recorder_host>:8088/login

# 6.4 触发一次 sudoless restart 测试
echo "$(date)" > /opt/recorder/run/restart-recorder.flag
sleep 3
systemctl status recorder-core | head -5
```

## 升级流程

### 升级 web 代码（仅前端 / 仅 .py / 仅 unit）

```bash
# Windows 本机
.\scripts\upload_web.ps1 <recorder_host>
```

新版 `install_web.sh`（commit `303b239`+）会**智能判断**：

| 改了什么 | 触发的重启 |
|---|---|
| `web/static/*` / `templates/*` only | **无重启** ✓ |
| `web/*.py` (app.py / auth.py / recorder_client.py) | recorder-web |
| `recorder-core.service` unit | recorder-core（**中断会议**） |
| `recorder-web.service` unit | recorder-web |
| `recorder-asr*.service` units | 这些 unit 不由 install_web.sh 管理 |

### 升级 recorder-core 二进制

```bash
# Windows 本机
.\scripts\upload_build.ps1   # 默认 102
.\scripts\deploy_104.ps1     # 104

# 新版 redeploy.sh（commit `1133c2b`+）用 install(1) atomic 替换 binary +
# 写触发文件让 path-unit 重启 systemd。同文件系统 rename，**不动 config.json**。
```

### 升级 ASR 模型 / hotwords

```bash
# Windows 本机直接 scp 新文件
scp scripts/asr/hotwords_default.txt ftadmin@102:/opt/recorder/asr/models/hotwords.txt
ssh ftadmin@102 "sudo systemctl restart recorder-asr"
# bridge 会自动重连 sherpa
```

模型升级见 [docs/operations.md](operations.md#asr-模型升级)。

## 安全

### 已知风险 + 缓解

| 风险 | 缓解措施 |
|---|---|
| **LLM API Key 明文存 config.json** | `chmod 0600 /opt/recorder/config/config.json`；运维独占读 |
| **Web 弱密码** | `setup_user.py` 强制密码长度；考虑接 LDAP/SSO（未实现）|
| **会议数据出境** | LLM 调用走 DeepSeek（中国境内 API），但本地数据仍发出。**敏感会议禁用 refine/summary**（不点按钮即可） |
| **录像目录无加密** | 文件系统层级加密（如 LUKS）+ 限制 ftadmin 之外用户读 `/opt/recorder/recordings/` |
| **SRS 1985 API 无鉴权** | 仅 127.0.0.1 绑定（防火墙阻外网） |
| **9001 ControlServer 无鉴权** | 仅 127.0.0.1 绑定 |
| **session cookie 30min idle 自动登出** | 已实现（`SESSION_TIMEOUT_MIN=30`） |
| **HTTP 明文** | 强烈建议加 nginx 反代 + Let's Encrypt 证书（见下方） |
| **sudoers drop-in 锁死 sudo** | 5/8 踩坑后已彻底废弃，**改用 systemd path-unit + 触发文件**，web 进程 0 sudo 权限 |
| **bridge 用 root socket?** | 否，bridge 是 ftadmin 跑，不需要任何特权 |

### HTTPS 反代（生产推荐）

```nginx
# /etc/nginx/conf.d/recorder.conf
server {
    listen 443 ssl http2;
    server_name recorder.example.com;

    ssl_certificate     /etc/letsencrypt/live/recorder.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/recorder.example.com/privkey.pem;
    ssl_protocols       TLSv1.2 TLSv1.3;

    # Flask 主接口
    location / {
        proxy_pass http://127.0.0.1:8088;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_read_timeout 300s;   # LLM 调用可能超 30s
    }

    # SSE 字幕流（要禁 buffer）
    location /api/transcript/stream {
        proxy_pass http://127.0.0.1:8088;
        proxy_set_header Connection "";
        proxy_http_version 1.1;
        proxy_buffering off;
        proxy_cache off;
        chunked_transfer_encoding off;
    }

    # SRS HLS（如果用浏览器跨域）
    location /live/ {
        proxy_pass http://127.0.0.1:8080/live/;
        # 添加 CORS（hls.js 需要）
        add_header Access-Control-Allow-Origin *;
    }
}

server {
    listen 80;
    server_name recorder.example.com;
    return 301 https://$host$request_uri;
}
```

加完反代后 firewall 只暴露 443，其余全 drop。

### 备份

| 内容 | 备份频率 | 方式 |
|---|---|---|
| 配置 `/opt/recorder/config/config.json` | 每次变更 | git 私有 repo |
| Web auth `/opt/recorder/web/auth.json` | 每次新增用户 | scp 异地 |
| 录像 `/opt/recorder/recordings/` | 每周/按需 | rsync 到 NAS |
| 系统配置 systemd units | 每次变更 | git（已 track） |
| **数据库** | 无（项目无 DB） | — |

录像目录大（每小时会议约 1.5GB），按业务保留期限定备份周期。`/opt/recorder/recordings/` 自带 [清理 7 天前会议的脚本模板](operations.md#磁盘空间管理)。

### 服务账户最小权限

- **ftadmin**：跑所有服务的 user。**不应**有 sudoers 任何 entry（5/8 踩坑后已清理 `/etc/sudoers.d/recorder-web`）。
- **systemd path-unit + service** 跑在 root，但**只能做一件事**：`systemctl restart recorder-core`（触发文件由 ftadmin 写）。

## 监控

详见 [docs/operations.md](operations.md#监控与告警)。简要：

- **systemctl status** 看服务存活
- **journalctl -u <service> -f** 看实时日志
- **bridge heartbeat** 每 30s 打 `heartbeat: audio_pushed=Xs msgs=Y finals=Z`，可作为存活心跳
- **/opt/recorder/recordings/<m>/meeting.json** 的 `last_activity_ms` 可作为最近会议时间戳
- 磁盘：`df -h /opt/recorder`，建议剩余 < 20% 报警

## 一键健康检查脚本

```bash
#!/bin/bash
# /opt/recorder/scripts/health.sh
echo "=== services ==="
for s in recorder-core recorder-web recorder-asr recorder-asr-bridge; do
  printf "  %-25s %s\n" "$s" "$(systemctl is-active $s)"
done
echo
echo "=== ports listening ==="
ss -lnt | grep -E ':(1720|1935|6006|8080|8088|9001)\b' | awk '{print "  "$4}' | sort -u
echo
echo "=== GK registration ==="
journalctl -u recorder-core -n 30 --no-pager | grep -oE "registered with GK [^ ]+ alias='[^']+'" | tail -1
echo
echo "=== disk ==="
df -h /opt/recorder | tail -1
echo
echo "=== last meeting ==="
ls -dt /opt/recorder/recordings/[0-9]* 2>/dev/null | head -1
```
