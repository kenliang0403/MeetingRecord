#include "VideoSender.h"
#include "../util/PixelFont.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

using clk = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────────

VideoSender::VideoSender()
    : cfg_{}
{
}

VideoSender::VideoSender(const Config& cfg)
    : cfg_(cfg)
{
}

VideoSender::~VideoSender()
{
    stop();
}

bool VideoSender::initNetwork()
{
    sockFd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockFd_ < 0) {
        spdlog::error("VideoSender: socket() failed: {}", strerror(errno));
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;   // let OS assign an ephemeral port

    if (bind(sockFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("VideoSender: bind() failed: {}", strerror(errno));
        ::close(sockFd_);
        sockFd_ = -1;
        return false;
    }

    socklen_t len = sizeof(addr);
    getsockname(sockFd_, reinterpret_cast<sockaddr*>(&addr), &len);
    localPort_ = static_cast<int>(ntohs(addr.sin_port));

    spdlog::info("VideoSender: network init OK, local UDP port={}", localPort_);
    return true;
}

bool VideoSender::startSending(const std::string& destIP, int destPort, int payloadType)
{
    if (sockFd_ < 0) {
        spdlog::error("VideoSender: startSending() called before initNetwork()");
        return false;
    }
    if (running_.load()) {
        spdlog::warn("VideoSender: already running");
        return true;
    }

    destIP_   = destIP;
    destPort_ = destPort;
    payloadType_ = payloadType;
    stopReq_  = false;
    running_  = true;
    th_       = std::thread([this] { threadMain(); });

    spdlog::info("VideoSender: startSending → {}:{}", destIP_, destPort_);
    return true;
}

void VideoSender::stop()
{
    if (!running_.exchange(false)) return;
    stopReq_ = true;
    if (th_.joinable()) th_.join();
    cleanup();
}

// ─── Internal ────────────────────────────────────────────────────────────────

bool VideoSender::initEncoder()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        spdlog::error("VideoSender: libx264 encoder not found");
        return false;
    }

    encCtx_ = avcodec_alloc_context3(codec);
    if (!encCtx_) { return false; }

    encCtx_->width      = cfg_.width;
    encCtx_->height     = cfg_.height;
    encCtx_->time_base  = { 1, cfg_.fps };
    encCtx_->framerate  = { cfg_.fps, 1 };
    encCtx_->pix_fmt    = AV_PIX_FMT_YUV420P;
    encCtx_->bit_rate   = cfg_.bitrate;
    encCtx_->gop_size   = cfg_.fps * 2;   // keyframe every 2 s
    encCtx_->max_b_frames = 0;

    av_opt_set(encCtx_->priv_data, "preset",  "ultrafast", 0);
    av_opt_set(encCtx_->priv_data, "tune",    "zerolatency", 0);
    av_opt_set(encCtx_->priv_data, "profile", "high", 0);
    av_opt_set(encCtx_->priv_data, "x264opts", "slice-max-size=1100", 0);
    av_opt_set(encCtx_->priv_data, "x264-params", "slice-max-size=1100", 0);
    // repeat_headers=1: prepend SPS+PPS before every IDR frame (in-band).
    // Do NOT set AV_CODEC_FLAG_GLOBAL_HEADER — that would move SPS/PPS to
    // extradata and strip them from the bitstream, breaking RTP streaming.
    av_opt_set_int(encCtx_->priv_data, "repeat_headers", 1, 0);

    int ret = avcodec_open2(encCtx_, codec, nullptr);
    if (ret < 0) {
        char buf[128];
        av_strerror(ret, buf, sizeof(buf));
        spdlog::error("VideoSender: avcodec_open2 failed: {}", buf);
        avcodec_free_context(&encCtx_);
        return false;
    }

    rawFrame_ = av_frame_alloc();
    rawFrame_->format = AV_PIX_FMT_YUV420P;
    rawFrame_->width  = cfg_.width;
    rawFrame_->height = cfg_.height;
    if (av_frame_get_buffer(rawFrame_, 32) < 0) {
        spdlog::error("VideoSender: av_frame_get_buffer failed");
        av_frame_free(&rawFrame_);
        avcodec_free_context(&encCtx_);
        return false;
    }

    encPkt_ = av_packet_alloc();
    spdlog::info("VideoSender: H.264 encoder ready {}x{}@{}fps {}kbps",
                 cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate / 1000);
    return true;
}

void VideoSender::cleanup()
{
    if (encPkt_)   { av_packet_free(&encPkt_); }
    if (rawFrame_) { av_frame_free(&rawFrame_); }
    if (encCtx_)   { avcodec_free_context(&encCtx_); }
    if (sockFd_ >= 0) { ::close(sockFd_); sockFd_ = -1; }
}

void VideoSender::fillFrame(int frameNum)
{
    if (cfg_.mode == Mode::ScreenSaver)
        fillScreenSaver(frameNum);
    else if (cfg_.mode == Mode::MainStream)
        fillMainStream(frameNum);
    else if (cfg_.mode == Mode::AuxStream)
        fillAuxStream(frameNum);
    else
        fillNoSignal(frameNum);
}

// ── Mode::ScreenSaver ─────────────────────────────────────────────────────────
// Cycling full-frame colour bars (one colour per second).
void VideoSender::fillScreenSaver(int frameNum)
{
    static const struct { uint8_t y, u, v; } kBars[8] = {
        {235, 128, 128},  // white
        {210,  16, 146},  // yellow
        {170, 166,  16},  // cyan
        {145,  54,  34},  // green
        {107, 202, 222},  // magenta
        { 82,  90, 240},  // red
        { 41, 240, 110},  // blue
        { 16, 128, 128},  // black
    };
    int barIdx = (frameNum / std::max(1, cfg_.fps)) % 8;
    uint8_t yv = kBars[barIdx].y;
    uint8_t uv = kBars[barIdx].u;
    uint8_t vv = kBars[barIdx].v;

    for (int row = 0; row < cfg_.height; ++row)
        std::memset(rawFrame_->data[0] + row * rawFrame_->linesize[0],
                    yv, static_cast<size_t>(cfg_.width));
    for (int row = 0; row < cfg_.height / 2; ++row) {
        std::memset(rawFrame_->data[1] + row * rawFrame_->linesize[1],
                    uv, static_cast<size_t>(cfg_.width / 2));
        std::memset(rawFrame_->data[2] + row * rawFrame_->linesize[2],
                    vv, static_cast<size_t>(cfg_.width / 2));
    }
}

// ── Mode::NoSignal ────────────────────────────────────────────────────────────
// TV static noise background + centred "AUX" label + HH:MM:SS clock.
void VideoSender::fillNoSignal(int frameNum)
{
    const int W  = cfg_.width;
    const int H  = cfg_.height;
    const int ls = rawFrame_->linesize[0];   // Y stride (may differ from W)
    const int ls2 = rawFrame_->linesize[1];  // UV stride

    // ── 1. Noise background ──────────────────────────────────────────────────
    // Simple LCG seeded from frameNum so consecutive frames differ noticeably.
    uint32_t rng = static_cast<uint32_t>(frameNum) * 1664525u + 1013904223u;
    for (int row = 0; row < H; ++row) {
        uint8_t* dst = rawFrame_->data[0] + row * ls;
        for (int col = 0; col < W; ++col) {
            rng = rng * 1664525u + 1013904223u;
            dst[col] = static_cast<uint8_t>(40 + (rng >> 24));  // Y 40..295→40..255
        }
    }
    // Chroma: neutral grey (noise is already in Y luma)
    for (int row = 0; row < H / 2; ++row) {
        std::memset(rawFrame_->data[1] + row * ls2, 128,
                    static_cast<size_t>(W / 2));
        std::memset(rawFrame_->data[2] + row * rawFrame_->linesize[2], 128,
                    static_cast<size_t>(W / 2));
    }

    // ── 2. Build a flat Y buffer for text drawing (stride == W) ─────────────
    // We draw into a temporary flat Y, then copy back respecting linesize.
    std::vector<uint8_t> flatY(static_cast<size_t>(W * H));
    for (int row = 0; row < H; ++row)
        std::memcpy(flatY.data() + row * W,
                    rawFrame_->data[0] + row * ls,
                    static_cast<size_t>(W));

    uint8_t* Y = flatY.data();

    // ── 3. Centred dark box ──────────────────────────────────────────────────
    // Box covers ~60% width × 50% height of frame
    int boxW = W * 6 / 10;
    int boxH = H * 5 / 10;
    int boxX = (W - boxW) / 2;
    int boxY = (H - boxH) / 2;
    pf::fillRectY(Y, W, H, boxX, boxY, boxW, boxH, 12);

    // ── 4. "AUX" label (large, centred in box) ───────────────────────────────
    // Scale: target ~40% of box height for "AUX" (7×scale pixels tall)
    int scale = std::max(1, boxH * 2 / (7 * 5));
    int auxW = pf::measureString("AUX", scale);
    int auxX = boxX + (boxW - auxW) / 2;
    int auxY = boxY + boxH / 5;
    pf::drawStringY(Y, W, H, auxX, auxY, "AUX", scale, 235);

    // ── 5. "NO SIGNAL" label (smaller, below "AUX") ──────────────────────────
    int scale2 = std::max(1, scale * 2 / 3);
    int nsW = pf::measureString("NO SIGNAL", scale2);
    int nsX = boxX + (boxW - nsW) / 2;
    int nsY = auxY + 7 * scale + scale * 3;
    pf::drawStringY(Y, W, H, nsX, nsY, "NO SIGNAL", scale2, 190);

    // ── 6. Clock (bottom of box) ──────────────────────────────────────────────
    int clockScale = std::max(1, scale2);
    int clockW = pf::measureString("00:00:00", clockScale);
    int clockX  = boxX + (boxW - clockW) / 2;
    int clockY  = boxY + boxH - 7 * clockScale - scale * 3;
    pf::drawClockY(Y, W, H, clockX, clockY, clockScale, 200);

    // ── 7. Copy flat Y back with stride ─────────────────────────────────────
    for (int row = 0; row < H; ++row)
        std::memcpy(rawFrame_->data[0] + row * ls,
                    Y + row * W,
                    static_cast<size_t>(W));
}

// ── Mode::MainStream ──────────────────────────────────────────────────────
// Clean dark background + "MAIN STREAM SENDING" title + large HH:MM:SS clock.
// Designed for maximum readability on MCU displays.
void VideoSender::fillMainStream(int frameNum)
{
    const int W  = cfg_.width;
    const int H  = cfg_.height;
    const int ls = rawFrame_->linesize[0];

    // ── 1. Solid dark background (Y=20, very dark grey) ─────────────────
    for (int row = 0; row < H; ++row)
        std::memset(rawFrame_->data[0] + row * ls, 20, static_cast<size_t>(W));
    // Chroma: slight blue tint (U=140, V=118) for a professional dark-blue look
    for (int row = 0; row < H / 2; ++row) {
        std::memset(rawFrame_->data[1] + row * rawFrame_->linesize[1],
                    140, static_cast<size_t>(W / 2));
        std::memset(rawFrame_->data[2] + row * rawFrame_->linesize[2],
                    118, static_cast<size_t>(W / 2));
    }

    // ── 2. Build flat Y buffer for text drawing ─────────────────────────
    std::vector<uint8_t> flatY(static_cast<size_t>(W * H));
    for (int row = 0; row < H; ++row)
        std::memcpy(flatY.data() + row * W,
                    rawFrame_->data[0] + row * ls,
                    static_cast<size_t>(W));
    uint8_t* Y = flatY.data();

    // ── 3. Horizontal separator lines (subtle) ──────────────────────────
    int sepY1 = H * 35 / 100;   // upper separator
    int sepY2 = H * 65 / 100;   // lower separator
    for (int x = W / 6; x < W * 5 / 6; ++x) {
        if (sepY1 >= 0 && sepY1 < H) Y[sepY1 * W + x] = 60;
        if (sepY2 >= 0 && sepY2 < H) Y[sepY2 * W + x] = 60;
    }

    // ── 4. Title: "MAIN STREAM" (centered, above upper sep) ──────────────
    int titleScale = std::max(1, H / 60);
    const char* title = "MAIN STREAM";
    int titleW = pf::measureString(title, titleScale);
    int titleX = (W - titleW) / 2;
    int titleY = sepY1 - 7 * titleScale - titleScale * 2;
    if (titleY < titleScale) titleY = titleScale;
    pf::drawStringY(Y, W, H, titleX, titleY, title, titleScale, 200);

    // ── 5. Large clock HH:MM:SS (centered, between separators) ──────────
    int clockScale = std::max(2, H / 30);
    int clockW = pf::measureString("00:00:00", clockScale);
    int clockX = (W - clockW) / 2;
    int clockY = sepY1 + (sepY2 - sepY1 - 7 * clockScale) / 2;
    pf::drawClockY(Y, W, H, clockX, clockY, clockScale, 235);

    // ── 6. Date line YYYY/MM/DD (centered, below lower sep) ─────────────
    int dateScale = std::max(1, titleScale);
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    char dateBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d",
             lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    int dateW = pf::measureString(dateBuf, dateScale);
    int dateX = (W - dateW) / 2;
    int dateY = sepY2 + dateScale * 2;
    pf::drawStringY(Y, W, H, dateX, dateY, dateBuf, dateScale, 180);

    // ── 7. Copy flat Y back with stride ─────────────────────────────────
    for (int row = 0; row < H; ++row)
        std::memcpy(rawFrame_->data[0] + row * ls,
                    Y + row * W,
                    static_cast<size_t>(W));
}

// ── Mode::AuxStream ───────────────────────────────────────────────────────
// Identical layout to MainStream but title = "AUX STREAM" and a warm amber
// chroma tint, so operators can visually distinguish main vs aux on the MCU.
void VideoSender::fillAuxStream(int frameNum)
{
    const int W  = cfg_.width;
    const int H  = cfg_.height;
    const int ls = rawFrame_->linesize[0];

    // ── 1. Dark background with warm amber tint (U=100, V=150) ──────────
    for (int row = 0; row < H; ++row)
        std::memset(rawFrame_->data[0] + row * ls, 18, static_cast<size_t>(W));
    for (int row = 0; row < H / 2; ++row) {
        std::memset(rawFrame_->data[1] + row * rawFrame_->linesize[1],
                    100, static_cast<size_t>(W / 2));   // U: warm amber
        std::memset(rawFrame_->data[2] + row * rawFrame_->linesize[2],
                    150, static_cast<size_t>(W / 2));   // V: warm amber
    }

    // ── 2. Flat Y buffer ────────────────────────────────────────────────
    std::vector<uint8_t> flatY(static_cast<size_t>(W * H));
    for (int row = 0; row < H; ++row)
        std::memcpy(flatY.data() + row * W,
                    rawFrame_->data[0] + row * ls,
                    static_cast<size_t>(W));
    uint8_t* Y = flatY.data();

    // ── 3. Separator lines ───────────────────────────────────────────────
    int sepY1 = H * 35 / 100;
    int sepY2 = H * 65 / 100;
    for (int x = W / 6; x < W * 5 / 6; ++x) {
        if (sepY1 >= 0 && sepY1 < H) Y[sepY1 * W + x] = 60;
        if (sepY2 >= 0 && sepY2 < H) Y[sepY2 * W + x] = 60;
    }

    // ── 4. Title: "AUX STREAM" ───────────────────────────────────────────
    int titleScale = std::max(1, H / 60);
    const char* title = "AUX STREAM";
    int titleW = pf::measureString(title, titleScale);
    int titleX = (W - titleW) / 2;
    int titleY = sepY1 - 7 * titleScale - titleScale * 2;
    if (titleY < titleScale) titleY = titleScale;
    pf::drawStringY(Y, W, H, titleX, titleY, title, titleScale, 200);

    // ── 5. Large clock ───────────────────────────────────────────────────
    int clockScale = std::max(2, H / 30);
    int clockW = pf::measureString("00:00:00", clockScale);
    int clockX = (W - clockW) / 2;
    int clockY = sepY1 + (sepY2 - sepY1 - 7 * clockScale) / 2;
    pf::drawClockY(Y, W, H, clockX, clockY, clockScale, 235);

    // ── 6. Date line ─────────────────────────────────────────────────────
    int dateScale = std::max(1, titleScale);
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    char dateBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d",
             lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    int dateW = pf::measureString(dateBuf, dateScale);
    int dateX = (W - dateW) / 2;
    int dateY = sepY2 + dateScale * 2;
    pf::drawStringY(Y, W, H, dateX, dateY, dateBuf, dateScale, 180);

    // ── 7. Copy flat Y back ──────────────────────────────────────────────
    for (int row = 0; row < H; ++row)
        std::memcpy(rawFrame_->data[0] + row * ls,
                    Y + row * W,
                    static_cast<size_t>(W));
}

// ─── RTP helpers ─────────────────────────────────────────────────────────────

void VideoSender::sendRtp(const uint8_t* payload, int payloadLen,
                          bool marker, uint32_t ts)
{
    if (sockFd_ < 0 || destPort_ == 0) return;

    uint8_t buf[kMaxRtpPayload + kRtpHeaderSize];
    buf[0]  = 0x80;   // V=2, P=0, X=0, CC=0
    buf[1]  = static_cast<uint8_t>((marker ? 0x80 : 0) |
                                   (payloadType_ & 0x7F));
    buf[2]  = static_cast<uint8_t>((rtpSeq_ >> 8) & 0xFF);
    buf[3]  = static_cast<uint8_t>( rtpSeq_        & 0xFF);
    ++rtpSeq_;
    buf[4]  = static_cast<uint8_t>((ts >> 24) & 0xFF);
    buf[5]  = static_cast<uint8_t>((ts >> 16) & 0xFF);
    buf[6]  = static_cast<uint8_t>((ts >>  8) & 0xFF);
    buf[7]  = static_cast<uint8_t>( ts         & 0xFF);
    buf[8]  = static_cast<uint8_t>((rtpSsrc_ >> 24) & 0xFF);
    buf[9]  = static_cast<uint8_t>((rtpSsrc_ >> 16) & 0xFF);
    buf[10] = static_cast<uint8_t>((rtpSsrc_ >>  8) & 0xFF);
    buf[11] = static_cast<uint8_t>( rtpSsrc_         & 0xFF);

    std::memcpy(buf + kRtpHeaderSize, payload,
                static_cast<size_t>(payloadLen));

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(static_cast<uint16_t>(destPort_));
    inet_aton(destIP_.c_str(), &dest.sin_addr);

    ::sendto(sockFd_, buf, static_cast<size_t>(kRtpHeaderSize + payloadLen),
             0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void VideoSender::sendNal(const uint8_t* data, int len,
                          bool lastNal, uint32_t ts)
{
    if (len <= 0) return;

    if (len <= kMaxRtpPayload) {
        // Single NAL unit packet (RFC 6184 §5.6)
        sendRtp(data, len, lastNal, ts);
    } else {
        // FU-A fragmentation (RFC 6184 §5.8)
        uint8_t nalHdr = data[0];
        uint8_t nalType = nalHdr & 0x1F;
        uint8_t nri     = nalHdr & 0x60;

        const uint8_t* payload = data + 1;
        int remaining = len - 1;
        bool first = true;

        uint8_t pkt[kMaxRtpPayload];
        while (remaining > 0) {
            int chunk = std::min(remaining, kMaxRtpPayload - 2);
            bool last = (remaining == chunk);

            pkt[0] = nri | 28u;   // FU indicator: NRI from orig, type=28 (FU-A)
            pkt[1] = static_cast<uint8_t>(
                       (first ? 0x80u : 0u) |
                       (last  ? 0x40u : 0u) |
                       nalType);             // FU header
            std::memcpy(pkt + 2, payload, static_cast<size_t>(chunk));

            sendRtp(pkt, 2 + chunk, last && lastNal, ts);

            payload   += chunk;
            remaining -= chunk;
            first      = false;
        }
    }
}

void VideoSender::sendEncodedPacket(AVPacket* pkt, uint32_t ts)
{
    // Split Annex B bitstream into individual NAL units.
    const uint8_t* data = pkt->data;
    int            size = pkt->size;

    struct NalSpan { const uint8_t* ptr; int len; };
    std::vector<NalSpan> nals;

    int i = 0;
    int nalStart = -1;

    while (i < size) {
        bool sc4 = (i + 3 < size  && data[i]==0 && data[i+1]==0 &&
                    data[i+2]==0  && data[i+3]==1);
        bool sc3 = (!sc4 &&
                    i + 2 < size  && data[i]==0 && data[i+1]==0 &&
                    data[i+2]==1);

        if (sc4 || sc3) {
            if (nalStart >= 0)
                nals.push_back({data + nalStart, i - nalStart});
            nalStart = i + (sc4 ? 4 : 3);
            i = nalStart;
        } else {
            ++i;
        }
    }
    if (nalStart >= 0 && nalStart < size)
        nals.push_back({data + nalStart, size - nalStart});

    for (size_t j = 0; j < nals.size(); ++j) {
        bool last = (j + 1 == nals.size());
        sendNal(nals[j].ptr, nals[j].len, last, ts);
    }
}

// ─── Encoder thread ───────────────────────────────────────────────────────────

void VideoSender::threadMain()
{
    spdlog::info("VideoSender: thread started");

    if (!initEncoder()) {
        running_ = false;
        spdlog::error("VideoSender: encoder init failed, thread exit");
        return;
    }

    const uint32_t tsIncrement =
        static_cast<uint32_t>(90000u / static_cast<unsigned>(cfg_.fps));
    const auto frameDuration =
        std::chrono::milliseconds(1000 / cfg_.fps);

    int     frameNum    = 0;
    auto    nextFrame   = clk::now();

    while (!stopReq_.load()) {
        // ── Generate & encode ─────────────────────────────────────────────
        av_frame_make_writable(rawFrame_);
        fillFrame(frameNum);
        rawFrame_->pts = frameNum;

        int ret = avcodec_send_frame(encCtx_, rawFrame_);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            spdlog::warn("VideoSender: avcodec_send_frame error {}", ret);
            break;
        }

        while ((ret = avcodec_receive_packet(encCtx_, encPkt_)) == 0) {
            sendEncodedPacket(encPkt_, rtpTs_);
            rtpTs_ += tsIncrement;
            av_packet_unref(encPkt_);
        }

        ++frameNum;

        // ── Pace to fps ───────────────────────────────────────────────────
        nextFrame += frameDuration;
        auto now = clk::now();
        if (nextFrame > now)
            std::this_thread::sleep_until(nextFrame);
        else
            nextFrame = now;  // reset if we fell behind
    }

    // Flush encoder
    avcodec_send_frame(encCtx_, nullptr);
    while (avcodec_receive_packet(encCtx_, encPkt_) == 0) {
        sendEncodedPacket(encPkt_, rtpTs_);
        rtpTs_ += tsIncrement;
        av_packet_unref(encPkt_);
    }

    spdlog::info("VideoSender: thread exit");
}
