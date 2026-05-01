// PTLib must be first
#include <ptlib.h>

#include "AudioCapturePChannel.h"
#include "FfmpegRecorder.h"
#include "AudioLevelMeter.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <cstring>

AudioCapturePChannel::AudioCapturePChannel(
    std::shared_ptr<FfmpegRecorder> recorder,
    int sampleRate, int channels)
    : recorder_(std::move(recorder))
    , sampleRate_(sampleRate)
    , channels_(channels)
{
}

PBoolean AudioCapturePChannel::Open(const PString& /*device*/,
                                     Directions /*dir*/,
                                     unsigned ch, unsigned sr, unsigned /*bits*/)
{
    if (ch > 0)  channels_   = (int)ch;
    if (sr > 0)  sampleRate_ = (int)sr;
    isOpen_ = true;
    firstWrite_ = true;
    firstRead_  = true;
    return TRUE;
}

PBoolean AudioCapturePChannel::Close()
{
    isOpen_ = false;
    return TRUE;
}

PBoolean AudioCapturePChannel::Write(const void* buf, PINDEX len)
{
    if (!recorder_ || !recorder_->isOpen()) return TRUE;

    int samples = (int)len / 2;  // int16_t = 2 bytes
    if (samples <= 0) return TRUE;

    int activeChannels = channels_ > 0 ? channels_ : 1;
    int sampleCountPerChannel = samples / activeChannels;
    int activeSampleRate = sampleRate_ > 0 ? sampleRate_ : 16000;

    // Calculate how long this chunk of audio represents in microseconds
    auto chunkDurationUs = std::chrono::microseconds(
        sampleCountPerChannel * 1000000LL / activeSampleRate);

    auto now = std::chrono::steady_clock::now();

    if (firstWrite_) {
        nextWriteTime_ = now + chunkDurationUs;
        firstWrite_ = false;
    } else {
        if (nextWriteTime_ > now) {
            std::this_thread::sleep_until(nextWriteTime_);
        } else if (now - nextWriteTime_ > std::chrono::milliseconds(500)) {
            // We fell way behind (e.g. paused), reset clock to avoid bursts
            nextWriteTime_ = now;
        }
        nextWriteTime_ += chunkDurationUs;
    }

    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    recorder_->writeAudioSamples(
        reinterpret_cast<const int16_t*>(buf), sampleCountPerChannel, nowMs);

    // 实时电平采样供 web 管理页 VU 表使用（无锁）
    AudioLevelMeter::instance().feedPcm(
        reinterpret_cast<const int16_t*>(buf),
        sampleCountPerChannel,
        activeChannels);

    lastWriteCount = len;
    return TRUE;
}

PBoolean AudioCapturePChannel::Read(void* buf, PINDEX len)
{
    int samples = (int)len / 2;
    int activeChannels = channels_ > 0 ? channels_ : 1;
    int sampleCountPerChannel = samples / activeChannels;
    int activeSampleRate = sampleRate_ > 0 ? sampleRate_ : 16000;

    auto chunkDurationUs = std::chrono::microseconds(
        sampleCountPerChannel * 1000000LL / activeSampleRate);
    auto now = std::chrono::steady_clock::now();

    if (firstRead_) {
        nextReadTime_ = now + chunkDurationUs;
        firstRead_ = false;
    } else {
        if (nextReadTime_ > now) {
            std::this_thread::sleep_until(nextReadTime_);
        } else if (now - nextReadTime_ > std::chrono::milliseconds(500)) {
            nextReadTime_ = now;
        }
        nextReadTime_ += chunkDurationUs;
    }

    std::memset(buf, 0, len);
    lastReadCount = len;
    return TRUE;
}

PBoolean AudioCapturePChannel::SetFormat(unsigned ch, unsigned sr, unsigned /*bits*/)
{
    if (ch > 0) channels_   = (int)ch;
    if (sr > 0) sampleRate_ = (int)sr;
    return TRUE;
}

PStringList AudioCapturePChannel::GetDeviceNames(Directions /*dir*/)
{
    PStringList list;
    list.AppendString("RecorderAudio");
    return list;
}
