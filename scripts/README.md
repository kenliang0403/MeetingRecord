# scripts/ — 部署核心脚本

这里只放**部署服务器必需**的脚本（编译、安装、systemd unit、ASR 桥）。
不依赖 Windows、不含任何主机/账号信息，可直接 ssh 上服务器跑。

## 文件清单

```
scripts/
├── build.sh                          首次部署：编译 H323Plus + PTLib + FFmpeg + recorder-core
├── install_web.sh                    装 Flask + gunicorn + 4 个 systemd unit + 重启触发机制
├── redeploy.sh                       atomic install binary + 触发 systemd 重启（增量升级用）
├── start-foreground.sh               systemd ExecStart 实际跑的入口（设 LD_LIBRARY_PATH 等）
└── asr/
    ├── install_asr.sh                装 sherpa-onnx + bridge + 2 个 ASR systemd unit
    ├── recorder-asr-bridge.py        核心：ffmpeg → sherpa → transcript.jsonl 桥
    ├── asr_offline.py                离线 rescue：从 mp4 回填字幕
    ├── hotwords_default.txt          902 行教育领域热词
    ├── recorder-asr.service          sherpa-onnx websocket server 的 systemd unit
    └── recorder-asr-bridge.service   bridge 的 systemd unit
```

## 部署流程（首次）

详细步骤见 [`docs/deployment.md`](../docs/deployment.md)，简版：

```bash
# 1. 上传源码到服务器 /opt/recorder/recorder-core
ssh <user>@<recorder_host>
sudo mkdir -p /opt/recorder && sudo chown $USER /opt/recorder
git clone <your-repo-url> /opt/recorder/recorder-core
cd /opt/recorder/recorder-core

# 2. 复制配置模板并填值
sudo mkdir -p /opt/recorder/config
sudo cp config/config.json /opt/recorder/config/config.json
sudo vim /opt/recorder/config/config.json    # 填 GK host / alias / e164

# 3. 编译（首次约 30 分钟）
bash scripts/build.sh

# 4. 装 web + systemd unit（提示输入 sudo 密码）
bash scripts/install_web.sh "$(read -s -p 'sudo password: ' p; echo $p)"

# 5. 装 ASR（先按 docs/deployment.md 下好 sherpa-onnx + 模型）
bash scripts/asr/install_asr.sh "$SUDO_PW"

# 6. 加 web 用户
sudo python3 /opt/recorder/web/setup_user.py admin
```

## 增量升级

改了 C++ 源码：

```bash
ssh <user>@<recorder_host>
cd /opt/recorder/recorder-core
git pull
cd build && sudo cmake --build . --target recorder-core -j
bash ../scripts/redeploy.sh "$SUDO_PW"
```

改了 web / asr：

```bash
git pull
bash scripts/install_web.sh "$SUDO_PW"        # 智能不重启（仅必要时）
bash scripts/asr/install_asr.sh "$SUDO_PW"    # 重启 bridge
```

## 自定义运行用户

默认所有脚本把文件 chown 给 `ftadmin`，systemd unit 也填 `User=ftadmin`。
如果你的部署用户不是 ftadmin：

```bash
RUN_USER=deploy bash scripts/install_web.sh "$SUDO_PW"
RUN_USER=deploy bash scripts/asr/install_asr.sh "$SUDO_PW"
```

`install_*.sh` 会在 `RUN_USER != ftadmin` 时自动 sed 渲染 service unit（替换 User= / Group= / /home/ftadmin）后再装到 `/etc/systemd/system/`。

## 想要更友好的 Windows 一键部署？

仓库里**没**提供 `.ps1` 包装脚本和 SSH 自动应答助手 —— 这套是项目维护者私用的，因为含具体服务器名、登录方式等私有信息。

如果你想要类似的"Windows 本机一行命令推到服务器"工作流，参考下面的最小实现：

```powershell
# upload_web.ps1（自己写）
$ErrorActionPreference = "Stop"
$RECORDER_HOST = $env:RECORDER_HOST       # 例如 10.0.0.10
$RECORDER_USER = $env:RECORDER_USER       # 例如 ftadmin
$target = "${RECORDER_USER}@${RECORDER_HOST}"

scp -r web/. "${target}:/opt/recorder/recorder-core/web/"
scp scripts/install_web.sh "${target}:/opt/recorder/recorder-core/scripts/install_web.sh"
ssh $target "bash /opt/recorder/recorder-core/scripts/install_web.sh 'YOUR_SUDO_PASSWORD'"
```

实际生产建议用 SSH 公钥认证（不要往 ssh 里推密码），并把上述变量放到 `.env`（已 gitignored）。

## ctrl_query — TCP 9001 控制接口

`docs/api.md` 里有完整协议描述，纯 JSON 行协议。最小客户端（10 行 Python）：

```python
import socket, json
def send(cmd):
    s = socket.socket(); s.connect(("127.0.0.1", 9001))
    s.sendall((json.dumps(cmd) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        buf += s.recv(4096)
    s.close()
    return json.loads(buf.decode())

print(send({"cmd": "status"}))
print(send({"cmd": "dial", "number": "<mcu-number>"}))
```

或者一行 netcat：

```bash
echo '{"cmd":"status"}' | nc -q 1 127.0.0.1 9001
```
