#pragma once

#include <ptlib.h>
#include <ptlib/sound.h>
#include "FfmpegRecorder.h"
#include <memory>

/**
 * AudioSoundChannel — a PTLib PSoundChannel that captures decoded
 * 16-bit PCM audio from H.323Plus and feeds it to FfmpegRecorder.
 *
 * Used as the audio SINK for incoming audio streams.
 */
class AudioSoundChannel : public PSoundChannel
{
    PCLASSINFO(AudioSoundChannel, PSoundChannel);

public:
    AudioSoundChannel(std::shared_ptr<FfmpegRecorder> recorder,
                      int sampleRate, int channels);

    // PSoundChannel interface
    PBoolean Open(const PString& device, Directions dir,
                  unsigned channels, unsigned sampleRate,
                  unsigned bitsPerSample) override;
    PBoolean IsOpen() const override;
    PBoolean Close() override;
    PBoolean Write(const void* buf, PINDEX len) override;
    PBoolean Read(void* buf, PINDEX len) override;
    PBoolean SetFormat(unsigned channels, unsigned sampleRate,
                       unsigned bitsPerSample) override;
    unsigned GetChannels() const override { return channels_; }
    unsigned GetSampleRate() const override { return sampleRate_; }
    unsigned GetSampleSize() const override { return 16; }

    static PStringList GetDeviceNames(Directions dir = Recorder);

private:
    std::shared_ptr<FfmpegRecorder> recorder_;
    int   sampleRate_;
    int   channels_;
    bool  isOpen_ = false;
    int64_t ptsMs_ = 0;
};
