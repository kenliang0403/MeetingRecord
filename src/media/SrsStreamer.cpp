#include "SrsStreamer.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

using clk = std::chrono::steady_clock;

static int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clk::now().time_since_epoch()).count();
}

SrsStreamer::SrsStreamer(const Config& cfg)
    : cfg_(cfg)
{
}

SrsStreamer::~SrsStreamer()
{
    stop();
    if (vPar_) { avcodec_parameters_free(&vPar_); }
    if (aPar_) { avcodec_parameters_free(&aPar_); }
}

void SrsStreamer::setVideoParams(const AVCodecParameters* codecpar,
                                 AVRational encoderTimeBase)
{
    if (vPar_) { avcodec_parameters_free(&vPar_); }
    vPar_ = avcodec_parameters_alloc();
    avcodec_parameters_copy(vPar_, codecpar);
    vTbNum_ = encoderTimeBase.num;
    vTbDen_ = encoderTimeBase.den;
}

void SrsStreamer::setAudioParams(const AVCodecParameters* codecpar,
                                 AVRational encoderTimeBase)
{
    if (aPar_) { avcodec_parameters_free(&aPar_); }
    aPar_ = avcodec_parameters_alloc();
    avcodec_parameters_copy(aPar_, codecpar);
    aTbNum_ = encoderTimeBase.num;
    aTbDen_ = encoderTimeBase.den;
    hasAudio_ = true;
}

void SrsStreamer::start()
{
    if (running_.exchange(true)) return;
    stopReq_ = false;
    th_ = std::thread([this]{ threadMain(); });
}

void SrsStreamer::stop()
{
    if (!running_.exchange(false)) return;
    stopReq_ = true;
    qCv_.notify_all();
    if (th_.joinable()) th_.join();

    // Flush leftover queue packets
    std::lock_guard<std::mutex> lk(qMu_);
    while (!queue_.empty()) {
        av_packet_free(&queue_.front().pkt);
        queue_.pop_front();
    }
}

void SrsStreamer::pushVideo(const AVPacket* pkt)
{
    if (!running_.load() || !pkt || !vPar_) return;

    AVPacket* clone = av_packet_clone(pkt);
    if (!clone) return;

    bool isKey = (clone->flags & AV_PKT_FLAG_KEY) != 0;

    {
        std::lock_guard<std::mutex> lk(qMu_);
        // 积压管理：队列超过 2×max 时强制断链重连（认为上游堵死）
        if (static_cast<int>(queue_.size()) > cfg_.queue_max * 2) {
            spdlog::warn("SrsStreamer[{}]: queue overflow ({}), dropping until keyframe",
                         cfg_.rtmp_url, queue_.size());
            // 丢到下一个关键帧
            while (!queue_.empty() && !queue_.front().isKey) {
                av_packet_free(&queue_.front().pkt);
                queue_.pop_front();
            }
        }
        // 积压超过 max 时，丢弃非关键视频帧
        if (static_cast<int>(queue_.size()) > cfg_.queue_max && !isKey) {
            av_packet_free(&clone);
            return;
        }
        queue_.push_back({clone, true, isKey});
    }
    qCv_.notify_one();
}

void SrsStreamer::pushAudio(const AVPacket* pkt)
{
    if (!running_.load() || !pkt || !aPar_) return;

    AVPacket* clone = av_packet_clone(pkt);
    if (!clone) return;

    {
        std::lock_guard<std::mutex> lk(qMu_);
        // 音频积压时直接丢，保证视频流畅
        if (static_cast<int>(queue_.size()) > cfg_.queue_max) {
            av_packet_free(&clone);
            return;
        }
        queue_.push_back({clone, false, false});
    }
    qCv_.notify_one();
}

bool SrsStreamer::openRtmp()
{
    spdlog::info("SrsStreamer: connecting to {}", cfg_.rtmp_url);

    int ret = avformat_alloc_output_context2(&fmt_, nullptr, "flv",
                                             cfg_.rtmp_url.c_str());
    if (ret < 0 || !fmt_) {
        spdlog::error("SrsStreamer: alloc flv context failed: {}", ret);
        return false;
    }

    // Video stream
    vStream_ = avformat_new_stream(fmt_, nullptr);
    if (!vStream_) { closeRtmp(); return false; }
    avcodec_parameters_copy(vStream_->codecpar, vPar_);
    vStream_->codecpar->codec_tag = 0;
    vStream_->time_base = {1, 1000};    // FLV: millisecond

    // Audio stream
    if (hasAudio_ && aPar_) {
        aStream_ = avformat_new_stream(fmt_, nullptr);
        if (!aStream_) { closeRtmp(); return false; }
        avcodec_parameters_copy(aStream_->codecpar, aPar_);
        aStream_->codecpar->codec_tag = 0;
        aStream_->time_base = {1, 1000};
    }

    // Open RTMP
    ret = avio_open2(&fmt_->pb, cfg_.rtmp_url.c_str(), AVIO_FLAG_WRITE,
                     nullptr, nullptr);
    if (ret < 0) {
        char buf[256] = {0};
        av_strerror(ret, buf, sizeof(buf));
        spdlog::error("SrsStreamer: avio_open2 '{}' failed: {} ({})",
                      cfg_.rtmp_url, ret, buf);
        closeRtmp();
        return false;
    }

    // Write FLV header
    ret = avformat_write_header(fmt_, nullptr);
    if (ret < 0) {
        char buf[256] = {0};
        av_strerror(ret, buf, sizeof(buf));
        spdlog::error("SrsStreamer: write_header '{}' failed: {} ({})",
                      cfg_.rtmp_url, ret, buf);
        closeRtmp();
        return false;
    }

    headerDone_ = true;
    connected_ = true;
    epochMs_   = -1;
    spdlog::info("SrsStreamer: connected, header written [{}]", cfg_.rtmp_url);
    return true;
}

void SrsStreamer::closeRtmp()
{
    connected_ = false;
    if (fmt_) {
        if (headerDone_) {
            av_write_trailer(fmt_);
            headerDone_ = false;
        }
        if (fmt_->pb) {
            avio_closep(&fmt_->pb);
        }
        avformat_free_context(fmt_);
        fmt_ = nullptr;
    }
    vStream_ = nullptr;
    aStream_ = nullptr;
}

void SrsStreamer::dropToKeyframe()
{
    std::lock_guard<std::mutex> lk(qMu_);
    while (!queue_.empty() && !queue_.front().isKey) {
        av_packet_free(&queue_.front().pkt);
        queue_.pop_front();
    }
}

void SrsStreamer::threadMain()
{
    spdlog::info("SrsStreamer: thread start [{}]", cfg_.rtmp_url);

    while (!stopReq_.load()) {
        if (!vPar_) {
            // 还没设置视频参数（编码器尚未初始化）— 等
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait_for(lk, std::chrono::milliseconds(200),
                          [this]{ return stopReq_.load() || vPar_ != nullptr; });
            continue;
        }

        if (!openRtmp()) {
            // 连接失败，清空积压非关键帧（避免堆积爆内存）
            dropToKeyframe();
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait_for(lk,
                std::chrono::milliseconds(cfg_.reconnect_delay_ms),
                [this]{ return stopReq_.load(); });
            continue;
        }

        // 连接已建立。之后只有 write_frame 失败才触发重连。
        // 即使长时间没数据（例如辅流没人共享）也保持连接，避免无谓重连抖动。
        bool keyframeSeen = false;
        while (!stopReq_.load() && connected_.load()) {
            QueuedPacket qp{nullptr, false, false};
            {
                std::unique_lock<std::mutex> lk(qMu_);
                qCv_.wait_for(lk, std::chrono::milliseconds(500),
                              [this]{ return stopReq_.load() || !queue_.empty(); });
                if (stopReq_.load()) break;
                if (queue_.empty()) continue;  // just wait, no timeout
                qp = queue_.front();
                queue_.pop_front();
            }

            // 首关键帧之前：丢弃所有非关键包
            if (!keyframeSeen) {
                if (qp.isVideo && qp.isKey) {
                    keyframeSeen = true;
                } else {
                    av_packet_free(&qp.pkt);
                    continue;
                }
            }

            // epoch baseline = 首包 pts (rescale to ms)
            AVRational encTb = qp.isVideo ? AVRational{vTbNum_, vTbDen_}
                                          : AVRational{aTbNum_, aTbDen_};
            int64_t ptsMs = av_rescale_q(qp.pkt->pts, encTb, AVRational{1, 1000});
            int64_t dtsMs = av_rescale_q(qp.pkt->dts != AV_NOPTS_VALUE ? qp.pkt->dts
                                                                       : qp.pkt->pts,
                                         encTb, AVRational{1, 1000});
            if (epochMs_ < 0) epochMs_ = ptsMs;
            qp.pkt->pts = ptsMs - epochMs_;
            qp.pkt->dts = dtsMs - epochMs_;
            if (qp.pkt->dts > qp.pkt->pts) qp.pkt->dts = qp.pkt->pts;
            qp.pkt->duration = av_rescale_q(qp.pkt->duration, encTb,
                                            AVRational{1, 1000});
            qp.pkt->pos = -1;
            qp.pkt->stream_index = qp.isVideo ? vStream_->index
                                              : aStream_->index;

            int ret = av_interleaved_write_frame(fmt_, qp.pkt);
            av_packet_free(&qp.pkt);
            if (ret < 0) {
                char buf[256] = {0};
                av_strerror(ret, buf, sizeof(buf));
                spdlog::warn("SrsStreamer: write_frame failed: {} ({}), reconnecting",
                             ret, buf);
                break;  // 触发重连
            }
        }

        closeRtmp();

        if (!stopReq_.load()) {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait_for(lk,
                std::chrono::milliseconds(cfg_.reconnect_delay_ms),
                [this]{ return stopReq_.load(); });
        }
    }

    closeRtmp();
    spdlog::info("SrsStreamer: thread exit [{}]", cfg_.rtmp_url);
}
