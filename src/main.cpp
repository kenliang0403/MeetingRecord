// PTLib must be the very first include in all PTLib compilation units
#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <h323.h>

#include "config/ConfigLoader.h"
#include "h323/RecorderEndpoint.h"
#include "h323/RecorderConnection.h"
#include "meeting/MeetingRegistry.h"
#include "media/AudioLevelMeter.h"
#include "media/Mp4Faststart.h"
#include "tcp/ControlServer.h"
#include "util/Logger.h"
#include <h323pluginmgr.h>
#include <ptlib/pluginmgr.h>

#include <csignal>
#include <atomic>
#include <filesystem>
#include <thread>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};

static void signalHandler(int /*sig*/)
{
    g_running = false;
}

// ── PTLib application wrapper ──────────────────────────────────────────────
class RecorderApp : public PProcess
{
    PCLASSINFO(RecorderApp, PProcess);
public:
    RecorderApp()
        : PProcess("HF Recorder", "recorder-core",
                   1, 0, AlphaCode, 0)
    {}
    void Main() override;
};

PCREATE_PROCESS(RecorderApp)

void RecorderApp::Main()
{
    // Enable PTLib trace to see plugin loading
    PTrace::Initialise(4, "/opt/recorder/logs/ptlib.log", PTrace::Timestamp | PTrace::Thread | PTrace::FileAndLine);

    // Force instantiate the H323 Plugin Codec Manager
    static H323PluginCodecManager pluginMgr;

    // Explicitly load the directory
    PPluginManager::GetPluginManager().LoadPluginDirectory("/usr/local/lib/pwlib/codecs/audio");
    PPluginManager::GetPluginManager().LoadPluginDirectory("/usr/local/lib/opal-1.27.2/codecs/audio");

    PArgList& args = GetArguments();
    args.Parse("c:h");

    if (args.HasOption('h')) {
        PError << "Usage: recorder-core -c <config.json>\n";
        return;
    }

    std::string configPath = "/opt/recorder/config/config.json";
    if (args.HasOption('c'))
        configPath = std::string(args.GetOptionString('c').GetPointer());

    // Load config
    AppConfig cfg;
    try {
        cfg = loadConfig(configPath);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << "\n";
        return;
    }

    // Init logging
    fs::create_directories(cfg.log_dir);
    initLogger(cfg.log_dir, cfg.log_level);
    spdlog::info("recorder-core v1.0 — config={}", configPath);

    // Enable H323Plus trace log (level 4 = enough to see RTP receive events)
    PTrace::Initialise(4, (cfg.log_dir + "/h323trace.log").c_str(),
                       PTrace::DateAndTime | PTrace::TraceLevel | PTrace::Thread);
    spdlog::info("GK: {}:{} alias={}", cfg.gk.host, cfg.gk.port, cfg.gk.alias);

    // Ensure recordings dir exists
    fs::create_directories(cfg.recorder.output_dir);

    // Signals
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    // TCP control server
    ControlServer ctrlServer(cfg.tcp.bind_addr, cfg.tcp.port);

    // H.323 endpoint
    RecorderEndpoint endpoint(cfg);  // NOLINT: mutable for currentConnection()

    // ── status: full runtime state ──────────────────────────────────────────
    ctrlServer.registerHandler("status", [&](const nlohmann::json&) {
        auto* conn = endpoint.currentConnection();
        nlohmann::json data = {
            // Static config
            {"alias",       cfg.gk.alias},
            {"gk_host",     cfg.gk.host},
            {"gk_port",     cfg.gk.port},
            {"output_dir",  cfg.recorder.output_dir},
            {"e164",        cfg.gk.e164},
            // Runtime state
            {"gk_registered",   endpoint.isRegistered()},
            {"in_call",         endpoint.isInCall()},
            {"call_token",      endpoint.currentTokenStr()},
            {"reconnect_count", endpoint.reconnectCount()}
        };

        if (conn) {
            data["meeting_name"]  = conn->meetingName();
            data["caller_id"]     = conn->callerId();
            data["recording"]     = conn->isRecording();
            data["main_file"]     = conn->mainFilePath();
            data["main_sending"]     = conn->isMainSending();
            data["h239_received"]    = conn->hasH239();
            data["has_presentation"] = conn->hasPresentation();
            data["aux_recording"]    = conn->isAuxRecording();
            data["aux_file"]         = conn->auxFilePath();
            auto mtg = conn->meetingCtx();
            data["meeting_id"]    = mtg ? mtg->meetingId() : "";
            data["meeting_dir"]   = mtg ? mtg->dirPath()   : "";
            data["connection_idx"] = conn->connectionIdx();
        } else {
            data["meeting_name"]  = "";
            data["caller_id"]     = "";
            data["recording"]     = false;
            data["main_file"]     = "";
            data["main_sending"]  = false;
            data["h239_received"] = false;
            data["aux_recording"] = false;
            data["aux_file"]      = "";
            data["meeting_id"]    = "";
            data["meeting_dir"]   = "";
            data["connection_idx"] = 0;
        }

        return nlohmann::json{{"ok", true}, {"data", data}};
    });

    // ── audio_levels: realtime VU meter values for web admin ────────────────
    // 返回当前主流 audio capture 的瞬时电平。Web 管理页 ~10Hz 拉取，绘制 VU 表。
    // peak: 瞬时峰值（快速跳）；rms: 指数平滑后的平均（慢速移动）；单位 dBFS [-120, 0]
    // age_ms: 距上次喂样的毫秒数；> 500 表示静默/无通话（值已被衰减到 -120）
    ctrlServer.registerHandler("audio_levels", [&](const nlohmann::json&) {
        auto s = AudioLevelMeter::instance().snapshot();
        return nlohmann::json{
            {"ok", true},
            {"data", {
                {"peak_dbfs", s.peak_dbfs},
                {"rms_dbfs",  s.rms_dbfs},
                {"age_ms",    s.age_ms}
            }}
        };
    });

    // ── config: return current full config ──────────────────────────────────
    ctrlServer.registerHandler("config", [&](const nlohmann::json&) {
        nlohmann::json data = {
            {"gk", {
                {"host",     cfg.gk.host},
                {"port",     cfg.gk.port},
                {"alias",    cfg.gk.alias},
                {"e164",     cfg.gk.e164},
                {"ttl",      cfg.gk.ttl},
                {"username", cfg.gk.username},
                {"password", cfg.gk.password.empty() ? "" : "***"}
            }},
            {"recorder", {
                {"output_dir",        cfg.recorder.output_dir},
                {"video_width",       cfg.recorder.video_width},
                {"video_height",      cfg.recorder.video_height},
                {"video_fps",         cfg.recorder.video_fps},
                {"audio_sample_rate", cfg.recorder.audio_sample_rate},
                {"audio_channels",    cfg.recorder.audio_channels},
                {"video_codec",       cfg.recorder.video_codec},
                {"audio_codec",       cfg.recorder.audio_codec},
                {"video_bitrate",     cfg.recorder.video_bitrate},
                {"audio_bitrate",     cfg.recorder.audio_bitrate},
                {"audio_gain",        cfg.recorder.audio_gain},
                {"rtp_port_base",     cfg.recorder.rtp_port_base}
            }},
            {"streaming", {
                {"enabled",            cfg.streaming.enabled},
                {"rtmp_server",        cfg.streaming.rtmp_server},
                {"main_key_tpl",       cfg.streaming.main_key_tpl},
                {"aux_key_tpl",        cfg.streaming.aux_key_tpl},
                {"push_aux",           cfg.streaming.push_aux}
            }},
            {"outgoing", {
                {"enabled",           cfg.outgoing.enabled},
                {"dial_number",       cfg.outgoing.dial_number},
                {"mcu_host",          cfg.outgoing.mcu_host},
                {"reconnect",         cfg.outgoing.reconnect},
                {"reconnect_delay_s", cfg.outgoing.reconnect_delay_s},
                {"max_reconnects",    cfg.outgoing.max_reconnects}
            }},
            {"tcp", {
                {"bind_addr", cfg.tcp.bind_addr},
                {"port",      cfg.tcp.port}
            }},
            {"auto_send_video", cfg.auto_send_video},
            {"log_dir",   cfg.log_dir},
            {"log_level", cfg.log_level}
        };
        return nlohmann::json{{"ok", true}, {"data", data}};
    });

    // ── reload: re-read config.json (GK change requires restart) ────────────
    ctrlServer.registerHandler("reload", [&](const nlohmann::json&) {
        try {
            auto newCfg = loadConfig(configPath);
            // Only update safe fields that don't need H.323 restart
            // Recorder params: can apply to next recording
            // GK params: logged but requires restart for actual re-registration
            // Outgoing params: can apply to next dial attempt
            bool gkChanged = (newCfg.gk.host     != cfg.gk.host   ||
                              newCfg.gk.port     != cfg.gk.port   ||
                              newCfg.gk.alias    != cfg.gk.alias  ||
                              newCfg.gk.e164     != cfg.gk.e164   ||
                              newCfg.gk.username != cfg.gk.username);
            cfg = newCfg;
            spdlog::info("config reloaded from {}", configPath);
            nlohmann::json resp = {{"ok", true}, {"message", "config reloaded"}};
            if (gkChanged) {
                resp["warning"] = "GK parameters changed — restart required to re-register";
            }
            return resp;
        } catch (const std::exception& e) {
            return nlohmann::json{{"ok", false}, {"error", std::string("reload failed: ") + e.what()}};
        }
    });

    // ── set: change a single config key at runtime ─────────────────────────
    // Usage: {"cmd":"set","key":"gk.host","value":"<gk_host>"}
    //        {"cmd":"set","key":"outgoing.dial_number","value":"<dial-number>"}
    //        {"cmd":"set","key":"log_level","value":"debug"}
    ctrlServer.registerHandler("set", [&](const nlohmann::json& req) {
        std::string key   = req.value("key",   "");
        std::string value = req.value("value", "");

        if (key.empty())
            return nlohmann::json{{"ok", false}, {"error", "key is required"}};
        if (value.empty())
            return nlohmann::json{{"ok", false}, {"error", "value is required"}};

        // Supported dot-notation keys
        if (key == "gk.host") {
            cfg.gk.host = value;
            return nlohmann::json{{"ok", true}, {"message", "gk.host updated — restart required to re-register"}};
        }
        if (key == "gk.port") {
            cfg.gk.port = std::stoi(value);
            return nlohmann::json{{"ok", true}, {"message", "gk.port updated — restart required to re-register"}};
        }
        if (key == "gk.alias") {
            cfg.gk.alias = value;
            return nlohmann::json{{"ok", true}, {"message", "gk.alias updated — restart required to re-register"}};
        }
        if (key == "gk.e164") {
            cfg.gk.e164 = value;
            return nlohmann::json{{"ok", true}, {"message", "gk.e164 updated — restart required to re-register"}};
        }
        if (key == "gk.password") {
            cfg.gk.password = value;
            return nlohmann::json{{"ok", true}, {"message", "gk.password updated — restart required to re-register"}};
        }
        if (key == "outgoing.dial_number") {
            cfg.outgoing.dial_number = value;
            return nlohmann::json{{"ok", true}, {"message", "outgoing.dial_number updated"}};
        }
        if (key == "outgoing.mcu_host") {
            cfg.outgoing.mcu_host = value;
            return nlohmann::json{{"ok", true}, {"message", "outgoing.mcu_host updated"}};
        }
        if (key == "outgoing.enabled") {
            cfg.outgoing.enabled = (value == "true" || value == "1");
            return nlohmann::json{{"ok", true}, {"message", "outgoing.enabled updated"}};
        }
        if (key == "outgoing.reconnect") {
            cfg.outgoing.reconnect = (value == "true" || value == "1");
            return nlohmann::json{{"ok", true}, {"message", "outgoing.reconnect updated"}};
        }
        if (key == "log_level") {
            cfg.log_level = value;
            // Re-init logger at new level
            initLogger(cfg.log_dir, cfg.log_level);
            return nlohmann::json{{"ok", true}, {"message", "log_level updated"}};
        }
        if (key == "recorder.output_dir") {
            cfg.recorder.output_dir = value;
            fs::create_directories(value);
            return nlohmann::json{{"ok", true}, {"message", "recorder.output_dir updated"}};
        }
        if (key == "auto_send_video") {
            cfg.auto_send_video = (value == "true" || value == "1");
            return nlohmann::json{{"ok", true}, {"message", "auto_send_video updated (takes effect on next call)"}};
        }

        return nlohmann::json{{"ok", false}, {"error", "unsupported key: " + key}};
    });

    // ── recordings: list active recording files ────────────────────────────
    ctrlServer.registerHandler("recordings", [&](const nlohmann::json&) {
        nlohmann::json recordings = nlohmann::json::array();

        // List all .mp4 files in output_dir sorted by modification time
        std::vector<std::pair<std::string, int64_t>> files;
        try {
            for (const auto& entry : fs::directory_iterator(cfg.recorder.output_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
                    auto ftime = entry.last_write_time();
                    auto sctp  = std::chrono::time_point_cast<std::chrono::seconds>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                    files.push_back({entry.path().filename().string(),
                                     std::chrono::system_clock::to_time_t(sctp)});
                }
            }
        } catch (...) {}

        // Sort by time descending (newest first)
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        for (const auto& [name, mtime] : files) {
            std::string fullPath = cfg.recorder.output_dir + "/" + name;
            try {
                auto size = fs::file_size(fullPath);
                recordings.push_back({
                    {"name",  name},
                    {"size",  (int64_t)size},
                    {"mtime", mtime}
                });
            } catch (...) {}
        }

        auto* conn = endpoint.currentConnection();
        nlohmann::json data = {
            {"output_dir",     cfg.recorder.output_dir},
            {"total_files",    recordings.size()},
            {"recording_now",  conn ? conn->isRecording() : false},
            {"aux_recording",  conn ? conn->isAuxRecording() : false},
            {"files",          recordings}
        };
        return nlohmann::json{{"ok", true}, {"data", data}};
    });

    // ── version: return version info ────────────────────────────────────────
    ctrlServer.registerHandler("version", [&](const nlohmann::json&) {
        return nlohmann::json{{"ok", true}, {"data", {
            {"name",    "recorder-core"},
            {"version", "3.1.0"},
            {"config",  configPath}
        }}};
    });

    // ── clear_call (existing) ──────────────────────────────────────────────
    ctrlServer.registerHandler("clear_call", [&](const nlohmann::json& req) {
        std::string token = req.value("token", "");
        if (!token.empty())
            endpoint.ClearCall(PString(token.c_str()), H323Connection::EndedByLocalUser);
        else
            endpoint.ClearAllCalls(H323Connection::EndedByLocalUser, FALSE);
        return nlohmann::json{{"ok", true}};
    });

    // ── dial (existing) ────────────────────────────────────────────────────
    ctrlServer.registerHandler("dial", [&](const nlohmann::json& req) {
        std::string number = req.value("number", "");
        std::string host   = req.value("host",   "");
        if (number.empty())
            return nlohmann::json{{"ok", false}, {"error", "number is required"}};
        std::string token = endpoint.dialTo(number, host);
        if (token.empty())
            return nlohmann::json{{"ok", false}, {"error", "MakeCall failed — check GK registration and number"}};
        return nlohmann::json{{"ok", true}, {"token", token}};
    });

    // ── start_main_video: 手动启动主流屏保发送（外呼时自动启动，此命令用于手动覆盖）
    // Usage: {"cmd":"start_main_video"}
    ctrlServer.registerHandler("start_main_video", [&](const nlohmann::json&) {
        if (!endpoint.isInCall())
            return nlohmann::json{{"ok", false}, {"error", "not in call"}};
        bool ok = endpoint.startMainVideo();
        return nlohmann::json{{"ok", ok},
            {"message", ok ? "main video started (screensaver)" : "start failed — check logs"}};
    });

    // ── stop_main_video: 停止主流发送
    // Usage: {"cmd":"stop_main_video"}
    ctrlServer.registerHandler("stop_main_video", [&](const nlohmann::json&) {
        bool ok = endpoint.stopMainVideo();
        return nlohmann::json{{"ok", ok},
            {"message", ok ? "main video stopped" : "stop failed — check logs"}};
    });

    // ── start_presentation: send H.239 OLC + start VideoSender(NoSignal) aux stream
    // Usage: {"cmd":"start_presentation"}
    ctrlServer.registerHandler("start_presentation", [&](const nlohmann::json&) {
        if (!endpoint.isInCall())
            return nlohmann::json{{"ok", false}, {"error", "not in call"}};
        bool ok = endpoint.startPresentation();
        return nlohmann::json{{"ok", ok},
            {"message", ok ? "H.239 presentation started" : "start failed — check logs"}};
    });

    // ── stop_presentation: stop VideoSender + send CLC + release token
    // Usage: {"cmd":"stop_presentation"}
    ctrlServer.registerHandler("stop_presentation", [&](const nlohmann::json&) {
        bool ok = endpoint.stopPresentation();
        return nlohmann::json{{"ok", ok},
            {"message", ok ? "H.239 presentation stopped" : "stop failed — check logs"}};
    });

    // ── faststart_one: 把单个 mp4 文件原地重写为 +faststart ────────────────
    // Usage: {"cmd":"faststart_one","path":"/opt/recorder/recordings/<m>/main_01.mp4"}
    // 同步执行，返回 ok/error。用于 web 单点修复或脚本调用。
    ctrlServer.registerHandler("faststart_one", [&](const nlohmann::json& req) {
        std::string p = req.value("path", "");
        if (p.empty())
            return nlohmann::json{{"ok", false}, {"error", "path is required"}};
        // 安全：路径必须在 recordings 目录内
        std::string base = cfg.recorder.output_dir;
        if (p.find(base) != 0 || p.find("..") != std::string::npos)
            return nlohmann::json{{"ok", false}, {"error", "path outside recordings dir"}};
        bool ok = faststartRewrite(p);
        return nlohmann::json{{"ok", ok}, {"path", p}};
    });

    // ── faststart_all: 后台扫 recordings/，所有 main_*.mp4 / aux_*.mp4
    // 串行重写。立即返回，处理在 detached thread 中进行，结果只在 log 看。
    // Usage: {"cmd":"faststart_all"}
    ctrlServer.registerHandler("faststart_all", [&](const nlohmann::json&) {
        std::string base = cfg.recorder.output_dir;
        std::thread([base]() {
            namespace fs = std::filesystem;
            int total = 0, ok_count = 0;
            std::error_code ec;
            for (auto& entry : fs::recursive_directory_iterator(base, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                auto p = entry.path();
                if (p.extension() != ".mp4") continue;
                auto name = p.filename().string();
                if (name.find(".faststart.tmp") != std::string::npos) continue;
                // 仅处理 main_/aux_ 命名的录像段；跳过散落历史文件
                if (name.rfind("main_", 0) != 0 && name.rfind("aux_", 0) != 0)
                    continue;
                ++total;
                if (faststartRewrite(p.string())) ++ok_count;
            }
            spdlog::info("faststart_all: done {}/{} ok", ok_count, total);
        }).detach();
        return nlohmann::json{{"ok", true},
            {"message", "started; tail logs to watch progress"}};
    });

    if (!ctrlServer.start()) {
        spdlog::error("TCP control server failed to start on port {}", cfg.tcp.port);
        return;
    }
    if (!endpoint.start()) {
        spdlog::error("H.323 endpoint failed to start");
        return;
    }

    spdlog::info("recorder-core ready — waiting for calls on TCP:{}", cfg.gk.h323_port);
    spdlog::info("TCP control on :{} — Ctrl+C to exit", cfg.tcp.port);

    while (g_running)
        PProcess::Sleep(500);

    spdlog::info("Shutting down...");
    endpoint.stop();
    ctrlServer.stop();
    spdlog::info("recorder-core stopped.");
}
