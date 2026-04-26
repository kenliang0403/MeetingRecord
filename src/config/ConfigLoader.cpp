#include "ConfigLoader.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

AppConfig loadConfig(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open config file: " + path);

    json j;
    f >> j;

    AppConfig cfg;

    // GK settings
    if (j.contains("gk")) {
        auto& g = j["gk"];
        cfg.gk.host      = g.value("host",      "");
        cfg.gk.port      = g.value("port",      1719);
        cfg.gk.alias     = g.value("alias",     "<recorder-alias>");
        cfg.gk.e164      = g.value("e164",      "");
        cfg.gk.ttl       = g.value("ttl",       60);
        cfg.gk.username  = g.value("username",  "");
        cfg.gk.password  = g.value("password",  "");
        cfg.gk.h323_port = g.value("h323_port", 1720);
    }

    // Recorder settings
    if (j.contains("recorder")) {
        auto& r = j["recorder"];
        cfg.recorder.output_dir        = r.value("output_dir",   "/opt/recorder/recordings");
        cfg.recorder.video_width       = r.value("video_width",  1920);
        cfg.recorder.video_height      = r.value("video_height", 1080);
        cfg.recorder.video_fps         = r.value("video_fps",    25);
        cfg.recorder.audio_sample_rate = r.value("audio_sample_rate", 8000);
        cfg.recorder.audio_channels    = r.value("audio_channels",    1);
        cfg.recorder.video_codec       = r.value("video_codec",  "libx264");
        cfg.recorder.audio_codec       = r.value("audio_codec",  "aac");
        cfg.recorder.video_bitrate     = r.value("video_bitrate", 1500000);
        cfg.recorder.audio_bitrate     = r.value("audio_bitrate", 64000);
        cfg.recorder.rtp_port_base     = r.value("rtp_port_base", 20000);
    }

    // Outgoing call settings
    if (j.contains("outgoing")) {
        auto& o = j["outgoing"];
        cfg.outgoing.enabled          = o.value("enabled",           false);
        cfg.outgoing.dial_number      = o.value("dial_number",       "");
        cfg.outgoing.mcu_host         = o.value("mcu_host",          "");
        cfg.outgoing.reconnect        = o.value("reconnect",         true);
        cfg.outgoing.reconnect_delay_s= o.value("reconnect_delay_s", 5);
        cfg.outgoing.max_reconnects   = o.value("max_reconnects",    -1);
    }

    // Streaming (SRS RTMP push)
    if (j.contains("streaming")) {
        auto& s = j["streaming"];
        cfg.streaming.enabled         = s.value("enabled",         false);
        cfg.streaming.rtmp_server     = s.value("rtmp_server",     std::string("rtmp://127.0.0.1/live"));
        cfg.streaming.main_key_tpl    = s.value("main_key_tpl",    std::string("recorder-{meeting}-main"));
        cfg.streaming.aux_key_tpl     = s.value("aux_key_tpl",     std::string("recorder-{meeting}-aux"));
        cfg.streaming.push_aux        = s.value("push_aux",        true);
        cfg.streaming.queue_max       = s.value("queue_max",       200);
        cfg.streaming.reconnect_delay_ms = s.value("reconnect_delay_ms", 2000);
    }

    // TCP control settings
    if (j.contains("tcp")) {
        auto& t = j["tcp"];
        cfg.tcp.bind_addr = t.value("bind_addr", "0.0.0.0");
        cfg.tcp.port      = t.value("port",       9001);
    }

    // 自动发送主流视频（模拟 TE 终端）
    cfg.auto_send_video = j.value("auto_send_video", false);

    // H.243 terminal ID (UTF-8 display name shown in MCU roster)
    cfg.terminal_id = j.value("terminal_id", std::string("TE录播设备"));

    // Logging
    cfg.log_dir   = j.value("log_dir",   "/opt/recorder/logs");
    cfg.log_level = j.value("log_level", "info");

    if (cfg.gk.host.empty())
        throw std::runtime_error("gk.host must be set in config");

    return cfg;
}
