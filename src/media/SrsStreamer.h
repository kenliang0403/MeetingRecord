#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Keep FFmpeg out of this header to avoid polluting PTLib TU
struct AVFormatContext;
struct AVStream;
struct AVPacket;
struct AVCodecParameters;
struct AVRational;

/**
 * SrsStreamer — 异步 RTMP 推流器。
 *
 * 设计：
 *  - FfmpegRecorder 编码得到的 H.264 / AAC AVPacket，clone 一份通过
 *    push* 接口非阻塞入队，立即返回。
 *  - 内部线程负责建立 FLV/RTMP 连接、写 header、drain 队列、处理断链重连。
 *  - MP4 路径完全不受网络抖动影响。
 *  - 队列超过 queue_max 时丢弃非关键帧；超过 2×queue_max 时强制断链重连。
 *  - 只负责 push；拉流格式（HLS/HTTP-FLV/RTMP）由 SRS 自动提供。
 *
 * 使用步骤：
 *    SrsStreamer::Config c;
 *    c.rtmp_url = "rtmp://127.0.0.1/live/recorder-<dial-number>-main";
 *    auto s = std::make_shared<SrsStreamer>(c);
 *    s->setVideoParams(videoCodecpar, vEncTimeBase);   // 编码器初始化后
 *    s->setAudioParams(audioCodecpar, aEncTimeBase);   // 可选
 *    s->start();
 *    // 编码循环里：
 *    s->pushVideo(pkt);   // pkt->time_base 应为 vEncTimeBase
 *    s->pushAudio(pkt);
 *    // 关闭时：
 *    s->stop();
 */
class SrsStreamer {
public:
    struct Config {
        std::string rtmp_url;               // rtmp://host[:port]/app/stream
        int         queue_max           = 200;
        int         reconnect_delay_ms  = 2000;
    };

    explicit SrsStreamer(const Config& cfg);
    ~SrsStreamer();

    // 必须在 start() 之前设置视频参数。音频可选。
    // codecpar 会被复制一份，调用方可立即释放/复用。
    void setVideoParams(const AVCodecParameters* codecpar,
                        AVRational encoderTimeBase);
    void setAudioParams(const AVCodecParameters* codecpar,
                        AVRational encoderTimeBase);

    void start();
    void stop();

    // 非阻塞：入队 + 唤醒线程，立即返回。pkt 的 pts/dts 应在编码器
    // time_base 下。内部会 rescale 到 flv 的 ms 时基。
    void pushVideo(const AVPacket* pkt);
    void pushAudio(const AVPacket* pkt);

    bool isRunning() const { return running_.load(); }
    // 是否已经把 header 成功写到 SRS。只有进入 connected 状态后
    // 推的包才会真正上行；之前的都排队等连接。
    bool isConnected() const { return connected_.load(); }

private:
    struct QueuedPacket {
        AVPacket* pkt;
        bool      isVideo;
        bool      isKey;
    };

    void threadMain();
    bool openRtmp();
    void closeRtmp();
    bool writeHeader();
    bool drainQueue();
    void dropToKeyframe();   // 积压过多时丢弃到下一个关键帧

    Config                  cfg_;

    std::thread             th_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       connected_{false};
    std::atomic<bool>       stopReq_{false};

    std::mutex                   qMu_;
    std::condition_variable      qCv_;
    std::deque<QueuedPacket>     queue_;

    // FFmpeg context (only touched by thread)
    AVFormatContext*        fmt_       = nullptr;
    AVStream*               vStream_   = nullptr;
    AVStream*               aStream_   = nullptr;
    bool                    headerDone_ = false;
    int64_t                 epochMs_    = -1;   // first packet wall-ms baseline

    // Cached codecpar + timebase, set before start()
    AVCodecParameters*      vPar_ = nullptr;
    AVCodecParameters*      aPar_ = nullptr;
    // time_base of the encoder that produced the packets (for rescale)
    int                     vTbNum_ = 0, vTbDen_ = 0;
    int                     aTbNum_ = 0, aTbDen_ = 0;
    bool                    hasAudio_ = false;
};
