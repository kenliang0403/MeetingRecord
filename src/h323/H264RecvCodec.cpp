// PTLib must be the very first include
#include <ptlib.h>

#include "H264RecvCodec.h"
#include "../media/VideoCapturePChannel.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <spdlog/spdlog.h>
#include <cstring>

// Annex B start code
static const uint8_t kStartCode[] = { 0x00, 0x00, 0x00, 0x01 };

// ─────────────────────────────────────────────────────────────────────────────
H264RecvCodec::H264RecvCodec(Direction dir)
    : H323VideoCodec(OpalMediaFormat("H.264",
                                     OpalMediaFormat::DefaultVideoSessionID,
                                     RTP_DataFrame::DynamicBase,  // H.264 uses dynamic PT
                                     FALSE,    // no jitter
                                     1920000,  // 1920 kbps
                                     0,        // variable frame size
                                     OpalMediaFormat::VideoTimeUnits,
                                     0),
                     dir)
{
    frameWidth  = 1280;   // 720p default until first frame decoded
    frameHeight = 720;
    decFrame_ = av_frame_alloc();
}

H264RecvCodec::~H264RecvCodec()
{
    Close();
    av_frame_free(&decFrame_);
}

void H264RecvCodec::Close()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (decCtx_) {
        avcodec_free_context(&decCtx_);
        decCtx_ = nullptr;
    }
    decoderReady_ = false;
    annexBuf_.clear();
    fuBuf_.clear();
    fuInProgress_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
bool H264RecvCodec::initDecoder()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (decoderReady_) return true;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        spdlog::error("H264RecvCodec: H.264 decoder not found in FFmpeg");
        return false;
    }

    decCtx_ = avcodec_alloc_context3(codec);
    if (!decCtx_) {
        spdlog::error("H264RecvCodec: avcodec_alloc_context3 failed");
        return false;
    }

    // Annex B input (raw H.264 bitstream)
    decCtx_->flags2 |= AV_CODEC_FLAG2_CHUNKS;

    if (avcodec_open2(decCtx_, codec, nullptr) < 0) {
        spdlog::error("H264RecvCodec: avcodec_open2 failed");
        avcodec_free_context(&decCtx_);
        return false;
    }

    spdlog::info("H264RecvCodec: H.264 FFmpeg decoder opened");
    decoderReady_ = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void H264RecvCodec::appendNAL(const uint8_t* data, size_t len)
{
    if (len == 0) return;
    annexBuf_.insert(annexBuf_.end(), kStartCode, kStartCode + 4);
    annexBuf_.insert(annexBuf_.end(), data, data + len);
}

// ─────────────────────────────────────────────────────────────────────────────
PBoolean H264RecvCodec::Write(const BYTE* buf, unsigned length,
                               const RTP_DataFrame& rtpFrame,
                               unsigned& written)
{
    written = length;
    if (!buf || length < 1) return TRUE;

    uint8_t nalType = buf[0] & 0x1F;

    // ── RFC 6184 de-packetization ────────────────────────────────────────
    if (nalType >= 1 && nalType <= 23) {
        // Single NAL unit packet
        appendNAL(buf, length);

    } else if (nalType == 24) {
        // STAP-A: multiple small NAL units in one packet
        // Format: [1-byte type] [2-byte size][NAL] [2-byte size][NAL] ...
        size_t offset = 1;
        while (offset + 2 <= (size_t)length) {
            uint16_t naluSize = (((uint16_t)buf[offset]) << 8) | buf[offset + 1];
            offset += 2;
            if (naluSize == 0 || offset + naluSize > (size_t)length) break;
            appendNAL(buf + offset, naluSize);
            offset += naluSize;
        }

    } else if (nalType == 28) {
        // FU-A: fragmented NAL unit
        if (length < 2) return TRUE;
        uint8_t fuHeader = buf[1];
        bool start = (fuHeader & 0x80) != 0;
        bool end   = (fuHeader & 0x40) != 0;
        // Reconstruct original NAL type: use NRI from FU indicator + type from FU header
        uint8_t origNalType = (buf[0] & 0xE0) | (fuHeader & 0x1F);

        if (start) {
            fuBuf_.clear();
            fuBuf_.push_back(origNalType);  // reconstructed NAL header
            fuInProgress_ = true;
        }

        if (fuInProgress_ && length > 2) {
            fuBuf_.insert(fuBuf_.end(), buf + 2, buf + length);
        }

        if (end && fuInProgress_) {
            appendNAL(fuBuf_.data(), fuBuf_.size());
            fuBuf_.clear();
            fuInProgress_ = false;
        }
    }
    // Other types (28=FU-B, 25-27, 29-31) — skip silently

    // RTP marker bit = last packet of this access unit
    if (!rtpFrame.GetMarker())
        return TRUE;

    if (annexBuf_.empty())
        return TRUE;

    if (!decoderReady_ && !initDecoder()) {
        annexBuf_.clear();
        return TRUE;
    }

    decodeAndDeliver();
    annexBuf_.clear();
    return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
void H264RecvCodec::decodeAndDeliver()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!decCtx_ || !decoderReady_) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    if (av_new_packet(pkt, (int)annexBuf_.size()) < 0) {
        av_packet_free(&pkt);
        return;
    }
    std::memcpy(pkt->data, annexBuf_.data(), annexBuf_.size());

    int ret = avcodec_send_packet(decCtx_, pkt);
    av_packet_free(&pkt);

    if (ret < 0) return;  // bitstream error — skip

    while (avcodec_receive_frame(decCtx_, decFrame_) == 0) {
        int w = decFrame_->width;
        int h = decFrame_->height;
        if (w <= 0 || h <= 0) continue;

        // Update codec-level dimensions (used by OpenVideoChannel)
        if (frameWidth != w || frameHeight != h) {
            frameWidth  = w;
            frameHeight = h;
            spdlog::info("H264RecvCodec: decoded frame {}x{}", w, h);
        }

        // Deliver to VideoCapturePChannel directly (Zero-Copy to FfmpegRecorder)
        PWaitAndSignal lock(rawChannelMutex);
        if (rawDataChannel) {
            auto* vchan = dynamic_cast<VideoCapturePChannel*>(rawDataChannel);
            if (vchan) {
                vchan->updateDimensions(w, h);
                vchan->WriteAVFrame(decFrame_);
            } else {
                spdlog::warn("H264RecvCodec: rawDataChannel is not VideoCapturePChannel");
            }
        }
    }
}
