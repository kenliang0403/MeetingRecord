// PTLib must be first
#include <ptlib.h>

#include "VideoCapturePChannel.h"
#include "FfmpegRecorder.h"
#include <spdlog/spdlog.h>
#include <chrono>

VideoCapturePChannel::VideoCapturePChannel(
    std::shared_ptr<FfmpegRecorder> recorder,
    unsigned width, unsigned height)
    : recorder_(std::move(recorder))
    , width_(width)
    , height_(height)
{
}

PBoolean VideoCapturePChannel::Read(void* /*buf*/, PINDEX /*len*/)
{
    return FALSE;
}

PBoolean VideoCapturePChannel::Write(const void* buf, PINDEX len)
{
    if (!recorder_ || !recorder_->isOpen()) return TRUE;

    // Validate that we have at least enough data for a 1x1 YUV420P frame.
    // The codec calls updateDimensions() before Write(), so width_/height_ are
    // the actual decoded frame dimensions at this point.
    size_t expected = static_cast<size_t>(width_) * height_ * 3 / 2;
    if (expected > 0 && static_cast<size_t>(len) < expected) {
        spdlog::warn("VideoCapturePChannel: short frame {} < {}x{}*3/2={}",
                     (int)len, width_, height_, expected);
        return TRUE;
    }

    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    recorder_->writeVideoFrame(
        reinterpret_cast<const uint8_t*>(buf), width_, height_, nowMs);

    lastWriteCount = len;
    return TRUE;
}

void VideoCapturePChannel::WriteAVFrame(const AVFrame* frame)
{
    if (!recorder_ || !recorder_->isOpen()) return;

    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    recorder_->writeVideoAVFrame(frame, nowMs);
}

PBoolean VideoCapturePChannel::Close()
{
    return TRUE;
}
