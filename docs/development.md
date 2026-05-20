# 开发指南

> 本地搭环境、编译、修改代码、测试、发布的全流程。

## 开发机要求

- **OS**：Windows 10/11（部署到 Linux 102/104，开发可在 Windows 用 PowerShell + scp/ssh + git）
- **可选 Linux 子系统 WSL**：跑 bash 脚本更方便（也可直接 PowerShell）
- **Git**：必须，强烈建议配 LF endings（仓库 `.gitattributes` 已锁 `*.sh / *.service / *.path / *.sudoers` 为 LF）
- **Python 3.7+**：本地不强求（实际开发都在远端跑）
- **Visual Studio Code** 或类似 IDE
- **OpenSSH 客户端**：Windows 10+ 自带 `ssh.exe` / `scp.exe`
- **能直连 102 / 104 SSH**：默认 ftadmin 用户 + 密码（密码存 `.env` 文件，详见下方）

## 仓库布局（开发者视角）

```
.
├── src/                    # C++ recorder-core 源码
│   ├── h323/               # H.323 信令处理
│   ├── media/              # 编码/录制/推流
│   ├── meeting/            # 会议目录管理
│   ├── control/            # TCP 控制
│   ├── config/             # 配置读取
│   └── main.cpp
├── third_party/            # H.323Plus / PTLib 源码（git submodule 或 tarball）
├── web/                    # Flask app + 前端 + systemd unit 模板
│   ├── app.py              # 所有 HTTP endpoint
│   ├── auth.py             # 用户哈希校验
│   ├── recorder_client.py  # TCP 9001 客户端
│   ├── static/             # JS / CSS / 图片
│   ├── templates/          # Jinja2 HTML
│   ├── recorder-core.service       # systemd unit 模板
│   ├── recorder-web.service
│   ├── recorder-restart.path
│   └── recorder-restart.service
├── scripts/                # 部署核心脚本（公开）
│   ├── README.md           # 使用说明
│   ├── build.sh            # 首次部署：编译 H323Plus/PTLib/FFmpeg/recorder-core
│   ├── install_web.sh      # 装 web + 4 个 systemd unit（智能不重启）
│   ├── redeploy.sh         # atomic install binary + 触发 systemd 重启
│   ├── start-foreground.sh # systemd ExecStart 入口
│   └── asr/
│       ├── recorder-asr-bridge.py        # ffmpeg → sherpa → transcript 桥
│       ├── asr_offline.py                # 离线 rescue 字幕
│       ├── install_asr.sh                # 装 ASR 2 个 unit
│       ├── recorder-asr.service
│       ├── recorder-asr-bridge.service
│       └── hotwords_default.txt   # 902 行教育领域热词
├── config/
│   └── config.json         # 模板，部署时复制到 /opt/recorder/config/
├── docs/                   # 本文档
└── CMakeLists.txt
```

## 关键约定

### .env 文件（gitignored）

如果你写自己的 Windows 部署包装脚本，仓库根目录创建 `.env`（从 `.env.example` 复制再填值）：

```bash
RECORDER_HOST=<your-recorder-host-or-ip>
RECORDER_USER=ftadmin
RECORDER_SSH_PASSWORD=<your password>
RECORDER_HOST_SECONDARY=
```

公开仓库里**不包含** PowerShell 部署包装脚本（含开发者私人主机/账号信息），但 `.env.example` 是模板，可作为参考。

如果你直接在服务器上跑 `bash scripts/install_web.sh "$SUDO_PW"`，根本不需要 `.env`。

### 行尾约定

`.gitattributes` 强制：

```
*.sh           text eol=lf
*.service      text eol=lf
*.path         text eol=lf
*.sudoers      text eol=lf
*.timer        text eol=lf
*.socket       text eol=lf
*.target       text eol=lf
*.mount        text eol=lf
```

**为什么**：Windows git autocrlf 会把 LF 转 CRLF，导致 visudo / bash 拒绝执行（5/8 sudoers 锁死事故）。`.py` / `.js` / `.css` 跨平台都能 CRLF/LF 不强制。

新增 Linux-only 文件类型记得加进 `.gitattributes`。

## 四种典型开发流程

> 假设源码已在服务器 `/opt/recorder/recorder-core`，`$REC=<user>@<recorder_host>` 是部署主机，`$SUDO_PW` 是 sudo 密码。
> Windows 开发者一般会自己写一个 `.ps1` 包装把下面这几步合一行（参考 [`scripts/README.md`](../scripts/README.md)）。

### A. 前端改动（live.js / player.js / template / css）

本地编辑后推上去 → 跑 install_web.sh，**不重启 recorder-core / recorder-web**：

```bash
git add -u && git commit -m "..." && git push
ssh $REC "cd /opt/recorder/recorder-core && git pull && bash scripts/install_web.sh '$SUDO_PW'"
# 浏览器 Ctrl+Shift+R 看效果
```

### B. Web Python 改动（app.py / auth.py）

同上。`install_web.sh` 自动判断 `*.py` 变了 → 重启 `recorder-web`（不重启 recorder-core）。SSE 断开 1-2s。

### C. recorder-core C++ 改动

```bash
git push
ssh $REC bash -lc "'
  cd /opt/recorder/recorder-core && git pull && \
  cd build && sudo cmake --build . --target recorder-core -j && \
  bash ../scripts/redeploy.sh \"$SUDO_PW\"
'"
```

`redeploy.sh` 用 `install(1)` atomic 替换 binary + 写触发文件让 path-unit 重启 systemd。**不动 `/opt/recorder/config/config.json`**。

### D. ASR / 字幕相关（bridge.py / hotwords）

```bash
git push
ssh $REC "cd /opt/recorder/recorder-core && git pull && bash scripts/asr/install_asr.sh '$SUDO_PW'"
```

紧急 hot-patch（不重启 sherpa，只更新 bridge）：

```bash
scp scripts/asr/recorder-asr-bridge.py $REC:/opt/recorder/asr/bridge/recorder-asr-bridge.py
ssh $REC "echo '$SUDO_PW' | sudo -S systemctl restart recorder-asr-bridge"
```

## C++ 编译（在 102 上做，开发机不编）

第一次准备：

```bash
# 102 上
cd /opt/recorder
git clone <repo-url> recorder-core
cd recorder-core

# 编译第三方依赖（PTLib + H.323Plus + FFmpeg 6.x）
bash scripts/build.sh   # 约 30 分钟
```

`build.sh` 会：
- 解压 `third_party/tarballs/h323plus-1_27_2/`，编译装到 `/usr/local`
- 解压 `third_party/tarballs/ptlib-2_18_8/`，同上
- 解压 FFmpeg 6.1 tarball，配置 `--enable-libx264 --enable-shared` 编译装到 `/usr/local`

后续 incremental build：

```bash
cd /opt/recorder/recorder-core/build
sudo cmake --build . --target recorder-core -j
sudo install -m 0755 ./recorder-core /opt/recorder/bin/recorder-core
echo "$(date)" > /opt/recorder/run/restart-recorder.flag
```

Windows 端可以写一个 `.ps1` 包装把上面这套自动化。仓库不带（含主机/账号信息），参考 [`scripts/README.md`](../scripts/README.md) 的最小实现示例。

### 关键编译坑

- **PASN_OctetString 赋值字符串**：不能用 `m_field = "xxx"`（部分版本会附 NUL 字节）。**永远**用 `m_field.SetValue(reinterpret_cast<const BYTE*>(s), strlen(s))`，明确给长度。否则 PER 编码错位 → GK 拒注册。
- **H245_TerminalID UTF-8 截断**：const char* 构造函数在首个非可打印字节处截断。同上，用 SetValue 显式给字节序列。
- **PTLib 主循环不能干净退出**：SIGTERM 后主线程不返回 main()，systemd 配 `TimeoutStopSec=3 KillMode=mixed` 强制 KILL 兜底。

## 在本地（开发机）查代码 / 调试

- 大部分调试是看 102 上的 journalctl：

```powershell
ssh ${RECORDER_USER}@${RECORDER_HOST}
journalctl -u recorder-core -f --since "5 min ago"   # 实时 follow
journalctl -u recorder-asr-bridge -n 100 --no-pager  # 最近 100 行
```

- 抓包看 H.323 信令（详见 memory 文档 `recorder-core_project_state.md` 待测段落）：

```bash
sudo tcpdump -i any -w /tmp/cap.pcap port 1720 or portrange 20000-21000 or port 1719
tshark -r /tmp/cap.pcap -V | grep -A 20 enterH243TerminalID
```

- Python bridge 本地调试：装 venv + pip install websockets，跑 `recorder-asr-bridge.py`，但 sherpa server 必须在 102（除非本机也装 sherpa-onnx）。所以一般直接在 102 改文件 + restart：

```bash
ssh ${RECORDER_USER}@${RECORDER_HOST}
sudo vim /opt/recorder/asr/bridge/recorder-asr-bridge.py
sudo systemctl restart recorder-asr-bridge
journalctl -u recorder-asr-bridge -f
```

## 测试 Checklist

修改后部署前确认：

### C++ 改动

- [ ] `cmake --build .` 通过，无 warning 级别提升
- [ ] systemctl restart recorder-core 后 GK 注册成功（`journalctl -u recorder-core | grep registered`）
- [ ] 手动拨号或被叫一次小会议（< 1 分钟），确认 main_01.mp4 + meeting.json 写出来
- [ ] 如果改动涉及 H.239 → 触发一次远端演示（SMC 070E）测试 aux 流
- [ ] mp4 用 ffprobe 看 atom 结构 + duration 正常

### Web 改动

- [ ] 浏览器登录 /login 成功
- [ ] /，/config，/live，/recordings 都能打开
- [ ] 改的 endpoint 用 curl 或浏览器 DevTools 测响应正确
- [ ] SSE 字幕流 `/api/transcript/stream` 在播会议时持续推送

### ASR 改动

- [ ] bridge service active running
- [ ] 模拟推 m4a 测试：`ffmpeg -re -i /tmp/asr_input.m4a -t 30 -vn -c:a aac -b:a 128k -ac 1 -ar 48000 -f flv rtmp://127.0.0.1/live/recorder-main`
- [ ] 看 `/opt/recorder/run/transcript.jsonl` 有新 final
- [ ] 看 bridge heartbeat（30s 一条）有 `finals++`

## 版本控制约定

### Commit 信息

- 短标题：动作 + 主体（`Web: add transcript refine endpoint`）
- body 描述 why（不是 what — diff 已经显示了 what），关键决策点
- 多文件改动 list 每个文件做了什么
- 修 bug 时引用问题来源（"5/13 silent hang"）

### 分支

- `main` — 生产版本
- `claude/<topic>` — Claude 协作开发分支，长跑后 fast-forward 合到 main
- 短期改动直接在 main（仓库只 1-2 人合作时这样最快）

### Push 到 main

```bash
git push origin <branch>:main   # fast-forward 合并
# 或：
git checkout main && git merge --ff-only <branch> && git push
```

## 历史经验教训

- **sudoers drop-in 触发服务重启** — 曾因 CRLF 让 visudo 拒绝并锁死整台机器 sudo。现已改用 systemd path-unit + 触发文件（`/opt/recorder/run/restart-recorder.flag`）。
- **GitHub 大文件直接在服务器下载不稳** — 模型 / 二进制 tarball 建议先在开发机下，再 `scp` 到服务器。
- **PASN_OctetString / H245_TerminalID 字符串赋值** — 必须用 `SetValue(BYTE*, len)`，不能用 `= "string"`（会被截到首个非可打印字节）。

## 添加新 LLM 功能的模板

如果要给某个 endpoint 加 LLM 调用：

```python
@app.route("/recordings/<m>/<feature>", methods=["POST"])
@login_required
def some_llm_feature(m):
    # 1. 路径安全检查
    base = Path(RECORDINGS_DIR)
    target = (base / m).resolve()
    if not str(target).startswith(str(base.resolve())): abort(404)
    if not target.exists(): abort(404)

    # 2. 读 LLM 配置
    cfg = json.loads(Path(CONFIG_PATH).read_text(encoding="utf-8"))
    llm = cfg.get("llm", {})
    base_url, api_key, model = (
        llm.get("base_url","").strip(),
        llm.get("api_key","").strip(),
        llm.get("model","").strip(),
    )
    if not all([base_url, api_key, model]):
        return jsonify({"ok": False, "error": "LLM 未配置"}), 400

    # 3. 准备 prompt（system 严格规则，user 数据）
    #    系统提示词内**不要嵌入 ASCII 双引号**（会截断 Python 字符串），
    #    用『中文引号』代替
    system_prompt = "你的任务是 ... 不可以做的事 ..."
    user_prompt   = "数据：..."

    # 4. 调用（response_format 强制 JSON 推荐）
    text, err = _llm_chat_complete(
        base_url, api_key, model,
        messages=[
            {"role": "system", "content": system_prompt},
            {"role": "user",   "content": user_prompt},
        ],
        timeout=180,
        response_format={"type": "json_object"},   # 可选
    )
    if err:
        return jsonify({"ok": False, "error": f"LLM 调用失败：{err}"}), 502

    # 5. 解析 + 写文件
    try:
        data = json.loads(text)   # 如果 response_format=json
    except json.JSONDecodeError as e:
        return jsonify({"ok": False, "error": f"LLM 输出不是合法 JSON：{e}"}), 502

    out = target / "<feature>.md"   # 或 .json
    out.write_text(..., encoding="utf-8")

    return jsonify({"ok": True, "..."})
```

注意 gunicorn 已配 `--timeout 300`，单次 LLM 调用 < 5 分钟都 OK。如果可能更长，分批（参考 `transcript/refine` 的 50 句/批实现）。
