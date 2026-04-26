#pragma once

// PTLib must be first in any translation unit using PTLib headers.
#include <ptlib.h>
#include <ptlib/sound.h>
#include <memory>
#include <chrono>

class FfmpegRecorder;

/**
 * AudioCapturePChannel — PSoundChannel that receives decoded PCM audio
 * from H.323Plus and feeds it to FfmpegRecorder.
 */
class AudioCapturePChannel : public PSoundChannel
{
    PCLASSINFO(AudioCapturePChannel, PSoundChannel);
public:
    AudioCapturePChannel(std::shared_ptr<FfmpegRecorder> recorder,
                         int sampleRate, int channels);

    PBoolean Open(const PString& device, Directions dir,
                  unsigned channels, unsigned sampleRate,
                  unsigned bitsPerSample) override;
    PBoolean IsOpen() const override { return isOpen_; }
    PBoolean Close() override;
    PBoolean Write(const void* buf, PINDEX len) override;
    PBoolean Read(void* buf, PINDEX len) override;
    PBoolean SetFormat(unsigned channels, unsigned sampleRate,
                       unsigned bitsPerSample) override;

    unsigned GetChannels()   const override { return (unsigned)channels_; }
    unsigned GetSampleRate() const override { return (unsigned)sampleRate_; }
    unsigned GetSampleSize() const override { return 16; }

    static PStringList GetDeviceNames(Directions dir = Player);

private:
    std::shared_ptr<FfmpegRecorder> recorder_;
    int  sampleRate_;
    int  channels_;
    bool isOpen_ = false;

    // Pacing logic to simulate real sound hardware blocking
    std::chrono::steady_clock::time_point nextWriteTime_;
    std::chrono::steady_clock::time_point nextReadTime_;
    bool firstWrite_ = true;
    bool firstRead_ = true;
};
