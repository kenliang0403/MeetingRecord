#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Keep FFmpeg out of this header to avoid polluting PTLib TUs
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

/**
 * VideoSender — FFmpeg H.264 test-pattern encoder → RFC 6184 RTP → raw UDP.
 *
 * Usage (two-phase start so the OLC can carry our local RTP port):
 *
 *   VideoSender vs(VideoSender::Mode::NoSignal);
 *   if (!vs.initNetwork())        return false;   // bind socket, get local port
 *   int port = vs.localPort();                    // put this in OLC mediaChannel
 *   // ... send OLC, get MCU RTP receive address from Ack ...
 *   if (!vs.startSending(destIP, destPort)) return false;
 *   // later:
 *   vs.stop();
 *
 * Mode::ScreenSaver — cycling colour bars (legacy, for quick testing).
 * Mode::NoSignal    — TV-static noise background, centred "AUX" label and
 *                     HH:MM:SS wall-clock (used for H.239 aux stream).
 */
class VideoSender {
public:
    enum class Mode {
        ScreenSaver,   // colour-cycling test bars (original)
        NoSignal,      // TV static noise + "AUX" label + clock
        MainStream,    // clean dark bg + "MAIN STREAM" + large clock
        AuxStream      // clean dark bg + "AUX STREAM" + large clock (H.239)
    };

    struct Config {
        int  width   = 320;
        int  height  = 240;
        int  fps     = 10;
        int  bitrate = 256000;  // bps
        Mode mode    = Mode::NoSignal;
    };

    VideoSender();                              // default config (NoSignal)
    explicit VideoSender(const Config& cfg);   // custom config
    ~VideoSender();

    // Phase 1: bind UDP socket, get local port.
    // Returns false if socket cannot be created/bound.
    bool initNetwork();

    // Local RTP port (valid after initNetwork() succeeds).
    int localPort() const { return localPort_; }

    // Phase 2: start encoder thread, sending RTP to destIP:destPort.
    // initNetwork() must have been called successfully first.
    bool startSending(const std::string& destIP, int destPort, int payloadType = 106);

    void stop();

    bool isRunning() const { return running_.load(); }

private:
    void threadMain();
    bool initEncoder();
    void cleanup();

    // Fill rawFrame_ with a test pattern keyed to frameNum_.
    void fillFrame(int frameNum);
    void fillScreenSaver(int frameNum);
    void fillNoSignal(int frameNum);
    void fillMainStream(int frameNum);
    void fillAuxStream(int frameNum);

    // RTP helpers
    void sendEncodedPacket(AVPacket* pkt, uint32_t ts);
    void sendNal(const uint8_t* data, int len, bool lastNal, uint32_t ts);
    void sendRtp(const uint8_t* payload, int payloadLen, bool marker, uint32_t ts);

    Config cfg_;

    // Network (Phase 1)
    int  sockFd_    = -1;
    int  localPort_ = 0;

    // Destination (Phase 2)
    std::string destIP_;
    int         destPort_ = 0;

    // Thread control
    std::thread       th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopReq_{false};

    // FFmpeg
    AVCodecContext* encCtx_   = nullptr;
    AVFrame*        rawFrame_ = nullptr;
    AVPacket*       encPkt_   = nullptr;

    // RTP state (only touched by encoder thread)
    uint16_t rtpSeq_  = 0;
    uint32_t rtpTs_   = 0;
    uint32_t rtpSsrc_ = 0xCAFE5678;
    int      payloadType_ = 106;

    static constexpr int kMaxRtpPayload = 1200;
    static constexpr int kRtpHeaderSize = 12;
};
