# 架构文档

## 系统全景

```
                       ┌─────────────────────────────────────┐
                       │           外部组件                   │
                       │  ┌──────┐    ┌──────────────┐       │
                       │  │  GK  │    │  MCU (VP9660)│       │
                       │  └──┬───┘    └──────┬───────┘       │
                       │     │ RAS           │ H.225/H.245   │
                       └─────┼───────────────┼───────────────┘
                             │ 1719/udp      │ 1720/tcp
                             ▼               ▼ + 20000-20200/udp RTP
                    ┌────────────────────────────────────────────┐
                    │      recorder-core (C++)        :1720      │
                    │  ─ H.323Plus + PTLib                       │
                    │  ─ Vendor 伪装为华为 TE52（让 SMC 070E 工作）│
                    │  ─ TCS 注入 + nonStandard echo（晚入会辅流）│
                    │  ─ FfmpegRecorder (H.264 + AAC, fmp4)      │
                    │    └─ attachStreamer → SrsStreamer         │
                    │  ─ TCP ControlServer       :9001 (JSON)    │
                    │  ─ AudioLevelMeter (RMS/Peak)              │
                    └───────────────┬────────────────────────────┘
                                    │ RTMP push
                                    ▼
              ┌──────────────────────────────────────────┐
              │   SRS Server 5.x       :1935 / :8080     │
              │   recorder-main / recorder-aux           │
              └────┬─────────────────────────────┬───────┘
                   │ HTTP-FLV pull (low-latency) │ HLS pull
                   │                              │
                   ▼                              ▼
        ┌──────────────────────────┐   ┌────────────────────────┐
        │ recorder-asr-bridge (Py) │   │ Browser (hls.js)       │
        │  asyncio + websockets    │   │  /live page            │
        │  ┌─ ffmpeg subprocess    │   │  ┌─ EventSource SSE    │
        │  │   (FLV→16k mono PCM)  │   │  │  /api/transcript/   │
        │  │                       │   │  │   stream            │
        │  ├─ WS client → sherpa   │   │  │  (字幕)             │
        │  ├─ Watchdog (60s idle)  │   │  └─ video src = HLS    │
        │  └─ punct subprocess     │   │                        │
        │     (per-final post)     │   │  4s default delay      │
        └──────┬───────────────────┘   │  (匹配 HLS 缓冲)       │
               │ writes                 └────────────────────────┘
               ▼
        /opt/recorder/run/transcript.jsonl  ← SSE 读这个
        /opt/recorder/recordings/<m>/transcript.jsonl  ← 持久化
        
        ┌──────────────────────────────────────────────────┐
        │ recorder-asr (sherpa-onnx WS server) :6006       │
        │ streaming-zipformer-zh-int8 + 902 hotwords       │
        │ modified_beam_search + ct-transformer punct      │
        └──────────────────────────────────────────────────┘

        ┌──────────────────────────────────────────────────┐
        │  recorder-web (Flask + gunicorn 2 workers x      │
        │  4 threads, --timeout 300)            :8088      │
        │  ─ /login /config /live                          │
        │  ─ /recordings/<m> 回放页                        │
        │  ─ POST /transcript/refine → LLM (batched ×50)   │
        │  ─ POST /summary → LLM                           │
        │  ─ SSE /api/transcript/stream (tail jsonl)       │
        └──────────────────┬───────────────────────────────┘
                           │ HTTP POST
                           ▼
                  ┌─────────────────────────┐
                  │  External LLM API       │
                  │  (DeepSeek / 通义 ...)  │
                  └─────────────────────────┘
```

## 关键设计决策

### 1. 为什么 sudoless restart trigger（path-unit + flag file）

**5/8 踩坑**：原方案是 web app `subprocess.run(["sudo", "systemctl", "restart", ...])` + 装 `/etc/sudoers.d/recorder-web` 放行 NOPASSWD。Windows git checkout 把 sudoers 文件转 CRLF，visudo 拒绝整个文件 → **整台机器 sudo 完全锁死**。

替代方案：
- web 写 `/opt/recorder/run/restart-recorder.flag`（recorder 运行用户拥有，无需 sudo）
- `recorder-restart.path`（root 跑，PathChanged 监听）触发
- `recorder-restart.service`（root 跑）执行 `systemctl restart recorder-core`

web 进程**零 sudo 权限**。详见 `feedback_sudoers_crlf_lockout.md` memory + commit `1039729`。

### 2. 为什么 bridge 应用层 watchdog（不只靠 WS ping）

**5/13 踩坑**：bridge `asyncio.wait(FIRST_COMPLETED)` 等 feeder/recv 任一结束。sherpa 服务端"应用层卡死"（ws TCP 还在、不发任何字幕 message）时，feeder 永远 send、receiver 永远 await recv()，整个 session 2h13min 完全 silent。WS 协议层 ping_interval/ping_timeout **检测不出应用层静默**。

修复（commit `cef828e`）：第三个 task `watchdog(state)`，每 30s 输出 heartbeat，60s 内无 ws msg 强制 return → asyncio.wait 唤醒 → 整个 session cleanup → outer 循环 3s 后重连。最多丢 60s 字幕。

### 3. 为什么字幕 raw + punct 双线 + 回放页 dedup

- bridge 收 final → 立即写 raw 一行
- 异步 fork `sherpa-onnx-offline-punctuation`（~0.5s）→ 收到 punct 文本写第二行 `punct=true, replaces_segment=<seg>`
- 回放页 `_build_canonical_finals` 配对 raw 和 punct：text 用 punct（带标点更易读），timestamps 用 raw（per-char 时间，前端按时间增量显示用）

好处：**实时字幕**马上有；**回放字幕**等 punct 完成后用更干净版本。

### 4. 为什么 LLM refine 分批

DeepSeek 默认 `max_tokens=4096`，240 句一次性给 LLM 输出会被 truncate → JSON parse 失败 → 整次 refine abort。

修复（commit `45a0bed`）：5 批 × 50 句一批，每批独立 LLM 调用 + 长度校验，任一批失败整个 abort（原子性）。

### 5. 为什么 install_web.sh 智能不重启

每次升级 web 都跑 `install_web.sh`。原版无条件 `systemctl restart recorder-core` + `restart recorder-web`，即使只动了 `live.js` 也会**打断会议 ~5s**。

修复（commit `303b239`）：
- 比较 unit 文件 hash 没变 → 不重启 core
- 比较 `web/*.py` hash 没变 → 不重启 web
- 静态文件改动 → 0 重启

### 6. 为什么用 install(1) 替换 binary 不用 cp

cp 在 binary 正在被 exec 时报"文本文件忙"。`install -m 0755 src dst` 用 rename(2)：
- 同 fs 下 atomic
- 旧进程的 fd 仍指向旧 inode（继续跑）
- 新 starts 读新 inode

效果：**热替换 binary 不需要先停服**。详见 commit `1133c2b`（redeploy.sh / deploy_104.sh 改造）。

### 7. 为什么用 cjkchar modeling unit + Chinese hotwords

sherpa-onnx 支持 cjkchar / bpe / cjkchar+bpe 三种 modeling unit。zipformer-zh 用 cjkchar（单字 token）。

启动 server 时加 `--hotwords-file --hotwords-score=2.0 --decoding-method=modified_beam_search`，hotwords 文件中的词在 ASR 候选选择时被加分，提升专业术语识别率。

**关键约束**：hotwords 中的字必须在模型 `tokens.txt` 里（2002 token 表）。否则 server 启动报 "Cannot find ID for token X" 跳过整条 hotword。本项目脚本预过滤后 902 行可用，含 219 行因含 `础/幼/督/践/障/综/辅/促/伍/殊/拨/籍/沪/...` 等 token 词表外字符被弃用。这是模型词表的硬限制——只能靠 LLM refine 补救（commit `19b082d`）。

### 8. 为什么字幕延迟 4 秒（live 页面）

HLS 视频路径：
```
recorder-core → RTMP push → SRS HLS slicing (2s/segment × 2 segments buffer) → hls.js MSE → <video>
```
延迟 ~5-8 秒。

字幕路径：
```
recorder-core → RTMP push → SRS HTTP-FLV → bridge ffmpeg (low_delay+nobuffer) → sherpa (RTF 0.21) → SSE → 浏览器
```
延迟 ~1-2 秒。

差值 ~4-6 秒就是"字幕快于声音"现象。在 live.js 加 setTimeout(delay×1000) 延迟显示，默认 4 秒（实测匹配 HLS 缓冲）。

### 9. 为什么 mp4 是 fragmented (frag_keyframe + empty_moov)

回放需求 + 进程 crash safety 的折衷：
- 普通 mp4：moov atom 在文件末尾，进程 SIGKILL 时 moov 没写，整个文件**不可播**
- fragmented mp4：moov 在文件头（empty 占位），每个 fragment self-contained，**进程被 KILL 也能播**
- 副作用：seek 慢（没 sample table）

解决：close mp4 时**异步 faststart 重写**为普通 mp4（moov 在前），seek 即时（commit memory：`mp4 faststart 后处理`）。

### 10. 为什么 SrsStreamer 异步 + 无锁队列

H.264 encoder 在 FfmpegRecorder 主线程跑（同步处理 RTP packet）。如果 RTMP push 阻塞会拖慢 encoder → 丢帧。

设计：
- `SrsStreamer` 独立线程 + 无锁 lockfree-queue（容量 200 帧）
- `FfmpegRecorder::attachStreamer` 在编码完成后 `pushVideo/pushAudio` 克隆 packet 给 streamer
- streamer 满了就 drop（保证不阻塞 encoder）
- 自动断链重连

## 数据流细节

### 录像生成时间线

```
T+0       MCU → recorder-core: SETUP
T+5ms     TCS exchange (含 H.239 capability 注入)
T+50ms    OLC session=1 (audio G.722/G.711/AAC-LD)
          OLC session=2 (main video H.264)
T+100ms   call established
          ↓
T+100ms   FfmpegRecorder::open → main_01.mp4 (fmp4 header 写入)
          SrsStreamer::start → 连 RTMP rtmp://127.0.0.1/live/recorder-main
T+500ms   SRS 接受 publish，HLS .m3u8 开始生成
          bridge ffmpeg 拉到第一帧
T+1500ms  bridge 推第一帧 PCM 给 sherpa
T+2-5s    sherpa 出第一个 partial
T+5-15s   VAD endpoint → 第一个 final → bridge 写 transcript.jsonl
T+5.5s    bridge fork punctuate → 写第二条 punct 版本
T+5.5s    SSE 推浏览器
T+9s      浏览器 HLS 视频开始播（含 ~4s 缓冲）+ 字幕 4s 延迟后显示
          → 字幕同步音频 ✓
...
T+30min   会议结束 / 中断 → CLC → FfmpegRecorder::close
          → 后台 faststart 重写 main_01.mp4 (moov 移到头)
T+30:05   主 recorder-core stopped；bridge 5s timeout 后等下一个 publisher
```

### H.239 辅流时间线（远端有人演示）

```
M+0   MCU → recorder-core: OLC extendedVideoCapability session=10
      RecorderConnection::CreateLogicalChannel session=10
      → fallback-create H.239 extended-video RTP channel
M+50  FfmpegRecorder::open → aux_01.mp4 (独立 mp4)
      SrsStreamer (aux) 连 rtmp://.../recorder-aux
      h239Received_ = true
...
M+N   远端停止演示 → MCU → CLC session=10
      h239Received_ = false  ← commit a4f5115 修复
      FfmpegRecorder::close aux_01.mp4
      SrsStreamer (aux) thread exit
      → faststart aux_01.mp4
```

下一次远端再演示，aux_02.mp4 重新开始（不连续段，meeting.json 里 segments 数组追加）。

### LLM refine 时间线

```
用户点回放页"运行字幕优化"按钮
  ↓
POST /recordings/<m>/transcript/refine
  ↓
读 transcript.jsonl 拿 240 个 finals
  ↓
分 5 批，每批 50 句
  ↓
For each batch:
  prompt = system("保守纠错...") + user("[0] xxx\n[1] xxx ...")
  response_format = {"type": "json_object"}
  HTTP POST DeepSeek (~25s/batch)
  parse JSON {"refined": [...]}
  validate len(refined) == len(batch)
  ↓
all_refined = list1 + list2 + ... + list5
  ↓
写 transcript.refined.jsonl（保留原 t / segment，text 用 refined 版）
  ↓
返回 {ok, lines_total, lines_changed}
  ↓
前端刷新字幕（player.js loadCaptions(true)）
```

总耗时 ~2 分钟。gunicorn `--timeout 300` 配置后不会被 worker timeout 杀掉。

## 关键文件 / 类 速查

| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 入口 + RecorderApp / ControlServer status JSON 拼装 |
| `src/h323/RecorderEndpoint.{h,cpp}` | H.323 Endpoint 子类，vendor 改写（华为 TE52）、能力注册 |
| `src/h323/RecorderConnection.{h,cpp}` | 单次通话连接：TCS 注入、OLC 处理、070E 解析、H.239 主动演示、SMC 远端控制 |
| `src/media/FfmpegRecorder.{h,cpp}` | mp4 编码 / write / attachStreamer |
| `src/media/SrsStreamer.{h,cpp}` | RTMP 异步推流（独立线程） |
| `src/media/Mp4Faststart.{h,cpp}` | fmp4 → normal mp4 后处理 |
| `src/meeting/MeetingContext.*` | 会议目录 + meeting.json 管理（3 天窗口合并） |
| `src/control/TcpControlServer.*` | TCP :9001 JSON 控制 |
| `web/app.py` | Flask 主应用（所有 HTTP endpoint） |
| `web/static/player.js` | 回放页：双流同步 + 字幕同步 + 字幕 toggle |
| `web/static/live.js` | 直播页：hls.js + SSE 字幕 + 延迟控件 |
| `scripts/asr/recorder-asr-bridge.py` | ASR pipeline 核心 |
| `scripts/asr/asr_offline.py` | mp4 离线 ASR rescue |
| `scripts/install_web.sh` | systemd unit + web 文件一键安装（智能不重启） |
| `scripts/redeploy.sh` | recorder-core binary 热替换（install + 触发文件） |
| `scripts/asr/install_asr.sh` | ASR 两个服务一键安装 |

## 历史里程碑

| Tag/Commit | 描述 |
|---|---|
| v1.0 | H.323 录播基础（无 web，nohup 启动） |
| v2.0 | H.239 晚入会修复（vendor 伪装华为）|
| 2026-04-30 | SMC 070E 远端控制演示工作 |
| 2026-05-01 | Web 管理页 + VU 表 + 配置编辑 + 回放页 |
| 2026-05-03 | 直播 / 回放页面拆分 |
| 2026-05-07 | systemd 取代 nohup；sudoers → path-unit |
| 2026-05-08 | sherpa-onnx 集成：实时字幕端到端工作 |
| 2026-05-09 | LLM 字幕优化 + 会议纪要按钮 |
| 2026-05-18 | install_web.sh 智能不重启 |
| 2026-05-20 | bridge JSON cmd 修复（per-meeting transcript 终于工作）|
