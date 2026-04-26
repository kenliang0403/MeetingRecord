#pragma once

#include <ptlib.h>
#include <ptlib/videoio.h>
#include "FfmpegRecorder.h"
#include <memory>

/**
 * VideoOutputDevice — a PTLib PVideoOutputDevice that captures
 * decoded YUV420P frames from H.323Plus and feeds them to FfmpegRecorder.
 */
class VideoOutputDevice : public PVideoOutputDevice
{
    PCLASSINFO(VideoOutputDevice, PVideoOutputDevice);

public:
    explicit VideoOutputDevice(std::shared_ptr<FfmpegRecorder> recorder);

    // PVideoOutputDevice interface
    PBoolean Open(const PString& deviceName, PBoolean startImmediate = TRUE) override;
    PBoolean IsOpen() override;
    PBoolean Close() override;
    PBoolean Start() override;
    PBoolean Stop() override;
    PBoolean SetColourFormat(const PString& colourFormat) override;
    PBoolean SetFrameSize(unsigned width, unsigned height) override;
    PBoolean SetFrameData(unsigned x, unsigned y,
                          unsigned width, unsigned height,
                          const BYTE* data, PBoolean endFrame) override;

    static PStringList GetOutputDeviceNames();
    PStringList GetDeviceNames() const override { return GetOutputDeviceNames(); }

private:
    std::shared_ptr<FfmpegRecorder> recorder_;
    std::vector<uint8_t>            frameBuffer_;
    unsigned frameWidth_  = 0;
    unsigned frameHeight_ = 0;
    bool     isOpen_      = false;
    int64_t  ptsMs_       = 0;
};
