#pragma once

// PTLib MUST be included first in any translation unit that includes PTLib headers.
// This header is only included from .cpp files that have #include <ptlib.h> first.
#include <ptlib.h>
#include <memory>
#include <cstdint>
#include <vector>

struct AVFrame; // Forward declaration

class FfmpegRecorder;

/**
 * VideoCapturePChannel — PTLib PChannel that receives decoded YUV420P frames
 * from H.323Plus codec and forwards them to FfmpegRecorder.
 */
class VideoCapturePChannel : public PChannel
{
    PCLASSINFO(VideoCapturePChannel, PChannel);
public:
    VideoCapturePChannel(std::shared_ptr<FfmpegRecorder> recorder,
                         unsigned width, unsigned height);

    // H323Codec::AttachChannel() returns channel->IsOpen().
    // A plain PChannel is only "open" after Open() is called.
    // Override IsOpen() to return TRUE so AttachChannel succeeds immediately.
    PBoolean IsOpen() const override { return TRUE; }

    PBoolean Read(void* buf, PINDEX len) override;
    PBoolean Write(const void* buf, PINDEX len) override;
    void WriteAVFrame(const AVFrame* frame);
    PBoolean Close() override;
    PString  GetName() const override { return "VideoCapturePChannel"; }

    // Called by codec when actual decoded frame dimensions are known
    void updateDimensions(unsigned w, unsigned h) { width_ = w; height_ = h; }

private:
    std::shared_ptr<FfmpegRecorder> recorder_;
    unsigned width_;
    unsigned height_;
};
