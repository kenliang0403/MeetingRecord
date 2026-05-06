# 晚入会辅流接收修复方案

## 问题描述

录播器（102 服务器）在晚入会场景下无法接收已有辅流：
- **场景 A（正常）**：录播器先入会，之后有人开始演示 → MCU 推送辅流 ✅
- **场景 B（异常）**：会议中已有人在演示，录播器后呼入 → MCU **不推送**辅流 ❌

## 根因分析

通过对比三组信令抓包（TE 终端正常接收、录播器异常失败、录播器开启主流后成功）定位到两个根因：

### 1. TCS 能力声明不足

录播器原始 TCS 只有 `receive*` 纯接收能力。MCU 在晚入会路径上检查终端是否具备**双向能力**，若为纯接收端则不推送已有辅流。

TE 终端的 TCS 包含：
- `receiveAndTransmitAudioCapability`（双向音频能力）
- `receiveAndTransmitDataApplicationCapability: t120(separateLANStack)`（T.120）
- `receiveAndTransmitDataApplicationCapability: h224(hdlcFrameTunnelling)`（H.224）
- Huawei 私有 nonStandard 厂商标识（h221 28/21/555 + 38/0/8209）

### 2. 未打开返回通道（关键）

**TE 终端在入会后立即打开返回音频+返回视频+H.224 通道。** MCU 不仅检查 TCS 声明，还需要终端**实际打开返回视频 OLC** 才将其确认为"完整参会终端"。只声明能力但不打开通道，MCU 仍视为被动监听者。

当会议中有人**新开始**演示时，MCU 走另一条路径——无条件向所有在会终端推送辅流，不区分终端类型。这解释了为什么场景 A（入会后有人开演示）能正常工作。

## 修改清单

### 1. `src/h323/RecorderConnection.cpp` — `OnSendCapabilitySet()`

#### a) Audio 能力提升为 `receiveAndTransmit`

```cpp
for (PINDEX i = 0; i < origSize; ++i) {
    if (pdu.m_capabilityTable[i].m_capability.GetTag() ==
        H245_Capability::e_receiveAudioCapability) {
        PINDEX idx = pdu.m_capabilityTable.GetSize();
        pdu.m_capabilityTable.SetSize(idx + 1);
        pdu.m_capabilityTable[idx] = pdu.m_capabilityTable[i];
        pdu.m_capabilityTable[idx].m_capability.SetTag(
            H245_Capability::e_receiveAndTransmitAudioCapability);
    }
}
```

#### b) 注入 T.120 双向数据能力

```cpp
t120Cap.m_application.SetTag(1);  // 1 = t120
t120Cap.m_maxBitRate = 640;
```

#### c) 注入 Huawei 私有 nonStandard 厂商标识

```cpp
// Entry B: h221 38/0/8209 — 华为私有视频 codec 标识
// Entry C: h221 28/21/555 — 华为厂商标识
```

### 2. `src/h323/RecorderConnection.cpp` — `OnEstablished()`

#### d) 注入标准 conferenceRequests + TE 非标请求

```cpp
// terminalListRequest
// requestAllTerminalIDs
// requestChairTokenOwner
// nonStandard h221 86/1/1 (TE-style conference query)
```

### 3. `src/h323/RecorderConnection.cpp` — `OnH245Command()`

#### e) ECEC 回显（非吸收）

TE 终端对 MCU 的 ECEC 探测（h221 86/1/4608/4614/4610/4616/4612/4607/4609/4615）**回显相同数据**而非静默吸收。不回显则 MCU 判定终端不支持华为控制协议，影响辅流推送。

```cpp
if (cc == 86 && ext == 1 && (manuf == 4608 || manuf == 4614 || ...)) {
    // Echo back same h221 identifier + same data
    WriteControlPDU(echoPdu);
    return TRUE;
}
```

其余 `86/1/*` 非标命令静默吸收，不回 `functionNotUnderstood`。

### 4. `src/h323/RecorderEndpoint.cpp` — H.225 Vendor 标识

```cpp
t35CountryCode   = 38;
t35Extension     = 21;
manufacturerCode = 555;
// productId = "HUAWEI TEx0"
// versionId = "Release 19.0.920"
```

### 5. `config/config.json` — 开启主流自动发送 ⭐ 关键

```json
"auto_send_video": true,
```

这会在每次通话建立后自动调用 `startMainVideo()`，向 MCU 发送主视频返回 OLC（channel=150, session=2）。**这是触发 MCU 推送晚入会辅流的最终关键条件。**

### 6. `src/h323/RecorderConnection.cpp` — `OnCapRefreshTimer()`

将旧版的 raw OLC subscribe 重试机制替换为简单日志版，避免发送多余的 extendedVideo OLC 干扰 MCU。

## 修改总结

| 文件 | 修改内容 | 作用 |
|------|---------|------|
| `RecorderConnection.cpp` OnSendCapabilitySet | Audio 能力提升为 `receiveAndTransmit` | 声明双向音频能力 |
| `RecorderConnection.cpp` OnSendCapabilitySet | 注入 T.120 数据能力 | 声明双向数据能力 |
| `RecorderConnection.cpp` OnSendCapabilitySet | 注入 Huawei nonStandard TCS 标识 | TE 设备伪装 |
| `RecorderConnection.cpp` OnEstablished | 注入 conferenceRequests + TE 非标请求 | 主动查询会议状态 |
| `RecorderConnection.cpp` OnH245Command | ECEC 回显（替代吸收） | TE 控制协议握手 |
| `RecorderEndpoint.cpp` | t35=38/21/555 + productId | H.225 层华为伪装 |
| `config.json` | **`auto_send_video: true`** | ⭐ **打开主流返回通道** |
| `RecorderConnection.cpp` OnCapRefreshTimer | 移除 raw OLC subscribe 重试 | 避免干扰 MCU |

## 为什么需要 `auto_send_video`

TE 终端入会后的信令序列（抓包验证）：

```
帧 45:  OLC Ack (MCU audio)
帧 51:  OLC Ack (MCU video) + nonStandard
帧 59:  OLC (音频返回)            ← TE 打开返回通道！
帧 62:  OLC (视频返回 + H.224)    ← TE 打开返回通道！
帧 62:  conferenceRequests + nonStandard
...
→ MCU 看到返回通道 + 双向能力 → 确认"完整终端" → 推送已有辅流
```

录播器如果不开启 `auto_send_video`，只发 conferenceRequests 不打开返回通道，MCU 仍视为"被动监听者"，不推送辅流。

## 注意事项

1. `auto_send_video` 会创建 VideoSender 向 MCU 发送主视频（ScreenSaver 模式），不会干扰其他参会者
2. T.120 数据能力仅用于 TCS 声明，不需要实际处理 T.120 协议
3. ECEC 回显数据与 MCU 发来的完全一致（echo back），不修改内容
4. 若 MCU 尝试以华为私有 codec 打开 OLC，需在 `CreateLogicalChannel` 中拒绝
5. 此修复不影响场景 A（入会后有人开演示），该场景走 MCU 的"全量推送"路径
6. 晚入会测试需通过 SMC 呼入（MCU 主动来电），外呼到 MCU 行为可能因 MCU 实例不同而异
