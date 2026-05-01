#include "AudioLevelMeter.h"
#include <cmath>
#include <algorithm>

namespace {
inline int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

// int16 满幅 = 32768，对应 0 dBFS
inline double toDbfs(double linear) {
    if (linear <= 1e-9) return -120.0;
    double dbfs = 20.0 * std::log10(linear / 32768.0);
    if (dbfs < -120.0) dbfs = -120.0;
    if (dbfs >    0.0) dbfs =    0.0;
    return dbfs;
}
}  // namespace

void AudioLevelMeter::feedPcm(const int16_t* pcm, int sampleCount, int channels)
{
    if (!pcm || sampleCount <= 0 || channels <= 0) return;

    int total = sampleCount * channels;
    int32_t peak = 0;
    double  sumSq = 0.0;

    for (int i = 0; i < total; ++i) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
        sumSq += double(pcm[i]) * double(pcm[i]);
    }

    double rms = std::sqrt(sumSq / double(total));

    // peak: 直接覆盖（瞬时）
    peakDbfs_.store(toDbfs(double(peak)), std::memory_order_relaxed);

    // rms: 指数移动平均（线性域做平滑，再转 dB；这样静默→响时上升更自然）
    double prevDb = rmsDbfs_.load(std::memory_order_relaxed);
    double prevLin = std::pow(10.0, prevDb / 20.0) * 32768.0;
    double smoothed = prevLin * (1.0 - kRmsAlpha_) + rms * kRmsAlpha_;
    rmsDbfs_.store(toDbfs(smoothed), std::memory_order_relaxed);

    lastFeedMs_.store(nowMs(), std::memory_order_relaxed);
}

AudioLevelMeter::Snapshot AudioLevelMeter::snapshot() const
{
    int64_t last = lastFeedMs_.load(std::memory_order_relaxed);
    int64_t age  = last == 0 ? 999999 : (nowMs() - last);

    Snapshot s;
    s.peak_dbfs = peakDbfs_.load(std::memory_order_relaxed);
    s.rms_dbfs  = rmsDbfs_.load(std::memory_order_relaxed);
    s.age_ms    = age;

    // 超过 500ms 没数据 → 视为静默，把读数衰减到 -120
    if (age > 500) {
        s.peak_dbfs = -120.0;
        s.rms_dbfs  = -120.0;
    }
    return s;
}

void AudioLevelMeter::reset()
{
    peakDbfs_.store(-120.0, std::memory_order_relaxed);
    rmsDbfs_.store( -120.0, std::memory_order_relaxed);
    lastFeedMs_.store(0, std::memory_order_relaxed);
}
