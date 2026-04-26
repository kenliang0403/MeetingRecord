# recorder-core v2.0 版本说明

**发布日期**：2026-04-23
**基线版本**：v1.0 → v2.0
**代码基线时间**：2026-04-17 → 2026-04-22

---

## 一、版本概述

v2.0 是 recorder-core 的一次重大功能升级，聚焦两个核心方向：

1. **H.239 辅流（双流）订阅能力的根本性重写** —— 从被动看门狗转为主动订阅，解决 VP9660 MCU 在晚加入（late-join）场景下不推送 H.239 辅流的问题。
2. **TCP 控制协议（ControlServer）能力大幅扩展** —— 由 2 个命令扩展至 8 个命令，支持完整运行态查询、配置热加载、单键值修改和录像清单查询。

同时附带 RTP 端口范围配置、端点运行态可观察性（`currentConnection()`）、以及 SRS 流媒体服务器启动脚本等增强。

---

## 二、改动文件清单

| 类别 | 文件 | 变更性质 |
| --- | --- | --- |
| 新增 | `scripts/start_srs.sh` | 新建（SRS 启动/检测脚本） |
| 删除 | `scripts/fetch_logs.ps1`、`scripts/remote_inspect.ps1` | 移除（开发期远程调试脚本） |
| 修改 | `src/main.cpp` | 控制命令大幅扩展（+216 行） |
| 修改 | `src/h323/RecorderConnection.h` / `.cpp` | H.239 主动订阅与 H.245 响应拦截（+196 行） |
| 修改 | `src/h323/RecorderEndpoint.h` / `.cpp` | RTP 端口配置、运行态 getter（+35 行） |
| 清理 | `logs/*.log` | 历史日志清理 |

---

## 三、重大功能改进

### 3.1 H.239 辅流订阅策略重写（核心变更）

**文件**：`src/h323/RecorderConnection.cpp`、`RecorderConnection.h`

#### v1 行为（被动看门狗）

```
入会 → 启动 5s 定时器 → 若超时未收到 H.239 OLC → 仅打印 warning
```

v1 依赖 VP9660 在收到 `terminalIDResponse` 后约 2s 自动推送 H.239 OLC。但在**会议已在进行中、录制端作为晚加入者**的场景下，VP9660 不会主动补推，导致辅流永远无法录制。

#### v2 行为（主动订阅 + 回退重试）

```
入会 → 2s 后发送 raw OLC(extendedVideo, session=10)
     → 收到 MCU 主动推送的 H.239 OLC 即成功
     → 未成功 → 按退避策略重试：5s / 10s / 15s×2 / 30s×N（最多 20 次）
     → 收到 openLogicalChannelReject → 置 h239Rejected_，停止重试
```

**关键技术决策**：

1. **绕过 H323Plus 的 `OpenLogicalChannel()`**
   H323Plus 内置的 OLC 流程对 `extendedVideoCapability` 不支持，会直接返回 FALSE。v2 参考抓包（TE-h239.pcap），手工构造 H.245 PDU 并通过 `WriteControlPDU()` 直接下发。

2. **单向通道（Recv-only）**
   省略 `reverseLogicalChannelParameters`，因为录制端是纯接收端。若包含反向参数，H323Plus 会把 Ack 当作双向通道打开，触发 `"Invalid cast to non-descendant class"` 断言崩溃。

3. **高编号 forwardLogicalChannelNumber（≥100）**
   避免与 MCU 主动打开的通道号冲突（MCU 通常使用 1、2、3…）。

4. **新增 `OnH245Response()` 拦截器**
   由于我们的 OLC 绕过了内部通道管理，H323Plus 默认的 Ack/Reject 处理会尝试按通道号查找 `H323Channel*` 对象，失败即断言崩溃。v2 在 `OnH245Response()` 中识别 `channel >= 100` 的响应并"吞掉"，从而保护进程不崩溃。

5. **PDU 字段要点**
   - `videoCapability`：genericVideoCapability，OID `0.0.8.241.0.0.1`（H.264），`maxBitRate=4096 kbps`
   - `videoCapabilityExtension`：genericCapability，OID `0.0.8.239.1.2`（H.239）
   - `h2250LogicalChannelParameters.sessionID = 10`（H.239 会话）

#### `OnHandleConferenceRequest` 重写

| 对比项 | v1 | v2 |
| --- | --- | --- |
| 处理的 Tag | `requestTerminalID` + `enterH243TerminalID` | 仅 `enterH243TerminalID`（其它交回 H323Plus） |
| TerminalLabel | v1 的 `enterH243TerminalID` 路径硬编码 `mcu=1, term=1` | 从请求中解码 `m_mcuNumber` / `m_terminalNumber` 并**回显**给 MCU |
| 触发时机 | 只作为 handshake 响应 | 响应后**立即触发**一次 H.239 订阅（1s 后） |

正确回显 `TerminalLabel` 是 VP9660 将录制端加入 H.239 分发列表的必要条件。

#### 新增状态标志与退避参数

```cpp
std::atomic<bool> h239Rejected_{false};   // MCU 明确拒绝 → 停止重试
// 重试间隔：attempt<3 → 5s；attempt<5 → 15s；否则 30s；>=20 次放弃
```

---

### 3.2 TCP 控制协议扩展（ControlServer）

**文件**：`src/main.cpp`

v1 仅支持 `status`（3 字段）、`clear_call`、`dial` 共 3 条命令。v2 扩展为 **8 条命令**，满足运维可观察性与运行期调优需求。

#### 命令清单

| 命令 | 类型 | 作用 | 新增 / 修改 |
| --- | --- | --- | --- |
| `status` | 查询 | 返回完整运行态（见下） | **扩展** |
| `config` | 查询 | 返回当前完整配置（密码脱敏） | **新增** |
| `reload` | 动作 | 从磁盘重读 `config.json` | **新增** |
| `set` | 动作 | 按点分路径修改单个配置项 | **新增** |
| `recordings` | 查询 | 列出 `output_dir` 下所有 `.mp4`（按 mtime 倒序） | **新增** |
| `version` | 查询 | 返回 name / version / config 路径 | **新增** |
| `clear_call` | 动作 | 清除当前通话 | 保留 |
| `dial` | 动作 | 手动呼叫 | 保留 |

#### `status` 返回字段

**v1**（3 字段）：`alias`、`gk_host`、`output_dir`

**v2**（13+ 字段）：

- 静态：`alias`、`gk_host`、`gk_port`、`output_dir`、`e164`
- 端点运行态：`gk_registered`、`in_call`、`call_token`、`reconnect_count`
- 连接运行态（若存在活动连接）：`meeting_name`、`caller_id`、`recording`、`main_file`、`h239_received`、`aux_recording`、`aux_file`

#### `set` 支持的键

`gk.host` · `gk.port` · `gk.alias` · `gk.e164` · `gk.password` · `outgoing.dial_number` · `outgoing.mcu_host` · `outgoing.enabled` · `outgoing.reconnect` · `log_level` · `recorder.output_dir`

- GK 相关键修改后返回 `restart required` 提示（H.323 注册需重启生效）
- `log_level` 修改即时重建 logger
- `recorder.output_dir` 修改时自动 `create_directories`

#### `reload` 语义

- 重新加载整个 `config.json`
- 检测 GK 五项关键字段是否变化，变化时在响应中附 `warning`
- 录制器参数对"下一次录制"生效，呼叫参数对"下一次拨号"生效

---

### 3.3 RecorderEndpoint 增强

**文件**：`src/h323/RecorderEndpoint.h` / `.cpp`

1. **RTP 端口范围配置**
   构造函数中调用 `SetRtpIpPorts(base, base+200)`，`base` 取自 `cfg.recorder.rtp_port_base`，为音频 + 视频 + H.239 会话预留充足端口。

2. **运行态 getter**
   - `isRegistered()` / `isInCall()` / `reconnectCount()`
   - `currentTokenStr()`：返回当前呼叫 token 字符串
   - `currentConnection()`：通过 `FindConnectionWithLock` 安全获取当前 `RecorderConnection*`（供 `status` / `recordings` 命令使用）

3. **RecorderConnection 新增 getter**
   - `hasH239()` / `isRecording()` / `isAuxRecording()` / `mainFilePath()` / `auxFilePath()` / `meetingName()` / `callerId()`

---

### 3.4 脚本与部署

- **新增** `scripts/start_srs.sh`：检测并启动 SRS 流媒体服务器（RTMP 1935 / HTTP 8080 / API 1985），支持幂等启动（已运行则跳过）。
- **移除** `scripts/fetch_logs.ps1`、`scripts/remote_inspect.ps1`：开发期远程调试脚本，不随产品发布。

---

## 四、兼容性说明

| 方面 | 影响 |
| --- | --- |
| 配置文件 `config.json` | **完全兼容**，未新增/删除字段 |
| 现有 `status`、`clear_call`、`dial` 命令 | **完全兼容**，`status` 返回字段仅新增不删除 |
| H.323 主流录制行为 | **无行为变化**，主流仍按 v1 逻辑打开 |
| H.239 辅流录制 | **显著改善**，晚加入场景现在可订阅成功 |
| H323Plus 依赖 | 无变化（仍为 third_party 内的 h323plus） |
| 构建系统 | `CMakeLists.txt` 无变化 |

---

## 五、升级与验证建议

1. **升级步骤**
   - 备份 v1 二进制与 `config.json`
   - 重新编译 v2 源码
   - 直接替换二进制，重启服务
   - `config.json` 无需改动

2. **验证要点**
   - `{"cmd":"version"}` 应返回 `{"name":"recorder-core","version":"2.0.0",...}`
   - `{"cmd":"status"}` 应返回扩展后的完整字段
   - 晚加入 VP9660 会议：应看到日志 `H.239 subscribe attempt 1 — sending raw OLC...` 并最终收到 `openLogicalChannelAck`
   - 若 MCU 拒绝 H.239：日志出现 `openLogicalChannelReject`，重试自动停止

---

## 六、修改统计

```
 scripts/start_srs.sh                   |  26 ++++
 scripts/fetch_logs.ps1                 | --- 删除
 scripts/remote_inspect.ps1             | --- 删除
 src/main.cpp                           | 216 +++++++++++++++++++++++++----
 src/h323/RecorderConnection.cpp        | 196 ++++++++++++++++++++++++----
 src/h323/RecorderConnection.h          |  16 +++-
 src/h323/RecorderEndpoint.cpp          |  25 ++++
 src/h323/RecorderEndpoint.h            |  10 ++
 共 5 个源文件修改、1 个脚本新增、2 个脚本删除
```

---

## 七、致谢与参考

- H.239 订阅 PDU 结构参考：`TE-h239.pcap`（帧 104 之后的 TE → MCU 交互）
- H.245 PDU 构造依赖：`third_party/h323plus` 的 `h245.h`（特别是 `H245_OpenLogicalChannel`、`H245_ExtendedVideoCapability`、`H245_GenericCapability`）
- 崩溃定位依据：h323plus `h245_1.cxx:11180` 处的"Invalid cast to non-descendant class"断言

