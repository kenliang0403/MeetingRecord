#pragma once
#include <string>

struct GkConfig {
    std::string host;           // VP9660 GK IP
    int         port    = 1719; // GK RAS port
    std::string alias   = "<recorder-alias>";
    std::string e164;           // optional E.164 number
    int         ttl     = 60;
    std::string username;
    std::string password;
    int         h323_port = 1720;  // local H.323 signalling listen port (change for multi-instance)
};

struct RecorderConfig {
    std::string output_dir        = "/opt/recorder/recordings";
    int         video_width       = 1920;
    int         video_height      = 1080;
    int         video_fps         = 25;
    int         audio_sample_rate = 8000;
    int         audio_channels    = 1;
    std::string video_codec       = "libx264";
    std::string audio_codec       = "aac";
    int         video_bitrate     = 1500000;
    int         audio_bitrate     = 128000;       // 录播+直播共享同一个 AAC 编码器
    int         rtp_port_base     = 20000;

    // 音量线性增益。1.0 = 原始音量；1.10 ≈ +0.83 dB（默认轻微提升 10%）；
    // 1.5 ≈ +3.5 dB；2.0 ≈ +6 dB。值大于 1 时若样本本身已接近满幅，
    // FfmpegRecorder 会做 saturating clamp 防爆音（信息会损失但不会失真）。
    double      audio_gain        = 1.10;
};

// Outgoing call: recorder dials the MCU conference room by itself.
struct OutgoingConfig {
    bool        enabled         = false;
    std::string dial_number;          // E.164 / alias to dial, e.g. "9000820575"
    std::string mcu_host;             // optional: MCU IP to call directly (bypass GK routing)
    bool        reconnect       = true;   // re-dial when call is cleared
    int         reconnect_delay_s = 5;    // seconds between reconnect attempts
    int         max_reconnects   = -1;    // -1 = unlimited
};

struct TcpConfig {
    std::string bind_addr = "0.0.0.0";
    int         port      = 9001;
};

// SRS 直播推流配置。启用后，FfmpegRecorder 编码后的 H.264 / AAC
// AVPacket 会被 clone 一份通过 FLV/RTMP muxer 推送到 SRS。MP4 录制
// 路径完全不受推流阻塞影响（异步队列 + 独立线程）。
struct StreamingConfig {
    bool        enabled         = false;
    // RTMP 基地址。SRS 安全策略只允许 127.0.0.1 推流，所以本机推 loopback。
    // 播放地址仍用 <recorder_host>:1935 / :8080，由 SRS 路由。
    std::string rtmp_server     = "rtmp://127.0.0.1/live";
    // stream key 模板。占位符：{meeting} = 会议号 / 被叫号码
    std::string main_key_tpl    = "recorder-{meeting}-main";
    std::string aux_key_tpl     = "recorder-{meeting}-aux";
    // 辅流（H.239 presentation）是否也推流。关掉即只推主会场。
    bool        push_aux        = true;
    // 队列上限。积压超过此值丢非关键帧，防内存爆；到 2×此值断链重连。
    int         queue_max       = 200;
    // RTMP 断链后重连间隔（毫秒）。
    int         reconnect_delay_ms = 2000;
};

struct AppConfig {
    GkConfig       gk;
    RecorderConfig recorder;
    OutgoingConfig outgoing;
    TcpConfig      tcp;
    StreamingConfig streaming;

    // 自动发送主流视频（screensaver 测试图案）。
    // true  → 任何通话建立后立即发送主流，模拟 TE 终端行为（同时收 + 发）。
    // false → 只录制，不主动发送主流（默认，不影响现有录播场景）。
    // 发出后仍然正常录制对方发来的主流。
    bool          auto_send_video = false;

    // H.243 terminalID — echoed back in terminalIDResponse to VP9660's
    // periodic enterH243TerminalID polls. Displayed in MCU conference
    // roster. Arbitrary UTF-8 string, up to 128 bytes.
    std::string   terminal_id = "TE录播设备";

    std::string   log_dir     = "/opt/recorder/logs";
    std::string   log_level   = "info";
};
