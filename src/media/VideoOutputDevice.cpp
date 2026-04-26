#include "VideoOutputDevice.h"
#include <spdlog/spdlog.h>
#include <chrono>

PCREATE_VIDINPUT_PLUGIN_EX(VideoRecorder, VideoOutputDevice,
                            "VideoRecorder",
                            "Recorder video output device")

VideoOutputDevice::VideoOutputDevice(std::shared_ptr<FfmpegRecorder> recorder)
    : recorder_(std::move(recorder))
{
    // Default format
    colourFormat = "YUV420P";
}

PBoolean VideoOutputDevice::Open(const PString& /*deviceName*/,
                                  PBoolean /*startImmediate*/)
{
    isOpen_ = true;
    return TRUE;
}

PBoolean VideoOutputDevice::IsOpen()
{
    return isOpen_ && recorder_ && recorder_->isOpen();
}

PBoolean VideoOutputDevice::Close()
{
    isOpen_ = false;
    return TRUE;
}

PBoolean VideoOutputDevice::Start() { return TRUE; }
PBoolean VideoOutputDevice::Stop()  { return TRUE; }

PBoolean VideoOutputDevice::SetColourFormat(const PString& fmt)
{
    colourFormat = fmt;
    return fmt == "YUV420P";
}

PBoolean VideoOutputDevice::SetFrameSize(unsigned width, unsigned height)
{
    frameWidth_  = width;
    frameHeight_ = height;
    frameBuffer_.resize(width * height * 3 / 2);
    return PVideoOutputDevice::SetFrameSize(width, height);
}

PBoolean VideoOutputDevice::SetFrameData(unsigned x, unsigned y,
                                          unsigned width, unsigned height,
                                          const BYTE* data, PBoolean endFrame)
{
    if (!recorder_ || !recorder_->isOpen()) return TRUE;

    // Accumulate frame data (H.323Plus may call this in tiles)
    if (x == 0 && y == 0 && width == frameWidth_ && height == frameHeight_) {
        // Full frame in one call — fast path
        if (endFrame) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            recorder_->writeVideoFrame(data, width, height, now);
        }
    } else {
        // Partial update (tiled) — accumulate into buffer
        // For YUV420P, y-plane only in first 2/3 of the buffer
        if (!frameBuffer_.empty() && width == frameWidth_) {
            size_t yOffset  = y * frameWidth_ + x;
            size_t uvOffset = frameWidth_ * frameHeight_ + (y / 2) * (frameWidth_ / 2) + x / 2;
            size_t uvH      = height / 2;
            size_t uvW      = width / 2;

            const BYTE* src = data;
            std::copy(src, src + width * height, frameBuffer_.begin() + yOffset);
            src += width * height;
            std::copy(src, src + uvW * uvH, frameBuffer_.begin() + frameWidth_ * frameHeight_ + y / 2 * frameWidth_ / 2);
            src += uvW * uvH;
            std::copy(src, src + uvW * uvH, frameBuffer_.begin() + frameWidth_ * frameHeight_ * 5 / 4 + y / 2 * frameWidth_ / 2);
        }

        if (endFrame) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            recorder_->writeVideoFrame(frameBuffer_.data(), frameWidth_, frameHeight_, now);
        }
    }

    return TRUE;
}

PStringList VideoOutputDevice::GetOutputDeviceNames()
{
    PStringList list;
    list.AppendString("VideoRecorder");
    return list;
}
