# Recorder-Core 架构设计与实现说明：应用层兜底接管 H.264 视频信令与媒体

## 1. 背景与挑战

在开发基于 H323Plus 协议栈的录制服务（`recorder-core`）时，我们在与主流 MCU（如华为 VP9660）对接 H.264 视频时遇到了严重障碍：
1. **TCS (Terminal Capability Set) 协商问题**：MCU 拒绝开启视频逻辑信道（OLC），因为传统的泛型（Generic）能力宣告方式缺少了关键的 `collapsing parameters`（Profile, Level 等），导致 MCU 无法判断本端能力。
2. **底层库插件缺失与 OLC 匹配失败**：即使修复了 TCS 参数，由于目标服务器环境的 H323Plus 库中未编译或未安装 H.264 专用的媒体插件（Plugin），导致在接收到 MCU 发来的 H.264 `videoData` 及包含特定 `mediaPacketization`（如 H.241 Annex A, RFC 3984）的入站 OLC 时，底层库无法将其匹配到本地的 Generic 能力，直接抛出 `unknown data type` 并拒绝建链。

如果采用传统的“编译安装 H.264 插件”方案，不仅环境依赖重（需要重新编译庞大的底层库并部署 `.so` 插件），而且会强制走完整的“RTP 解包 -> 解码为 YUV -> (应用层) YUV 重编码 -> 存 MP4”流程，这对于纯录制业务来说，带来了极大的 CPU 开销与画质损耗。

## 2. 方案思路：信令与媒体解耦的“应用层兜底接管”

针对上述痛点，我们提出并实现了一种**“把 H323Plus 当作纯信令透传层，将音视频能力协商与 RTP 处理全部上浮到应用层”**的轻量级兜底架构。

### 核心设计原则
1. **轻量能力宣告**：在应用层实现最小化的 `H323GenericVideoCapability`，手工按 H.241 规范拼装 ASN.1 语法树（填入 OID 和 Profile/Level），欺骗 MCU 相信本端是一个合法的 H.264 终端。
2. **拦截与兜底 OLC**：重写连接层（Connection）的入站 OLC 处理函数。当底层库因为找不到匹配的插件而报错 `unknown data type` 时，拦截该错误，并在应用层手工解析 H.245 PDU 树。如果确认是 H.264，则强行绕过底层库，直接创建 RTP 接收信道。
3. **原生 RTP 录制（避免重编解码）**：在应用层直接接收 RTP 包，按照 RFC 6184 规范进行去包化（Depacketization）还原出 NALU 裸流，然后无损透传给 FFmpeg 封装成 MP4。

## 3. 具体实现方式

### 3.1 宣告合规的 H.264 能力 (TCS 阶段)
在 `H264RecvCodec.h` 中，自定义 `H323_H264RecvCap` 继承自 `H323GenericVideoCapability`。重写 `OnSendingPDU` 方法，严格按照 H.241 规范填充 `collapsing parameters`：

```cpp
// 关键实现点：H264RecvCodec.h -> H323_H264RecvCap::OnSendingPDU
gen.m_capabilityIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
PASN_ObjectId &oid = gen.m_capabilityIdentifier;
oid.SetValue("0.0.8.241.0.0.1"); // H.264 标准 OID

// 填充 Profile (Baseline + Main)
gen.m_collapsing.SetSize(gen.m_collapsing.GetSize() + 1);
// ... 设置 standard = 41 (Profile), booleanArray = 0x60

// 填充 Level (4.0)
gen.m_collapsing.SetSize(gen.m_collapsing.GetSize() + 1);
// ... 设置 standard = 42 (Level), unsignedMin = 85 (Level 4.0 的 H.241 编码)
```

### 3.2 拦截并兜底创建视频逻辑信道 (OLC 阶段)
在 `RecorderConnection.cpp` 中，重写 `OnReceivedOpenLogicalChannel`。当底层调用 `H323Connection::OnReceivedOpenLogicalChannel` 失败（即找不到插件）时，启动兜底逻辑：

```cpp
// 关键实现点：RecorderConnection.cpp -> RecorderConnection::OnReceivedOpenLogicalChannel
BOOL ok = H323Connection::OnReceivedOpenLogicalChannel(olc, pdu);
if (!ok) {
    // 底层库匹配失败，检查是否是 H.264 (包含特定的 mediaPacketization)
    if (dataType.GetTag() == H245_DataType::e_videoData) {
        const H245_VideoCapability &videoCap = dataType;
        if (videoCap.GetTag() == H245_VideoCapability::e_genericVideoCapability) {
            const H245_GenericCapability &gen = videoCap;
            // 提取 OID
            PString oidStr = gen.m_capabilityIdentifier.GetString();
            if (oidStr == "0.0.8.241.0.0.1") {
                // 确认是 H.264，手工创建 H323_H264RecvCap 并放行
                PTRACE(2, "RecorderConnection\tfallback-create H.264 RTP channel");
                H323_H264RecvCap* cap = new H323_H264RecvCap();
                return CreateRealTimeLogicalChannel(olc, cap,
                                                    H323Channel::IsReceiver,
                                                    sessionID,
                                                    param,
                                                    pdu);
            }
        }
    }
}
return ok;
```

### 3.3 应用层 RTP 去包化与落盘
底层库负责接收 UDP 数据并剥离 IP/UDP 头，将纯 RTP 包通过 `H264RecvCodec::Write` 回调给应用层。
在应用层，我们根据 RFC 6184 规范，将 Single NAL、STAP-A、FU-A 等类型的 RTP Payload 还原成带有 `00 00 00 01` Start Code 的 Annex B 格式裸流，最终送入 `FfmpegRecorder` 直接封装为 MP4。

## 4. 架构优势与未来扩展

1. **部署极简**：不再依赖庞大的 `plugin.so` 生态和环境变量配置，单个二进制文件即可随处运行。
2. **性能极致**：完全避免了“解码出 YUV 再重新编码”的巨额 CPU 消耗，录制过程仅涉及网络 IO 和文件封装。
3. **扩展 H.239 双流**：只需在兜底逻辑中识别不同的 `sessionID`（或 `H239ExtendedVideoCapability` 标识），即可轻松将辅流路由到第二个 MP4 文件。
4. **支持 H.265 (HEVC) 无障碍**：底层库即使再老、完全不懂 H.265 也没关系。只需在应用层仿写一个 `H323_H265RecvCap`，匹配 OID `0.0.8.241.0.0.2`，并加入 RFC 7798 的去包化逻辑，即可立刻实现 H.265 的直接录制。
