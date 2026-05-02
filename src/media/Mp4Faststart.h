#pragma once
#include <string>

/**
 * 把一个 fragmented MP4 文件就地重写成普通 (non-fragmented) MP4 +
 * `movflags=+faststart`，moov atom 在文件头，浏览器 seek 即时响应。
 *
 * 流程：
 *   1. 打开 input
 *   2. 创建 output 上下文 → 写到 path + ".faststart.tmp"
 *   3. avcodec_parameters_copy 不重新编码 (-c copy 等价)
 *   4. av_write_header 时设 movflags=+faststart
 *   5. 全部 packet 拷贝
 *   6. write trailer 时 ffmpeg 内部把 moov 移到 mdat 前
 *   7. atomic rename tmp → path 替换原文件
 *
 * 失败时清理 tmp，原文件保持不变（fmp4 仍可播，只是 seek 慢）。
 *
 * 是否真正"原地"取决于 std::filesystem::rename 在同一文件系统上的语义。
 * 我们的 recordings 目录都在同一卷下，所以是 atomic 的。
 *
 * 线程安全：可以在 detached thread 调用。函数本身不持锁。
 */
bool faststartRewrite(const std::string& path);
