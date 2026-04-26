# v3.0.0 服务器端部署与测试对比报告

**日期**：2026-04-23
**版本**：recorder-core v3.0.0
**服务器**：`ftadmin@<recorder_host>`
**二进制路径**：`/opt/recorder/bin/recorder-core`
**配置**：`/opt/recorder/config/config.json`（alias=`<alias-1>`, E.164=`<alias-1>`, GK=`<gk_host>:1719`）

---

## 一、测试环境

| 项 | 值 |
| --- | --- |
| 服务器 | <recorder_host> / ens192 |
| recorder-core 进程 | PID 355155 |
| TCP 控制端口 | 9001 |
| H.225 信令 | 1720 |
| 本次呼叫目标会议号 | **<dial-number>**（SMC 当前会议号） |
| 抓包 1（SMC 测试） | `logs/v3_test_20260423/smc_test.pcap`（48 MB） |
| 抓包 2（服务器测试） | `logs/v3_test_20260423/server_test.pcap`（5.3 MB） |
| 进程日志 | `logs/v3_test_20260423/recorder.log` |

---

## 二、两组测试的原始事件时间线

### 2.1 SMC 发起（MCU → 录制端，入呼）

| 时间 | 事件 | Token | 说明 |
| --- | --- | --- | --- |
| 09:25:21 | Q.931 Setup 入 | `ip$<mcu_host>:1027/3` | SMC 第 1 次呼叫 |
| 09:25:27 | Q.931 Connect | | 建链成功 |
| 09:25:33 | 主录像开始 | | `20260423_092533_6501.mp4`(59 MB) |
| 09:25:35 | 辅录像开始 | | `20260423_092535_6501_aux.mp4`(1.9 MB) |
| 09:27:23 | Q.931 ReleaseComplete（双向） | | SMC 挂断，reason=3 |
| | | | |
| 09:27:55 | Q.931 Setup 入 | `ip$<mcu_host>:1027/4` | SMC 第 2 次呼叫 |
| 09:28:01 | Q.931 Connect | | 建链成功 |
| 09:28:07 | 主录像开始 | | `20260423_092807_6501.mp4`(54 MB) |
| 09:28:09 | H.239 订阅成功 | | raw OLC → Ack → MCU 推送 OLC（session=10） |
| 09:28:09 | 辅录像开始 | | `20260423_092809_6501_aux.mp4`(**12.6 MB**，内容完整) |
| 09:29:47 | Q.931 ReleaseComplete（双向） | | SMC 挂断，reason=3 |
| 09:29:47 | 主/辅录像关闭 | | 文件落盘完整 |

### 2.2 服务器发起（录制端 → MCU，外呼 <dial-number>）

| 时间 | 事件 | Token | 说明 |
| --- | --- | --- | --- |
| 09:36:30 | `dial number=<dial-number>` | `ip$localhost/11358` | Cycle 1 开始 |
| 09:36:43 | `call established` | | GK 解析 → MCU 接受，13s |
| 09:36:43 | 主录像开始 | | `20260423_093643_6501.mp4`(**8.1 MB**) |
| 09:36:45 | H.239 订阅 attempt #1 | | raw OLC 发送成功，60ms 后收到 MCU 的 OLC |
| 09:36:45 | 辅录像开始 | | `20260423_093645_6501_aux.mp4`(**1.0 MB**) |
| 09:36:58 | `clear_call`（本地主动挂断） | | |
| 09:36:59 | 主/辅录像关闭 | | reason=0 (EndedByLocalUser) |
| | | | |
| 09:37:07 | `dial number=<dial-number>` | `ip$localhost/11359` | Cycle 2 开始 |
| 09:37:19 | `call established` | | 12s 建链 |
| 09:37:19 | 主录像开始 | | `20260423_093719_6501.mp4`(**7.7 MB**) |
| 09:37:21 | H.239 订阅成功 | | attempt #1 即订阅到 |
| 09:37:21 | 辅录像开始 | | `20260423_093721_6501_aux.mp4`(**512 KB**) |
| 09:37:35 | `clear_call` | | |
| 09:37:35 | 主/辅录像关闭 | | reason=0 |

---

## 三、主流 / 辅流记录情况对比

| 指标 | SMC 测试 Cycle 1 | SMC 测试 Cycle 2 | 服务器测试 Cycle 1 | 服务器测试 Cycle 2 |
| --- | --- | --- | --- | --- |
| 呼叫方向 | 入呼 | 入呼 | 外呼 | 外呼 |
| 会议号 | （SMC 分配） | （SMC 分配） | <dial-number> | <dial-number> |
| 建链耗时 | 6s | 6s | 13s | 12s |
| 主流录制 | ✅ 59 MB | ✅ 54 MB | ✅ 8.1 MB | ✅ 7.7 MB |
| 辅流订阅 | ✅ 1.9 MB | ✅ **12.6 MB** | ✅ 1.0 MB | ✅ 512 KB |
| 辅流订阅时机 | attempt #1 | attempt #1 | attempt #1 | attempt #1 |
| H.239 received | 是 | 是 | 是 | 是 |
| 文件正常落盘 | ✅ | ✅ | ✅ | ✅ |
| 挂断原因 | reason=3（对端） | reason=3（对端） | reason=0（本地） | reason=0（本地） |

**结论**：**四次呼叫中，主流和辅流均正常录制、正常落盘**，无文件损坏或丢帧遗留。v3 的 H.239 主动订阅策略（方案 4）在这两种方向上都成功。

注：服务器测试的 aux 文件较小（1.0 MB / 512 KB），原因是 dwell 只有 25 秒，且 MCU 只在有 presentation 推送时才会传 H.239 流。SMC 测试第 2 次 aux 达到 12.6 MB 说明当时确实有演示内容在推。

---

## 四、发现的 Bug：currentToken_ 状态泄漏（严重性：中）

### 现象

两组测试中，**每一次** `call cleared` 事件后，`status` 命令返回的字段：

- `call_token`：**保留上一次已清理的 token**
- `in_call`：**保持为 `true`**
- `recording` / `main_file` / `aux_file` 等连接级字段：能正确反映文件已关闭（因为 `currentConnection()` 返回 null 后这些字段取默认值）

这意味着：外部运维只看 `in_call` / `call_token` 会被误导以为还有通话在进行。

### 证据

- SMC Cycle 2 日志 `09:29:47` 已 `call cleared`，但 09:32:48 前 `status` 仍返回 `call_token=ip$<mcu_host>:1027/4`、`in_call=true`
- 服务器测试每轮 `clear_call` 后 `status` 都保留上一轮 token
- 甚至主动 `clear_call` 也无法重置

### 根本原因

`RecorderEndpoint::isInCall()` 实现为 `!currentToken_.IsEmpty()`，但 `currentToken_` 在 `onCallCleared` 回调中未被清空。需要在对应位置加 `currentToken_ = PString::Empty()`。

### 影响

- `status` 命令呈现误导性状态
- `dial` 会立即把 `currentToken_` 覆盖为新 token —— 所以拨号功能本身不受影响
- **不影响录制流程**：主/辅文件都能正确开关

---

## 五、H.239 主动订阅（v3 方案 4）效果验证

所有四次呼叫都显示一致的日志模式：

```
H.239 subscribe attempt 1 — sending raw OLC extendedVideoCapability session=10
raw OLC(extendedVideo, session=10) sent successfully
received openLogicalChannelAck forwardChannel=101
absorbing Ack for our raw OLC (channel 101)
CreateLogicalChannel called for OLC (session=10)
H.239 OLC received, suppressing further TCS refresh
```

**每次都在 attempt #1（即首次订阅后 60ms 内）就收到 MCU 推送的 H.239 OLC**，说明：

1. `OnH245Response` 拦截器生效：吸收了我们 raw OLC 对应的 Ack，防止 H323Plus 断言崩溃
2. `enterH243TerminalID` 回显 `mcu=0 term=1024` 正确（v2 前版本会硬编码 `term=1`）
3. 订阅重试机制未触发（`attempt 1` 即成功），回退策略无负担

---

## 六、两组测试的关键差异

| 对比维度 | SMC 侧（入呼） | 服务器侧（外呼） |
| --- | --- | --- |
| Q.931 发起方 | SMC 发 Setup | recorder 发 Setup（经 GK） |
| Connect 时长 | ≈ 6s（直接 TCP） | ≈ 12-13s（多一跳 GK 解析） |
| 挂断发起方 | SMC ReleaseComplete | 本地 `clear_call` |
| reason 码 | 3 (EndedByRemoteUser) | 0 (EndedByLocalUser) |
| 文件大小 | 较大（25-95s 通话时长） | 较小（25s dwell） |
| 主流录制 | 正常 | 正常 |
| 辅流订阅 | attempt #1 成功 | attempt #1 成功 |
| 文件落盘 | 完整 | 完整 |
| currentToken_ 残留 bug | **出现** | **出现** |

**核心结论**：两种呼叫方向下 v3 录制行为一致，均能正常记录主流+辅流。bug 也是相同的（状态泄漏）——说明问题在 `RecorderEndpoint` 而非呼叫方向特定代码。

---

## 七、关于"意外断开"的观察

本次测试未刻意注入网络故障或对端崩溃，但从日志可以观察：

- SMC 挂断（`reason=3`）和本地主动挂断（`reason=0`）两种场景下，文件关闭流程相同，未出现残留 open fd 或文件截断
- `FfmpegRecorder::closed` 在 `call cleared` 之前执行，说明资源释放顺序正确
- 如需进一步测试"真正意外断开"（如 MCU 崩溃 / 网络闪断 / TCP RST），可通过 iptables 暂时屏蔽端口 1720 或 kill 连接模拟，建议另行安排

---

## 八、后续开发建议（v3 待办）

1. ~~**[必要] 修复 `currentToken_` 清理**~~ **✅ 已修复**（见第十章）
2. **[建议] 增加呼叫历史**：`status` 只返回当前通话，可加一个 `history` 命令返回最近 N 次通话的 token/file/duration
3. **[建议] 更细粒度的断开原因**：当前日志只打 `reason=N`，可映射为人类可读字符串（`EndedByRemoteUser` 等）
4. **[建议] 注入式测试**：为"意外断开"场景编写一个脚本（tcpkill / iptables -j DROP 等），验证资源释放

---

## 十、Bug 修复记录：currentToken_ 状态泄漏

### 修复位置

[src/h323/RecorderEndpoint.cpp](../src/h323/RecorderEndpoint.cpp) 的 `OnConnectionCleared`

### 修改内容

```cpp
void RecorderEndpoint::OnConnectionCleared(
    H323Connection& conn, const PString& clearedToken)
{
    H323Connection::CallEndReason reason = conn.GetCallEndReason();
    spdlog::info("RecorderEndpoint: call cleared token={} reason={}",
                 (const char*)clearedToken, (int)reason);

    // Clear active-call state so status/isInCall reflects reality.
    // Only reset if the cleared token is still the one we track — a new
    // dial() may already have overwritten currentToken_ with a newer call.
    if (currentToken_ == clearedToken) {
        currentToken_ = PString::Empty();
    }

    // Reconnect logic for outgoing calls
    ...
}
```

**关键点**：用 `currentToken_ == clearedToken` 做匹配检查，防止"夹在中间的新 dial 已经把 token 覆盖"时被错误清空。

### 验证（2026-04-23 09:45）

重新部署 + 跑相同的 2-cycle 测试（dial <dial-number> × 2）：

| 事件 | 修复前 `status` | 修复后 `status` |
| --- | --- | --- |
| 启动后（空闲） | - | `call_token=""`, `in_call=false` ✅ |
| Cycle 1 dial 后 | `call_token=ip$localhost/11358`, `in_call=true` | `call_token=ip$localhost/22073`, `in_call=true` ✅ |
| Cycle 1 recording 中 | `recording=true`, `h239_received=true` | `recording=true`, `h239_received=true` ✅ |
| Cycle 1 `clear_call` 后 | **`call_token=ip$localhost/11358`（残留）**, **`in_call=true`** ❌ | **`call_token=""`, `in_call=false`** ✅ |
| Cycle 2 dial 后 | `call_token=ip$localhost/11359` | `call_token=ip$localhost/22074` ✅ |
| Cycle 2 `clear_call` 后 | **`call_token=ip$localhost/11359`（残留）**, **`in_call=true`** ❌ | **`call_token=""`, `in_call=false`** ✅ |

录制文件在修复前后均正常落盘（主 7.7 MB / 辅 512 KB），说明此 bug 只影响状态呈现，不影响功能。

### 部署状态

- 新二进制：`/opt/recorder/bin/recorder-core` (PID 358597)
- `version` 仍为 `3.0.0`（未 bump，作为 v3 的 bugfix 直接生效）

---

## 十一、修复后的 SMC 入呼验证（2026-04-23 09:48-09:50）

通过 SMC 发起两次完整呼叫（呼入 → 挂断 → 再次呼入 → 挂断），后台以 2s 粒度轮询 `status`，得到状态时间线：

| 时间 | 事件 | `in_call` | `call_token` | `recording` | 备注 |
| --- | --- | --- | --- | --- | --- |
| 09:48:51 | SMC Call 1 established | → true | `ip$<mcu_host>:1028/5` | 即将 true | reason 尚未产生 |
| 09:49:04 | 通话稳定 | true | `...1028/5` | true, aux=true, h239=true | meeting=Huawei, caller=<dial-number> |
| 09:49:22 | SMC 挂断 Call 1（reason=3） | true | `...1028/5` | **false** | 文件关闭瞬间（中间态） |
| **09:49:25** | `OnConnectionCleared` 执行 | **false** ✅ | **`""`** ✅ | false | **token 被清空** |
| 09:49:45 | SMC Call 2 established | true | `ip$<mcu_host>:1029/6` | false→true | 新 token |
| 09:49:50 | Call 2 H.239 订阅 | true | `...1029/6` | true, aux=true, h239=true | |
| 09:50:15 | SMC 挂断 Call 2（reason=3） | | | | |
| **09:50:17** | `OnConnectionCleared` 执行 | **false** ✅ | **`""`** ✅ | false | **token 再次清空** |

### 文件落盘

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `20260423_094851_6501.mp4` | 16.9 MB | Call 1 主流 |
| `20260423_094853_6501_aux.mp4` | 815 KB | Call 1 辅流（H.239 首次 attempt 即成功） |
| `20260423_094945_6501.mp4` | 15.9 MB | Call 2 主流 |
| `20260423_094948_6501_aux.mp4` | 825 KB | Call 2 辅流 |

### 结论

- ✅ 入呼 / 外呼两种方向都已修复 `currentToken_` 残留问题
- ✅ 对端 ReleaseComplete（reason=3）与本地 clear_call（reason=0）两种挂断路径一致进入 `OnConnectionCleared`，token 都被正确清空
- ✅ 主流/辅流均正常录制并落盘
- ✅ 中间态（recording stopped 已执行、connection cleared 未执行）持续约 3 秒，这是 Q.931 ReleaseComplete 握手的正常窗口期

### 产物

- [logs/v3_test_20260423/smc_after_fix.pcap](../logs/v3_test_20260423/smc_after_fix.pcap)（8.3 MB）
- [logs/v3_test_20260423/smc_after_fix_poll.log](../logs/v3_test_20260423/smc_after_fix_poll.log)（状态变化时间线）


---

## 九、产物索引

- 抓包：[logs/v3_test_20260423/smc_test.pcap](../logs/v3_test_20260423/smc_test.pcap)（SMC 呼入）
- 抓包：[logs/v3_test_20260423/server_test.pcap](../logs/v3_test_20260423/server_test.pcap)（服务器外呼 <dial-number>）
- 进程日志：[logs/v3_test_20260423/recorder.log](../logs/v3_test_20260423/recorder.log)
- 录像文件（服务器 `/opt/recorder/recordings/`）：
  - `20260423_092533_6501.mp4` + `_aux.mp4` （SMC Cycle 1）
  - `20260423_092807_6501.mp4` + `_aux.mp4` （SMC Cycle 2）
  - `20260423_093643_6501.mp4` + `_aux.mp4` （Server Cycle 1）
  - `20260423_093719_6501.mp4` + `_aux.mp4` （Server Cycle 2）
