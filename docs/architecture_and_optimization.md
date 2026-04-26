# Recorder-Core 代码架构与优化说明

## 1. 代码架构说明

`recorder-core` 采用了 **H.323Plus (底层协议)** + **FFmpeg (音视频编解码与封装)** 的双引擎架构。系统的核心职责是监听 H.323 呼叫、建立音视频逻辑通道、解码接收到的 RTP 媒体流，并使用 FFmpeg 将其重新编码封装为高清同步的 MP4 文件。

核心业务逻辑集中在 `src/h323/` 和 `src/media/` 目录下：

### `src/h323/`：协议与连接管理
- **`RecorderEndpoint`**：H.323 系统的入口。负责向 Gatekeeper (GK) 注册、监听 1720 端口，处理来电呼叫。它还负责配置端点能力，并向华为 MCU 注入特定的 Vendor ID 以通过厂商兼容性检查。
- **`RecorderConnection`**：代表一次具体的视频会议通话。负责在 H.245 协商阶段处理 `OpenLogicalChannel` (OLC) 请求，动态解析并创建对应的音视频逻辑接收通道。
- **`H264RecvCodec` / `H261RecvCodec`**：关键解码层。负责拦截从网络收到的 RTP 裸流（Annex B / RFC 6184 组包），利用 FFmpeg 解码器将其解码为 YUV 原始画面。

### `src/media/`：媒体桥接与录制
- **`VideoCapturePChannel` / `AudioCapturePChannel`**：音视频桥接层。它们继承自 PTLib 的虚拟设备类（“摄像头”与“声卡”），接收解码后的原始 YUV 和 PCM 数据。其中 `AudioCapturePChannel` 包含了精确的时间控制（Pacing）逻辑，防止 PTLib 空载时过度合成音频。
- **`FfmpegRecorder`**：核心的录制引擎。它持有 `libx264` (视频) 和 `aac` (音频) 编码器，负责将双路的原始数据重新编码，并严格根据全局时间基准 (`globalStartMs_`) 进行时间戳 (PTS) 对齐，最终封装写入 `.mp4` 文件。

---

## 2. 已清理的调试残留与临时代码

在排查系统崩溃、时长异常和音视频同步等棘手问题时，代码中曾加入了一些较为暴力的调试日志。这些代码现已被完全清理，以确保生产环境的性能和日志整洁：

1. **直接写文件的原始系统调用**
   - **清理位置**：`VideoCapturePChannel.cpp` 中的 `debugVCP` 函数。
   - **说明**：此前为了绕过标准库缓冲区问题，使用了底层 `open` 和 `write` 强行将日志写入 `/tmp/vcp_debug.txt`。该函数及其调用现已删除。
2. **高频的 stderr 打印**
   - **清理位置**：`VideoCapturePChannel.cpp` 和 `VideoOutputDevice.cpp` 中的 `Write`/`SetFrameData` 方法。
   - **说明**：移除了每 200 帧触发一次的 `fprintf(stderr)` 高频打印，降低了磁盘 I/O 消耗。
3. **能力表转储日志**
   - **清理位置**：`RecorderEndpoint.cpp` 的初始化方法。
   - **说明**：移除了启动时遍历并打印所有 H.323 注册能力的调试块，使得启动日志更加清爽。

---

## 3. 未来性能与架构优化建议 (Future Work)

目前系统“能跑且正确”，但在应对高并发录制（如同时录制多个 1080P 会议）时，还可以考虑以下几个优化方向：

### 3.1 减少内存拷贝 (Zero-Copy 优化)
- **现状**：在 `H264RecvCodec` 中，FFmpeg 解码出画面后，经过了 `sws_scale` 缩放，然后通过手动 `memcpy` 将 Y/U/V 三个平面拼凑成一块连续内存，最后发给桥接层。
- **优化**：可以考虑重构接口，让 `FfmpegRecorder` 直接接收非连续的 `AVFrame` 结构，省去中间巨大的 YUV 内存申请和拼接过程，极大降低 CPU 缓存未命中率。

### 3.2 音视频锁的粒度分离与异步编码
- **现状**：`FfmpegRecorder` 中，`writeVideoFrame` 和 `writeAudioSamples` 共用了一把全局互斥锁 `mu_`。
- **优化**：H.264 编码 (libx264) 非常耗时。当视频线程持锁编码时，音频线程会被阻塞，这可能导致底层音频队列堆积。建议将锁拆分为 `videoMutex_` 和 `audioMutex_`，或者引入无锁队列（Lock-free Queue）将音视频的耗时编码工作完全抛给后台独立线程执行。

### 3.3 内存池化 (Memory Pooling)
- **现状**：在录像时，我们每一帧都在频繁调用 `av_frame_alloc()` 和 `av_packet_alloc()`，并在写入后立刻 `free`。
- **优化**：实现一个固定大小的 `AVFrame` / `AVPacket` 对象池，循环利用。这能彻底避免堆内存碎片的产生，对 C++ 长期运行的服务器程序非常有益。

### 3.4 恢复安全的 Flush 机制
- **现状**：在 `FfmpegRecorder::close()` 中，我们注释掉了向编码器发送 `nullptr` 来刷新尾部缓存帧的代码，以避免挂断时的段错误（Segfault）。
- **优化**：虽然目前禁用了 B 帧，少录最后几帧画面无伤大雅。但最完美的做法是彻底理清 H.323 连接销毁线程与 FFmpeg 录制线程的生命周期先后顺序，确保在底层网络完全断开、PChannel 安全释放**前**，优雅地执行 Flush 操作。
