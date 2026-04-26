#pragma once

#include "../config/AppConfig.h"
#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdint>

class SrsStreamer;

// Forward declarations — keep FFmpeg out of this header to avoid
// polluting PTLib compilation units that include FfmpegRecorder.h
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct AVAudioFifo;
struct SwsContext;
struct SwrContext;

/**
 * FfmpegRecorder — writes decoded YUV420P video + PCM audio to MP4.
 *
 * Thread safety: writeVideoFrame and writeAudioSamples are mutex-protected.
 */
class FfmpegRecorder {
public:
    explicit FfmpegRecorder(const RecorderConfig& cfg);
    ~FfmpegRecorder();

    bool open(const std::string& outputPath,
              int videoWidth, int videoHeight, int fps,
              int sampleRate, int channels,
              bool hasAudio = true);
    void close();

    void writeVideoFrame(const uint8_t* yuv420p, int width, int height, int64_t ptsMs);
    void writeVideoAVFrame(const AVFrame* srcFrame, int64_t ptsMs);
    void writeAudioSamples(const int16_t* pcm, int sampleCount, int64_t ptsMs);

    bool isOpen()      const { return isOpen_.load(); }
    std::string outputPath() const { return outputPath_; }

    // Attach/detach a live streamer. The streamer must be already started.
    // Encoder output packets are cloned and pushed to it.
    // Safe to call while open — takes the same mutex as write* methods.
    // Passing nullptr detaches.
    void attachStreamer(std::shared_ptr<SrsStreamer> s);

private:
    bool addVideoStream();
    bool addAudioStream();
    bool writePacket(AVPacket* pkt, AVStream* st);

    const RecorderConfig& cfg_;

    AVFormatContext* fmtCtx_  = nullptr;
    AVCodecContext*  vEncCtx_ = nullptr;
    AVCodecContext*  aEncCtx_ = nullptr;
    AVStream*        vStream_ = nullptr;
    AVStream*        aStream_ = nullptr;
    SwsContext*      swsCtx_  = nullptr;
    SwrContext*      swrCtx_  = nullptr;
    AVAudioFifo*     aFifo_   = nullptr;
    AVFrame*         vEncodeFrame_ = nullptr;
    AVFrame*         aEncodeFrame_ = nullptr;
    AVPacket*        encodePkt_    = nullptr;

    int      vWidth_ = 0, vHeight_ = 0, vFps_ = 25;
    int      srcW_ = 0, srcH_ = 0;
    int      aSampleRate_ = 8000, aChannels_ = 1;
    int64_t  vPts_ = 0, aPts_ = 0;
    int64_t  globalStartMs_ = -1;
    bool     aPtsInitialized_ = false;

    std::string       outputPath_;
    std::atomic<bool> isOpen_{false};
    std::mutex        mu_;

    // Optional live streamer. When attached, every encoded packet is cloned
    // and pushed to it (non-blocking). Lifetime is managed by the owner.
    std::shared_ptr<SrsStreamer> streamer_;
    bool streamerParamsPushed_ = false;
};
