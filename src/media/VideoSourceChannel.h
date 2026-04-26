#pragma once

// PTLib MUST be included first in any TU that uses PTLib headers.
// This header is only included from .cpp files that already have #include <ptlib.h>.
#include <ptlib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

/**
 * VideoSourceChannel — PTLib PChannel that generates animated YUV420P frames
 * for use as the H.323 primary video ENCODE source (main stream).
 *
 * Visual: slowly-shifting colour-band gradient background, a "REC" block
 * bouncing around in a Lissajous path, and a HH:MM:SS wall-clock rendered
 * with a built-in 5×7 pixel-art font in the bottom-right corner.
 *
 * H.323Plus codec calls Read() at the negotiated frame rate; this class
 * paces itself to fps_ and returns a freshly-rendered frame each call.
 */
class VideoSourceChannel : public PChannel
{
    PCLASSINFO(VideoSourceChannel, PChannel);
public:
    VideoSourceChannel(unsigned width, unsigned height, int fps);

    // PTLib PChannel interface
    PBoolean Read(void* buf, PINDEX len) override;
    PBoolean Write(const void*, PINDEX) override { return FALSE; }
    PBoolean IsOpen()  const override { return TRUE; }
    PBoolean Close()         override { return TRUE; }
    PString  GetName() const override { return "VideoSourceChannel"; }

    // Called by codec when negotiated dimensions change
    void updateDimensions(unsigned w, unsigned h);

private:
    void generateFrame(uint8_t* yuv, unsigned W, unsigned H);
    void fillGradient(uint8_t* Y, uint8_t* U, uint8_t* V,
                      unsigned W, unsigned H, uint64_t nowMs);
    void drawBouncingBlock(uint8_t* Y, unsigned W, unsigned H, uint64_t nowMs);
    void drawCornerClock(uint8_t* Y, unsigned W, unsigned H);

    unsigned width_;
    unsigned height_;
    int      fps_;
    uint64_t frameMs_ = 0;   // wall-clock ms of last frame (for animation time)

    std::chrono::steady_clock::time_point nextFrameTime_{};
    mutable std::mutex dimMutex_;
};
