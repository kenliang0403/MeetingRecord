// PTLib must be the very first include
#include <ptlib.h>

#include "H261RecvCodec.h"
#include "../media/VideoCapturePChannel.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <spdlog/spdlog.h>
#include <cstring>
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────────
H261RecvCodec::H261RecvCodec(Direction dir)
    : H323VideoCodec(OpalMediaFormat("H.261",
                                     OpalMediaFormat::DefaultVideoSessionID,
                                     RTP_DataFrame::H261,
                                     FALSE,   // no jitter
                                     352000,  // 352 kbps
                                     0,       // variable frame size
                                     OpalMediaFormat::VideoTimeUnits,
                                     0),
                     dir)
{
    frameWidth  = 352;   // H.261 CIF default
    frameHeight = 288;
    decFrame_ = av_frame_alloc();
}

H261RecvCodec::~H261RecvCodec()
{
    Close();
    av_frame_free(&decFrame_);
}

void H261RecvCodec::Close()
{
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (decCtx_) {
        avcodec_free_context(&decCtx_);
        decCtx_ = nullptr;
    }
    decoderReady_ = false;
    frameBuf_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
bool H261RecvCodec::initDecoder()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H261);
    if (!codec) {
        spdlog::error("H261RecvCodec: H.261 decoder not found in FFmpeg");
        return false;
    }

    decCtx_ = avcodec_alloc_context3(codec);
    if (!decCtx_) {
        spdlog::error("H261RecvCodec: avcodec_alloc_context3 failed");
        return false;
    }

    // Let FFmpeg auto-detect dimensions from the bitstream
    decCtx_->width  = 0;
    decCtx_->height = 0;

    if (avcodec_open2(decCtx_, codec, nullptr) < 0) {
        spdlog::error("H261RecvCodec: avcodec_open2 failed");
        avcodec_free_context(&decCtx_);
        return false;
    }

    spdlog::info("H261RecvCodec: H.261 FFmpeg decoder opened");
    decoderReady_ = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
PBoolean H261RecvCodec::Write(const BYTE* buf, unsigned length,
                               const RTP_DataFrame& rtpFrame,
                               unsigned& written)
{
    written = length;

    static std::atomic<int> writeCount{0};
    int wc = ++writeCount;
    // Bypass spdlog entirely — write directly to stderr so run.log captures it
    fprintf(stderr, "[H261RecvCodec::Write] #%d len=%u marker=%d\n",
            wc, length, (int)rtpFrame.GetMarker());
    fflush(stderr);
    if (wc <= 5 || wc % 100 == 0)
        spdlog::warn("H261RecvCodec::Write called #{} len={}", wc, length);

    // Minimum: 4-byte H.261 RTP header (RFC 4587) + at least 1 byte of data
    if (!buf || length < 5) return TRUE;

    // ── RFC 4587 H.261 RTP header (4 bytes) ─────────────────────────────
    // Byte 0: SBIT[7:5] | EBIT[4:2] | I[1] | V[0]
    // Byte 1: GOBN[7:4] | MBAP[3:0] (high)
    // Byte 2: MBAP[7] | QUANT[6:2] | HMVD[1:0] (high)
    // Byte 3: HMVD[7:5] | VMVD[4:0]
    //
    // SBIT: bits to skip at START of first payload byte after header
    // EBIT: bits to skip at END  of last  payload byte
    //
    // Most implementations send byte-aligned data (SBIT=EBIT=0).
    // We handle that common case; edge cases degrade gracefully.

    uint8_t sbit = (buf[0] >> 5) & 0x07;

    const uint8_t* data = buf + 4;
    size_t         len  = length - 4;

    if (sbit == 0) {
        // Fast path: byte-aligned payload, just append
        frameBuf_.insert(frameBuf_.end(), data, data + len);
    } else {
        // Bit-shift: combine trailing bits of previous last byte with
        // shifted current payload.  This ensures a continuous bitstream.
        if (!frameBuf_.empty()) {
            // OR the high SBIT bits of data[0] into the last accumulated byte
            frameBuf_.back() |= (data[0] >> (8 - sbit));
        }
        // Shift each byte left by sbit, borrowing from the next byte
        for (size_t i = 0; i < len - 1; ++i)
            frameBuf_.push_back((uint8_t)((data[i] << sbit) | (data[i+1] >> (8 - sbit))));
        frameBuf_.push_back((uint8_t)(data[len - 1] << sbit));
    }

    // RTP marker bit = last packet of this video picture
    if (!rtpFrame.GetMarker())
        return TRUE;

    if (frameBuf_.empty())
        return TRUE;

    if (!decoderReady_ && !initDecoder()) {
        frameBuf_.clear();
        return TRUE;
    }

    spdlog::debug("H261RecvCodec: decoding frame buf={}B", frameBuf_.size());
    decodeAndDeliver(rtpFrame.GetTimestamp());
    frameBuf_.clear();
    return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
void H261RecvCodec::decodeAndDeliver(int64_t /*rtpTimestamp*/)
{
    // Feed the assembled H.261 picture to FFmpeg
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    if (av_new_packet(pkt, (int)frameBuf_.size()) < 0) {
        av_packet_free(&pkt);
        return;
    }
    std::memcpy(pkt->data, frameBuf_.data(), frameBuf_.size());

    int ret = avcodec_send_packet(decCtx_, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        // Bitstream error — skip this frame silently
        return;
    }

    int receiveRet;
    while ((receiveRet = avcodec_receive_frame(decCtx_, decFrame_)) == 0) {
        int w = decFrame_->width;
        int h = decFrame_->height;
        spdlog::warn("H261RecvCodec: decoded frame {}x{}", w, h);
        if (w <= 0 || h <= 0) continue;

        // Update codec-level dimensions so the endpoint knows actual size
        if (frameWidth != w || frameHeight != h) {
            frameWidth  = w;
            frameHeight = h;
            spdlog::info("H261RecvCodec: decoded frame {}x{}", w, h);
        }

        // Deliver to attached channel (VideoCapturePChannel) directly
        PWaitAndSignal lock(rawChannelMutex);
        if (rawDataChannel) {
            auto* vchan = dynamic_cast<VideoCapturePChannel*>(rawDataChannel);
            if (vchan) {
                vchan->updateDimensions(w, h);
                vchan->WriteAVFrame(decFrame_);
            } else {
                spdlog::warn("H261RecvCodec: rawDataChannel is not VideoCapturePChannel");
            }
        } else {
            spdlog::warn("H261RecvCodec: rawDataChannel is NULL, dropping frame {}x{}", w, h);
        }
    }
    if (receiveRet != AVERROR(EAGAIN) && receiveRet != AVERROR_EOF) {
        char errbuf[64];
        av_strerror(receiveRet, errbuf, sizeof(errbuf));
        spdlog::warn("H261RecvCodec: avcodec_receive_frame: {}", errbuf);
    }
}
