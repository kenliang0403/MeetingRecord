#include "AudioSoundChannel.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>

PCREATE_SOUND_PLUGIN(RecorderAudio, AudioSoundChannel)

AudioSoundChannel::AudioSoundChannel(std::shared_ptr<FfmpegRecorder> recorder,
                                     int sampleRate, int channels)
    : recorder_(std::move(recorder))
    , sampleRate_(sampleRate)
    , channels_(channels)
{
}

PBoolean AudioSoundChannel::Open(const PString& /*device*/, Directions /*dir*/,
                                  unsigned ch, unsigned sr, unsigned /*bits*/)
{
    channels_   = ch;
    sampleRate_ = sr;
    isOpen_     = true;
    return TRUE;
}

PBoolean AudioSoundChannel::IsOpen() const
{
    return isOpen_ && recorder_ && recorder_->isOpen();
}

PBoolean AudioSoundChannel::Close()
{
    isOpen_ = false;
    return TRUE;
}

PBoolean AudioSoundChannel::Write(const void* buf, PINDEX len)
{
    if (!recorder_ || !recorder_->isOpen()) return TRUE;

    int sampleCount = len / sizeof(int16_t);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    recorder_->writeAudioSamples(
        reinterpret_cast<const int16_t*>(buf), sampleCount, now);
    return TRUE;
}

PBoolean AudioSoundChannel::Read(void* buf, PINDEX len)
{
    // We are a sink only; return silence for outbound
    std::memset(buf, 0, len);
    return TRUE;
}

PBoolean AudioSoundChannel::SetFormat(unsigned channels, unsigned sampleRate,
                                       unsigned /*bitsPerSample*/)
{
    channels_   = channels;
    sampleRate_ = sampleRate;
    return TRUE;
}

PStringList AudioSoundChannel::GetDeviceNames(Directions /*dir*/)
{
    PStringList list;
    list.AppendString("RecorderAudio");
    return list;
}
