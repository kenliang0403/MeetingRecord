#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

/**
 * AudioLevelMeter — 进程级单例，从主流 audio 解码 PCM 路径上抽样实时电平。
 *
 * 输入：mono / multi-channel int16 PCM 帧
 * 输出：peak dBFS（瞬时） + RMS dBFS（指数平滑）+ 最后更新时间戳
 *
 * 设计：
 *   - 读写无锁（atomic<double>），Web 端通过 9001 控制端口高频拉取
 *   - 没有数据进来时（无通话/静音）值会衰减到 -inf；查询端把 < -60 当 silence
 *   - 单声道 / 多声道一律折成 mono peak/RMS（VU "左右两条"分别用 peak / RMS）
 */
class AudioLevelMeter
{
public:
    static AudioLevelMeter& instance() {
        static AudioLevelMeter inst;
        return inst;
    }

    // 从 AudioCapturePChannel::Write 中调用，每帧一次。
    // pcm 是 interleaved int16，sampleCount 是每声道样本数。
    void feedPcm(const int16_t* pcm, int sampleCount, int channels);

    struct Snapshot {
        double  peak_dbfs;     // 瞬时峰值（快速跳）
        double  rms_dbfs;      // RMS（慢速移动，指数平滑）
        int64_t age_ms;        // 距离上次 feedPcm 多少毫秒；> 500 表示静默/无通话
    };

    Snapshot snapshot() const;

    // 用于通话结束时复位，不影响新通话的电平起始点。
    void reset();

private:
    AudioLevelMeter() = default;

    // atomic<double> 在大部分平台是 lock-free（GCC/Clang on x86_64 yes）
    std::atomic<double>  peakDbfs_{ -120.0 };
    std::atomic<double>  rmsDbfs_{  -120.0 };
    // 用 steady_clock 时间戳的 ms count
    std::atomic<int64_t> lastFeedMs_{ 0 };

    // 平滑系数：约 200 ms 的指数移动平均
    static constexpr double kRmsAlpha_ = 0.05;
};
