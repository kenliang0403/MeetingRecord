# 运维手册

> 日常运维、监控、故障排查、备份、升级、清理。

## 1. 健康检查（5 秒确认全栈）

```bash
ssh ${RECORDER_USER}@${RECORDER_HOST}

# 4 个服务存活
for s in recorder-core recorder-web recorder-asr recorder-asr-bridge; do
  echo "$s = $(systemctl is-active $s)"
done

# GK 注册
journalctl -u recorder-core -n 20 --no-pager | grep -oE "registered with GK .* alias='[^']+'" | tail -1

# 端口监听
ss -lnt | grep -E ':(1720|1935|6006|8080|8088|9001)\b' | awk '{print $4}' | sort -u

# 磁盘
df -h /opt/recorder

# 最近会议
ls -dt /opt/recorder/recordings/[0-9]* | head -3
```

正常输出：4 个 active + GK alias 显示 + 6 个端口 LISTEN + 磁盘 < 80% + 最近会议有目录。

## 2. 监控与告警

### Bridge heartbeat（最重要的存活指标）

```bash
journalctl -u recorder-asr-bridge -f --since "1 hour ago" | grep heartbeat
```

期望每 30 秒一条：
```
heartbeat: audio_pushed=120s msgs=87 finals=3 idle=0s
```

报警阈值：
- 60s 内**没有 heartbeat** → bridge 死了或卡了
- `idle > 60s` → watchdog 将触发 reconnect（自愈）
- `finals` 长时间不增（>5 分钟）但 `audio_pushed` 在涨 → sherpa 应用层卡，watchdog 会兜底

### recorder-core 注册状态

```bash
# 当前注册情况
journalctl -u recorder-core --since "5 min ago" | grep -E "registered with GK|GK registration failed"
```

`failed (transport/discovery)` 出现 → GK host/port 配置错误 或 alias 跟 e164 不一致 或 GK 拒绝。看 [docs/deployment.md](deployment.md) 端口表确认。

### 接入会议

```bash
journalctl -u recorder-core --since "10 min ago" | grep -E "incoming call|call established|RecorderConnection"
```

### 磁盘

```bash
df -h /opt/recorder
du -sh /opt/recorder/recordings/[0-9]* | sort -h | tail -10
```

90% 以上必须清理（见下文）。

### 系统资源

```bash
top -bn1 | head -20
free -h
```

recorder-core 单次会议典型：CPU 30-50% (单核, H.264 encode 用)、RAM 200-400 MB。
sherpa-onnx：CPU 20-40% (modified_beam_search), RAM 200 MB（含模型）。
其余服务 < 10 MB RAM 每个。

## 3. 实时日志查看

```bash
# 全部 4 个服务的合并日志（按时间）
journalctl -u recorder-core -u recorder-web -u recorder-asr -u recorder-asr-bridge -f

# 单服务
journalctl -u recorder-asr-bridge -f

# 历史（最近 N 分钟 / 时间窗口）
journalctl -u recorder-core --since "10 min ago" --no-pager
journalctl -u recorder-core --since "2026-05-20 13:00" --until "2026-05-20 15:00" --no-pager

# 过滤 ERROR / WARNING
journalctl -u recorder-core -p err --no-pager
```

## 4. 故障排查 Cookbook

### 问题：浏览器看不到回放视频

1. 看 `/recordings/<m>` 是否能打开（页面元素都在）
2. F12 → Network 看 `/play/<m>/main_01.mp4` 是不是 206 Partial Content
3. ssh 102 看 mp4 文件存在 + ffprobe 看 atom 结构（**moov 必须在前**说明 faststart 完成）

```bash
ffprobe -v error -show_format /opt/recorder/recordings/<m>/main_01.mp4
```

4. 如果 mp4 健康但浏览器播不了 → 浏览器 cache 问题，Ctrl+Shift+R 强刷
5. 如果 mp4 损坏（recorder-core 被 SIGKILL 时没 faststart）→ ffmpeg 手动重写：

```bash
ffmpeg -i broken.mp4 -c copy -movflags +faststart fixed.mp4
```

### 问题：回放页没字幕

1. 看 `/opt/recorder/recordings/<m>/transcript.jsonl` 是否存在
2. 如果不存在 → 当时 bridge 故障 / 在 bridge 修复之前。**跑 offline ASR 回填**：

```bash
PY=/opt/recorder/asr/venv/bin/python
SCR=/opt/recorder/asr/bridge/asr_offline.py
MID=<meeting_id>

# 单段
$PY $SCR $MID --mp4 main_01.mp4 --overwrite

# 多段
$PY $SCR $MID --mp4 main_01.mp4 --overwrite
for n in 02 03 04 05 06 07 08 09; do
  [ -f /opt/recorder/recordings/$MID/main_${n}.mp4 ] && \
    $PY $SCR $MID --mp4 main_${n}.mp4 --append
done
```

每段约 `audio_duration × 0.21` + ~30s punct。

3. 如果存在但**字幕跟视频对不上** → 看回放页"字幕偏移"控件，加几秒；或者 transcript.jsonl 的 `t` 时间戳跟 meeting.json 的 `wall_start_ms` 偏差太大（bridge 启动晚于 recorder-core 开始录的情况）。

### 问题：直播页没字幕（活会议）

1. 看 bridge 是否在 active 状态 + heartbeat
2. 看 SRS publish 是否 active：

```bash
curl -s http://127.0.0.1:1985/api/v1/streams/ | python3 -m json.tool | grep -E '(name|publish|frames)'
```

3. 看 ctrl_query：`in_call=true` + `main_sending=true`
4. 看 bridge 是否在收 sherpa 字幕：`heartbeat: ... finals=N` 中 N 在增长

如果 audio_pushed 涨但 finals 不涨 → **可能没人说话**（VAD 没触发）或 sherpa 应用层卡（watchdog 60s 内会自愈）。

### 问题：GK 注册失败

```
GK registration failed (transport/discovery)
```

最常见：alias 跟 e164 不一致。修：

```bash
# 编辑 config
sudo vim /opt/recorder/config/config.json
# 确认 gk.alias == gk.e164
# 触发重启
echo "$(date)" > /opt/recorder/run/restart-recorder.flag
```

Web 管理页 `/config` 改 alias 时**会自动同步 e164**（commit `ec822ab`），但直接 vim 编辑要手动保持一致。

### 问题：LLM 调用失败（refine / summary）

错误："Failed to fetch" / 502 → 看 web log：

```bash
journalctl -u recorder-web --since "5 min ago" -p err
```

常见原因：

| 错误 | 原因 | 修 |
|---|---|---|
| `model not found` | `llm.model` 写错（如 `deepseek-v4-pro` 不存在） | 改成 `deepseek-chat` 或 `deepseek-reasoner` |
| `Failed to fetch` 浏览器侧 | gunicorn worker timeout | 已设 `--timeout 300`，应该不会再有；查 unit 文件确认 |
| `LLM 第 X/Y 批输出长度不对` | max_tokens 不够 / model 不严格遵守 prompt | 减小 `ASR_REFINE_BATCH_SIZE`（环境变量），或换更好的 model |
| `401 Unauthorized` | API Key 过期/无效 | 重新生成 + 改 config.json |

### 问题：bridge silent fail（heartbeat 没出现）

5/13 踩坑：bridge ws connection 活但 sherpa 不回字幕，bridge 卡死。**已加 watchdog 自愈**（commit `cef828e`）。如果还遇到：

```bash
sudo systemctl restart recorder-asr-bridge
journalctl -u recorder-asr-bridge -f
```

watchdog 60s 触发后会自动 reconnect。如果重启后 1 分钟仍不出 heartbeat → 看 sherpa 状态：

```bash
sudo systemctl restart recorder-asr   # 先重启 sherpa
sleep 8
sudo systemctl restart recorder-asr-bridge   # 再重启 bridge
```

### 问题：会议中断（recorder-core crash / restart）

会议会自动续段：mp4 文件名 main_02.mp4, main_03.mp4 ... 拼在同一 meeting_id 目录。**meeting.json 的 segments 数组自动追加**，回放页能跨段同步播放。

无操作必要。除非 crash 频繁 → 看 crash 原因：

```bash
journalctl -u recorder-core --since "1 day ago" | grep -E "Stopped|Failed|Killing"
```

### 问题：H.323 信令异常 / 听不到声音

抓包：

```bash
sudo tcpdump -i any -w /tmp/cap.pcap "(port 1720 or portrange 20000-21000 or port 1719)"
# 在另一终端拨号或被叫
# Ctrl+C 停抓
tshark -r /tmp/cap.pcap -V | less
```

常用 grep：

```bash
tshark -r /tmp/cap.pcap -V | grep -A 5 "openLogicalChannel"
tshark -r /tmp/cap.pcap -V | grep -E "registrationRequest|registrationConfirm"
```

## 5. 磁盘空间管理

录像目录涨得快（每小时会议 ~1.5 GB main + 几十 MB aux）。**保留策略由运维定**，例如保留 14 天：

```bash
# Dry-run：列出 14 天前的会议
DAYS=14
TODAY=$(date +%Y%m%d)
CUTOFF=$(date -d "$DAYS days ago" +%Y%m%d)
find /opt/recorder/recordings -mindepth 1 -maxdepth 1 -type d -name '[0-9]*' \
  | while read d; do
      name=$(basename $d)
      date_part="${name:0:8}"
      [ "$date_part" -lt "$CUTOFF" ] && echo "$d ($(du -sh $d | cut -f1))"
    done

# 实际删（确认列表无误后）
find /opt/recorder/recordings -mindepth 1 -maxdepth 1 -type d -name '[0-9]*' \
  | while read d; do
      name=$(basename $d)
      date_part="${name:0:8}"
      [ "$date_part" -lt "$CUTOFF" ] && rm -rf "$d"
    done
```

可挂 cron 每天 03:00 跑（写到 `/etc/cron.daily/recorder-cleanup`）。

历史踩坑：早期版本（pre 2026-04）会写一些 `<yyyymmdd>_<HHMMSS>_<alias>.mp4` 散落根目录文件。用 `find /opt/recorder/recordings -mindepth 1 -maxdepth 1 -name '[0-9]*'` 一起匹配可清理。

## 6. ASR 模型升级

如果 sherpa-onnx 升级了 model（如更好的中文流式 zipformer）：

```bash
cd /opt/recorder/asr/models
# 1. 下新模型（建议 Windows 本机下完 scp）
# scp <Windows>:<path>/new-model.tar.bz2 ftadmin@102:/opt/recorder/asr/models/

# 2. 解压
tar -xjf new-model.tar.bz2

# 3. 改 systemd unit 的 ExecStart 引用新模型路径
sudo vim /etc/systemd/system/recorder-asr.service
# 改 --tokens / --encoder / --decoder / --joiner 路径

# 4. 重新过滤 hotwords（如果新模型 token 表不同）
python3 - <<'PY'
# 见 docs/api.md "hotwords 过滤" 段落
PY

# 5. reload + restart
sudo systemctl daemon-reload
sudo systemctl restart recorder-asr

# 6. 验证 0 token errors
START=$(date '+%Y-%m-%d %H:%M:%S')
sleep 8
journalctl -u recorder-asr --since "$START" --no-pager | grep -c "Cannot find ID for token"
# 期望 0
```

## 7. 升级 recorder-core binary（hot swap）

```bash
ssh ${RECORDER_USER}@${RECORDER_HOST} bash -lc "'
  cd /opt/recorder/recorder-core && git pull && \
  cd build && sudo cmake --build . --target recorder-core -j && \
  bash ../scripts/redeploy.sh \"$SUDO_PW\"
'"
```

新版 redeploy.sh 用 `install -m 0755 src dst`（atomic rename）+ 触发文件机制。**会议进行中也可热替换**（旧 binary 的 process 继续跑直到 systemd restart 把它 KILL 重启）。

会议被 KILL 中断 ~5-10s，然后 recorder-core 重启重新 listen 1720 + GK 重注册。**MCU 通常 30s 内会自动重拨**让会议续上。

## 8. 升级 web

详见 [docs/deployment.md](deployment.md#升级流程)。简言：

```bash
ssh ${RECORDER_USER}@${RECORDER_HOST} "cd /opt/recorder/recorder-core && git pull && bash scripts/install_web.sh '$SUDO_PW'"
```

智能不重启逻辑（commit `303b239`）：
- 纯前端改动（JS/CSS/HTML） → 0 重启
- Python 改动 → 重启 recorder-web（SSE 断开 ~2s）
- systemd unit 改动 → 重启对应服务

## 9. 备份恢复

### 备份关键资产

```bash
# 配置（每次变更后备份）
sudo cp /opt/recorder/config/config.json ~/backups/config_$(date +%Y%m%d).json

# Web 用户（每次新增用户后）
sudo cp /opt/recorder/web/auth.json ~/backups/auth_$(date +%Y%m%d).json

# 录像（按业务保留期，rsync 到 NAS）
rsync -av --delete /opt/recorder/recordings/ /mnt/nas/recorder_backup/recordings/
```

### 从备份恢复

```bash
# 配置
sudo cp ~/backups/config_<date>.json /opt/recorder/config/config.json
echo "$(date)" > /opt/recorder/run/restart-recorder.flag

# Web 用户
sudo cp ~/backups/auth_<date>.json /opt/recorder/web/auth.json
sudo systemctl restart recorder-web
```

## 10. 灾难恢复（102 整机挂了）

如果 102 整机不可恢复：

1. 准备一台新机器，按 [docs/deployment.md](deployment.md#首次部署流程) 完整跑一遍
2. 修改 GK 让新机的 IP 注册同样 alias（或者改 DNS 指到新机）
3. 从 NAS 同步录像 `/opt/recorder/recordings/` 回新机
4. 从备份 cp 回 `config.json` + `auth.json`
5. 全部 systemctl restart
6. 通知 MCU 重新呼叫

录像恢复后**已有的 transcript.jsonl / summary.md 都还能用**（路径相同）。

## 11. 升级期间的会议中断 SLA

| 操作 | 中断会议 | 自动恢复 | 用户感知 |
|---|---|---|---|
| 改 web/static/* | ❌ 不中断 | — | 无 |
| 改 web/app.py | ❌ 不中断录像 | 1-2s SSE 闪断 | 字幕短暂卡顿 |
| 改 web/*.service | 取决于哪个 | recorder-web 自动重启 | 同上 |
| 改 recorder-core.service | ✅ 中断 5-10s | MCU 30s 内重拨 | 会议短暂掉线 |
| redeploy.sh（C++ 改动） | ✅ 中断 5-10s | 同上 | 同上 |
| asr/install_asr.sh | ❌ 不中断 | bridge 2-3s 内重连 sherpa | 字幕短暂中断 |
| 升级 sherpa server | ❌ 不中断录像 | 同上 | 字幕中断 30-60s（模型加载） |

## 12. 防止 install_web.sh 意外重启 recorder-core

新版 `install_web.sh` 已经智能判断。**确认服务器上的 install_web.sh 是新版**：

```bash
grep "NEED_CORE_RESTART" /opt/recorder/recorder-core/scripts/install_web.sh
# 应该有几处匹配；如果 grep 无匹配 → 服务器上是旧版，git pull 一下源码就行
```

## 13. 应急联系信息

> 部署时由运维填入

| 角色 | 联系方式 |
|---|---|
| 主运维 | xxx |
| 备运维 | xxx |
| GK 管理员（MCU 厂商） | xxx |
| LLM API 续费/Key 管理 | xxx |
| NAS 备份管理员 | xxx |

---

## 附：常用 ssh 一行命令

```bash
# 看当前通话（直接调 TCP 9001，避免依赖额外的客户端脚本）
ssh ${RECORDER_USER}@${RECORDER_HOST} "echo '{\"cmd\":\"status\"}' | nc -q 1 127.0.0.1 9001"

# 触发 recorder-core 重启
ssh ${RECORDER_USER}@${RECORDER_HOST} 'echo "$(date)" > /opt/recorder/run/restart-recorder.flag'

# 直接重启 bridge
ssh ${RECORDER_USER}@${RECORDER_HOST} 'echo $PW | sudo -S systemctl restart recorder-asr-bridge'

# 看磁盘 + 最近 10 个会议
ssh ${RECORDER_USER}@${RECORDER_HOST} 'df -h /opt/recorder; ls -lhS /opt/recorder/recordings/[0-9]* 2>/dev/null | tail -10'

# tail bridge heartbeat
ssh ${RECORDER_USER}@${RECORDER_HOST} "journalctl -u recorder-asr-bridge -f --since '5 min ago' | grep heartbeat"
```
