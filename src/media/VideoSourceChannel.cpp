// PTLib must be first
#include <ptlib.h>

#include "VideoSourceChannel.h"
#include "../util/PixelFont.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>

using clk = std::chrono::steady_clock;
using sc  = std::chrono::system_clock;

// ─── Colour palette (8 EBU colour bars, BT.601) ──────────────────────────────
namespace {
struct YUV { uint8_t y, u, v; };
static const YUV kPalette[8] = {
    {235, 128, 128},   // white
    {210,  16, 146},   // yellow
    {170, 166,  16},   // cyan
    {145,  54,  34},   // green
    {107, 202, 222},   // magenta
    { 82,  90, 240},   // red
    { 41, 240, 110},   // blue
    { 16, 128, 128},   // black
};
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

VideoSourceChannel::VideoSourceChannel(unsigned width, unsigned height, int fps)
    : width_(width > 0 ? width : 320)
    , height_(height > 0 ? height : 240)
    , fps_(fps > 0 ? fps : 10)
{
    nextFrameTime_ = clk::now();
}

void VideoSourceChannel::updateDimensions(unsigned w, unsigned h)
{
    std::lock_guard<std::mutex> lk(dimMutex_);
    if (w > 0) width_  = w;
    if (h > 0) height_ = h;
}

// ─── PChannel::Read ───────────────────────────────────────────────────────────

PBoolean VideoSourceChannel::Read(void* buf, PINDEX len)
{
    unsigned W, H;
    {
        std::lock_guard<std::mutex> lk(dimMutex_);
        W = width_;
        H = height_;
    }

    size_t expected = (size_t)W * H * 3u / 2u;
    if ((size_t)len < expected) {
        spdlog::warn("VideoSourceChannel::Read: buffer too small ({} < {})",
                     (int)len, (int)expected);
        return FALSE;
    }

    // Pace to fps_
    auto now = clk::now();
    if (nextFrameTime_ > now)
        std::this_thread::sleep_until(nextFrameTime_);
    nextFrameTime_ = clk::now() + std::chrono::milliseconds(1000 / fps_);

    frameMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                   sc::now().time_since_epoch()).count();

    generateFrame(static_cast<uint8_t*>(buf), W, H);

    lastReadCount = static_cast<PINDEX>(expected);
    return TRUE;
}

// ─── Frame generation ────────────────────────────────────────────────────────

void VideoSourceChannel::generateFrame(uint8_t* yuv, unsigned W, unsigned H)
{
    uint8_t* Y = yuv;
    uint8_t* U = yuv + W * H;
    uint8_t* V = yuv + W * H + (W / 2) * (H / 2);

    fillGradient(Y, U, V, W, H, frameMs_);
    drawBouncingBlock(Y, W, H, frameMs_);
    drawCornerClock(Y, W, H);
}

// Slowly-shifting colour-band gradient.
// The colour assignment shifts by 1 band every 2 seconds.
void VideoSourceChannel::fillGradient(uint8_t* Y, uint8_t* U, uint8_t* V,
                                       unsigned W, unsigned H, uint64_t nowMs)
{
    int shift = static_cast<int>(nowMs / 2000) % 8;

    // Y plane
    for (unsigned row = 0; row < H; ++row) {
        int band     = static_cast<int>(row * 8 / H);
        int colorIdx = (band + shift) % 8;
        std::memset(Y + row * W, kPalette[colorIdx].y, W);
    }
    // U / V planes (half resolution)
    for (unsigned row = 0; row < H / 2; ++row) {
        int band     = static_cast<int>((row * 2) * 8 / H);
        int colorIdx = (band + shift) % 8;
        std::memset(U + row * (W / 2), kPalette[colorIdx].u, W / 2);
        std::memset(V + row * (W / 2), kPalette[colorIdx].v, W / 2);
    }
}

// A dark rectangle with white "REC" label that bounces in a Lissajous path.
void VideoSourceChannel::drawBouncingBlock(uint8_t* Y,
                                            unsigned W, unsigned H,
                                            uint64_t nowMs)
{
    // Block size: ~W/5 × H/8, minimum 80×24
    unsigned bW = std::max(80u, W / 5);
    unsigned bH = std::max(24u, H / 8);

    double t = nowMs / 1000.0;
    // Elliptical Lissajous motion (ωx ≠ ωy → figure-8 variants over time)
    double halfX = (double)(W - bW) / 2.0 - 2.0;
    double halfY = (double)(H - bH) / 2.0 - 2.0;
    int cx = static_cast<int>(W / 2.0 + halfX * std::sin(t * 0.71));
    int cy = static_cast<int>(H / 2.0 + halfY * std::cos(t * 0.97));
    int bx = cx - static_cast<int>(bW) / 2;
    int by = cy - static_cast<int>(bH) / 2;
    bx = std::max(0, std::min(bx, static_cast<int>(W - bW)));
    by = std::max(0, std::min(by, static_cast<int>(H - bH)));

    // Dark fill (Y = 25)
    pf::fillRectY(Y, W, H, bx, by, bW, bH, 25);

    // "REC" label centred in block
    // Scale: target ~60 % of block height
    int scale = std::max(1, static_cast<int>(bH) * 3 / (7 * 5));
    int textW = pf::measureString("REC", scale);
    int tx = bx + (static_cast<int>(bW) - textW) / 2;
    int ty = by + (static_cast<int>(bH) - 7 * scale) / 2;
    pf::drawStringY(Y, W, H, tx, ty, "REC", scale, 235);

    // Blinking dot to the left of "REC" (on for even seconds)
    if ((static_cast<int>(nowMs / 500) % 2) == 0) {
        int dotX = tx - scale * 4;
        int dotY = ty + (7 * scale) / 2 - scale;
        pf::fillRectY(Y, W, H, dotX, dotY,
                      scale * 2, scale * 2, 200);
    }
}

// HH:MM:SS in the bottom-right corner with a dark background box.
void VideoSourceChannel::drawCornerClock(uint8_t* Y, unsigned W, unsigned H)
{
    // Choose scale so the clock is ~10 % of frame height
    int scale = std::max(1, static_cast<int>(H) / 70);
    int charW = 5 * scale + scale;
    int textW = 8 * charW - scale;   // "HH:MM:SS" = 8 chars
    int textH = 7 * scale;
    int pad   = scale * 3;

    int x = static_cast<int>(W) - textW - pad * 2;
    int y = static_cast<int>(H) - textH - pad * 2;

    // Background box
    pf::fillRectY(Y, W, H, x - pad, y - pad,
                  textW + pad * 2, textH + pad * 2, 18);

    pf::drawClockY(Y, W, H, x, y, scale, 235);
}
