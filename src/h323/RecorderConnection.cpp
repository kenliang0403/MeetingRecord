// PTLib must be first
#include <ptlib.h>
#include <h323.h>
#include <h323pdu.h>    // H323ControlPDU / WriteControlPDU
#include <h245.h>       // H245_OpenLogicalChannel etc.
#include <rtp.h>        // RTP_Session (for port logging)
#include <h323caps.h>   // H323_H264Cap

#include "RecorderConnection.h"
#include "RecorderEndpoint.h"
#include "H264RecvCodec.h"
#include "AACLDRecvCap.h"
#include "../media/FfmpegRecorder.h"
#include "../media/SrsStreamer.h"
#include "../media/VideoSender.h"
#include "../meeting/MeetingRegistry.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <ctime>

// POSIX socket headers (Linux only — this file is server-side only)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ptlib/ipsock.h>

namespace {
// Build an RTMP URL from template + meeting key. Template may contain
// {meeting} placeholder; unreplaced placeholders are left alone so the
// SRS side fails loudly rather than silently streaming to a weird path.
std::string BuildRtmpUrl(const std::string& server,
                         const std::string& keyTpl,
                         const std::string& meetingKey)
{
    std::string key = keyTpl;
    const std::string token = "{meeting}";
    size_t pos = 0;
    while ((pos = key.find(token, pos)) != std::string::npos) {
        key.replace(pos, token.size(), meetingKey);
        pos += meetingKey.size();
    }
    // Sanitize key: RTMP stream names shouldn't have slashes / whitespace.
    for (char& c : key) {
        if (c == '/' || c == ' ' || c == '\t') c = '_';
    }
    std::string url = server;
    if (!url.empty() && url.back() != '/') url += '/';
    url += key;
    return url;
}
}

namespace {
inline int64_t NowWallMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

namespace {
bool MatchOid(const H245_CapabilityIdentifier& id, const char* value)
{
    if (id.GetTag() != H245_CapabilityIdentifier::e_standard)
        return false;
    const PASN_ObjectId& oid = (const PASN_ObjectId&)(const PASN_ObjectId&)id;
    return oid == value;
}

bool MatchPayloadOid(const H245_H2250LogicalChannelParameters& params, const char* value)
{
    if (!params.HasOptionalField(H245_H2250LogicalChannelParameters::e_mediaPacketization))
        return false;
    if (params.m_mediaPacketization.GetTag() != H245_H2250LogicalChannelParameters_mediaPacketization::e_rtpPayloadType)
        return false;
    const H245_RTPPayloadType& payload =
        (const H245_RTPPayloadType&)(const H245_RTPPayloadType&)params.m_mediaPacketization;
    if (payload.m_payloadDescriptor.GetTag() != H245_RTPPayloadType_payloadDescriptor::e_oid)
        return false;
    const PASN_ObjectId& oid =
        (const PASN_ObjectId&)(const PASN_ObjectId&)payload.m_payloadDescriptor;
    return oid == value;
}

bool IsH264VideoOlc(const H245_OpenLogicalChannel& open)
{
    const auto& fwd = open.m_forwardLogicalChannelParameters;
    if (fwd.m_dataType.GetTag() != H245_DataType::e_videoData)
        return false;
        
    const H245_VideoCapability& vc = fwd.m_dataType;
    
    if (vc.GetTag() == H245_VideoCapability::e_genericVideoCapability) {
        const H245_GenericCapability& gen =
            (const H245_GenericCapability&)(const H245_GenericCapability&)vc;
        return MatchOid(gen.m_capabilityIdentifier, "0.0.8.241.0.0.1");
    } 
    else if (vc.GetTag() == H245_VideoCapability::e_extendedVideoCapability) {
        const H245_ExtendedVideoCapability& ext = 
            (const H245_ExtendedVideoCapability&)(const H245_ExtendedVideoCapability&)vc;
        if (ext.m_videoCapability.GetSize() > 0) {
            const H245_VideoCapability& innerVc = ext.m_videoCapability[0];
            if (innerVc.GetTag() == H245_VideoCapability::e_genericVideoCapability) {
                const H245_GenericCapability& gen =
                    (const H245_GenericCapability&)(const H245_GenericCapability&)innerVc;
                return MatchOid(gen.m_capabilityIdentifier, "0.0.8.241.0.0.1");
            }
        }
    }
    
    return false;
}

bool IsAACAudioOlc(const H245_OpenLogicalChannel& open)
{
    const auto& fwd = open.m_forwardLogicalChannelParameters;
    if (fwd.m_dataType.GetTag() != H245_DataType::e_audioData)
        return false;
    const H245_AudioCapability& ac = fwd.m_dataType;
    if (ac.GetTag() != H245_AudioCapability::e_genericAudioCapability)
        return false;
    const H245_GenericCapability& gen =
        (const H245_GenericCapability&)(const H245_GenericCapability&)ac;
    if (!MatchOid(gen.m_capabilityIdentifier, "0.0.8.245.1.1.0") &&
        !MatchOid(gen.m_capabilityIdentifier, "0.0.8.245.1.1.11"))
        return false;
    return true;
}

bool IsG722AudioOlc(const H245_OpenLogicalChannel& open)
{
    const auto& fwd = open.m_forwardLogicalChannelParameters;
    if (fwd.m_dataType.GetTag() != H245_DataType::e_audioData)
        return false;
    const H245_AudioCapability& ac = fwd.m_dataType;
    // e_g722_64k is typically tag 5 in AudioCapability
    return ac.GetTag() == H245_AudioCapability::e_g722_64k ||
           ac.GetTag() == H245_AudioCapability::e_g722_56k ||
           ac.GetTag() == H245_AudioCapability::e_g722_48k;
}
}

RecorderConnection::RecorderConnection(unsigned callReference,
                                       RecorderEndpoint& ep,
                                       unsigned options)
    : H323Connection(ep, callReference, options)
    , ep_(ep)
{
    const auto& cfg = ep_.config();
    recorder_ = std::make_shared<FfmpegRecorder>(cfg.recorder);
    auxRecorder_ = std::make_shared<FfmpegRecorder>(cfg.recorder);
}

RecorderConnection::~RecorderConnection()
{
    if (recorder_ && recorder_->isOpen())
        recorder_->close();
    if (auxRecorder_ && auxRecorder_->isOpen())
        auxRecorder_->close();
}

void RecorderConnection::OnEstablished()
{
    H323Connection::OnEstablished();

    const auto& cfg = ep_.config().recorder;

    // ── Meeting folder bootstrap ─────────────────────────────────────────
    // callerId_ was populated by OnAnswerCall (incoming) or dial/dialTo
    // (outgoing). If still empty, fall back to a synthetic id so we at
    // least have a folder — but log a warning because JSON will be hard
    // to correlate.
    int64_t nowMs = NowWallMs();
    std::string callerForKey = callerId_.empty() ? std::string("unknown")
                                                 : callerId_;
    if (callerId_.empty()) {
        spdlog::warn("RecorderConnection: caller_id empty at OnEstablished — "
                     "using 'unknown' as meeting key");
    }

    meeting_       = ep_.meetingRegistry().openOrJoin(callerForKey, nowMs);
    connectionIdx_ = ep_.nextConnectionIdx();
    activeMainPath_ = meeting_->allocMainPath(connectionIdx_);

    if (recorder_->open(activeMainPath_,
                        cfg.video_width, cfg.video_height, cfg.video_fps,
                        cfg.audio_sample_rate, cfg.audio_channels)) {
        meeting_->recordSegmentStart(SegmentType::Main, activeMainPath_,
                                     connectionIdx_, nowMs);
        ep_.meetingRegistry().touchIndex(meeting_, nowMs);
        ep_.onRecordingStarted(GetCallToken(), activeMainPath_);

        // ── Start main RTMP push ────────────────────────────────────────
        const auto& scfg = ep_.config().streaming;
        if (scfg.enabled) {
            SrsStreamer::Config sc;
            sc.rtmp_url = BuildRtmpUrl(scfg.rtmp_server, scfg.main_key_tpl,
                                       callerForKey);
            sc.queue_max = scfg.queue_max;
            sc.reconnect_delay_ms = scfg.reconnect_delay_ms;
            mainStreamer_ = std::make_shared<SrsStreamer>(sc);
            mainStreamer_->start();
            recorder_->attachStreamer(mainStreamer_);
            spdlog::info("RecorderConnection: main RTMP push → {}", sc.rtmp_url);
        }
    } else {
        spdlog::error("RecorderConnection: failed to open recorder {}",
                      activeMainPath_);
    }

    // H.239 迟加入订阅：通话建立后等待 5s，如果 MCU 没主动推 H.239 OLC，就发送
    // raw OLC(extendedVideoCapability, session=10) 提示 MCU 把现有的演示流分发给我们。
    // 已收到 MCU OLC（h239Received_）或被拒（h239Rejected_）后定时器自动停止。
    capRefreshRetries_ = 0;
    h239Received_ = false;
    h239Rejected_ = false;
    capRefreshTimer_.SetNotifier(PCREATE_NOTIFIER(OnCapRefreshTimer));
    capRefreshTimer_.SetInterval(0, 5);

    // ── 主流视频自动发送 ──────────────────────────────────────────────────────
    // auto_send_video=true  → 任何通话建立后自动发送主流（模拟 TE 终端）
    // auto_send_video=false → 只录制，不主动发送（默认）
    // 也兼容旧的 outgoing.enabled 触发（外呼时也自动发）
    if (ep_.config().auto_send_video || ep_.config().outgoing.enabled) {
        spdlog::info("RecorderConnection: auto-starting main video send (ScreenSaver)");
        PProcess::Sleep(500);   // 等 H.245 完全就绪
        startMainVideo();
    }
}

void RecorderConnection::OnCleared()
{
    int64_t nowMs = NowWallMs();

    // Finalize aux first (if still open — e.g. call dropped without CLC)
    if (auxRecorder_ && auxRecorder_->isOpen()) {
        auxRecorder_->close();
        if (meeting_ && !activeAuxPath_.empty()) {
            meeting_->recordSegmentEnd(activeAuxPath_, nowMs);
            activeAuxPath_.clear();
        }
    }

    // Then finalize main
    if (recorder_ && recorder_->isOpen()) {
        recorder_->close();
        if (meeting_ && !activeMainPath_.empty()) {
            meeting_->recordSegmentEnd(activeMainPath_, nowMs);
        }
        ep_.onRecordingStopped(GetCallToken());
    }

    // Stop video senders
    if (mainSender_) { mainSender_->stop(); mainSender_.reset(); }
    mainSendActive_ = false;
    if (h239Sender_) { h239Sender_->stop(); h239Sender_.reset(); }
    h239SendActive_ = false;

    // Stop RTMP streamers. Detach from recorders first so no further
    // pushes can queue packets after stop() begins.
    if (recorder_)    recorder_->attachStreamer(nullptr);
    if (auxRecorder_) auxRecorder_->attachStreamer(nullptr);
    if (mainStreamer_) {
        mainStreamer_->stop();
        mainStreamer_.reset();
    }
    if (auxStreamer_) {
        auxStreamer_->stop();
        auxStreamer_.reset();
    }

    if (meeting_) {
        ep_.meetingRegistry().touchIndex(meeting_, nowMs);
    }

    H323Connection::OnCleared();
}

// Allocate & remember the next aux path — called by the endpoint's
// OpenExtendedVideoChannel right before it opens auxRecorder_.
std::string RecorderConnection::allocateAuxPath()
{
    if (!meeting_) {
        spdlog::warn("RecorderConnection::allocateAuxPath — no meeting context");
        return {};
    }
    activeAuxPath_ = meeting_->allocAuxPath(connectionIdx_);
    return activeAuxPath_;
}

void RecorderConnection::recordAuxStart(int64_t wallStartMs)
{
    if (!meeting_ || activeAuxPath_.empty()) return;
    meeting_->recordSegmentStart(SegmentType::Aux, activeAuxPath_,
                                 connectionIdx_, wallStartMs);
    ep_.meetingRegistry().touchIndex(meeting_, wallStartMs);

    // ── Start aux RTMP push (lazy, only on first aux segment) ──────────
    // The streamer is kept alive across presenter-stop / next-presenter-start
    // so viewers see a continuous stream URL. It is torn down in OnCleared.
    const auto& scfg = ep_.config().streaming;
    if (scfg.enabled && scfg.push_aux && !auxStreamer_) {
        std::string meetingKey = callerId_.empty() ? std::string("unknown")
                                                   : callerId_;
        SrsStreamer::Config sc;
        sc.rtmp_url = BuildRtmpUrl(scfg.rtmp_server, scfg.aux_key_tpl,
                                   meetingKey);
        sc.queue_max = scfg.queue_max;
        sc.reconnect_delay_ms = scfg.reconnect_delay_ms;
        auxStreamer_ = std::make_shared<SrsStreamer>(sc);
        auxStreamer_->start();
        spdlog::info("RecorderConnection: aux RTMP push → {}", sc.rtmp_url);
    }
    if (auxStreamer_ && auxRecorder_) {
        auxRecorder_->attachStreamer(auxStreamer_);
    }
}

void RecorderConnection::closeActiveAux(int64_t wallEndMs)
{
    if (auxRecorder_ && auxRecorder_->isOpen()) {
        auxRecorder_->close();
    }
    if (meeting_ && !activeAuxPath_.empty()) {
        meeting_->recordSegmentEnd(activeAuxPath_, wallEndMs);
    }
    activeAuxPath_.clear();
}

// H.239 channel teardown detection: when the MCU sends CLC for session=10
// (presenter stopped sharing), finalize the aux segment immediately so the
// duration in meeting.json reflects the real presentation window rather
// than the full call length.
void RecorderConnection::OnClosedLogicalChannel(const H323Channel& channel)
{
    unsigned sessionID = channel.GetSessionID();
    spdlog::info("RecorderConnection: OnClosedLogicalChannel session={}", sessionID);

    // H.239 extended video uses session=10 (VP9660 convention).
    // Any non-primary session that we had tagged as aux should finalize.
    if (sessionID == 10 && auxRecorder_ && auxRecorder_->isOpen()) {
        spdlog::info("RecorderConnection: H.239 CLC → finalizing aux segment {}",
                     activeAuxPath_);
        closeActiveAux(NowWallMs());
    }

    H323Connection::OnClosedLogicalChannel(channel);
}

PBoolean RecorderConnection::OnCreateLogicalChannel(
    const H323Capability& capability,
    H323Channel::Directions dir,
    unsigned& errorCode)
{
    // For inbound (receive) channels accept anything the MCU offers.
    // The default implementation rejects video with
    // dataTypeALCombinationNotSupported when it cannot verify that the
    // audio+video caps are paired in the same capabilityDescriptor.
    // As a receive-only recorder we don't care about that constraint.
    if (dir == H323Channel::IsReceiver) {
        spdlog::info("RecorderConnection: accepting inbound OLC cap='{}'",
                     (const char*)capability.GetFormatName());
        return TRUE;
    }
    // For transmit channels use default logic (we never transmit anyway)
    return H323Connection::OnCreateLogicalChannel(capability, dir, errorCode);
}

H323Channel* RecorderConnection::CreateRealTimeLogicalChannel(
    const H323Capability& capability,
    H323Channel::Directions dir,
    unsigned sessionID,
    const H245_H2250LogicalChannelParameters* param,
    RTP_QOS* rtpqos)
{
    spdlog::info("RecorderConnection: CreateRealTimeLogicalChannel cap='{}' dir={} session={}",
                 (const char*)capability.GetFormatName(), (int)dir, sessionID);

    if (param == nullptr) {
        spdlog::warn("RecorderConnection: CreateRealTimeLogicalChannel param is NULL");
    } else {
        spdlog::info("RecorderConnection: param hasMediaControlChannel={}",
                     param->HasOptionalField(H245_H2250LogicalChannelParameters::e_mediaControlChannel) ? "yes" : "no");
    }

    H323Channel* ch = H323Connection::CreateRealTimeLogicalChannel(
        capability, dir, sessionID, param, rtpqos);

    if (ch == nullptr) {
        spdlog::error("RecorderConnection: CreateRealTimeLogicalChannel returned NULL — "
                      "session creation failed (check sessionID={} and mediaControlChannel)", sessionID);
    } else {
        // Log the session ID and channel info
        unsigned chSessionID = ch->GetSessionID();
        spdlog::info("RecorderConnection: CreateRealTimeLogicalChannel OK → "
                     "channel sessionID={} requested sessionID={}",
                     chSessionID, sessionID);
    }
    return ch;
}

H323Channel* RecorderConnection::CreateLogicalChannel(
    const H245_OpenLogicalChannel& open,
    PBoolean startingFast,
    unsigned& errorCode)
{
    int sid = -1;
    if (open.m_forwardLogicalChannelParameters.m_multiplexParameters.GetTag() == 
        H245_OpenLogicalChannel_forwardLogicalChannelParameters_multiplexParameters::e_h2250LogicalChannelParameters) {
        const auto& h2250 = (const H245_H2250LogicalChannelParameters&)open.m_forwardLogicalChannelParameters.m_multiplexParameters;
        sid = (int)h2250.m_sessionID;
    }
    spdlog::info("RecorderConnection: CreateLogicalChannel called for OLC (session={})", sid);

    H323Channel* channel = H323Connection::CreateLogicalChannel(open, startingFast, errorCode);
    if (channel != nullptr)
        return channel;

    bool isH264 = IsH264VideoOlc(open);
    bool isAAC  = IsAACAudioOlc(open);

    if (!isH264 && !isAAC)
        return nullptr;

    const auto& fwd = open.m_forwardLogicalChannelParameters;
    if (fwd.m_multiplexParameters.GetTag() !=
        H245_OpenLogicalChannel_forwardLogicalChannelParameters_multiplexParameters::e_h2250LogicalChannelParameters)
        return nullptr;

    const H245_H2250LogicalChannelParameters& h2250 =
        (const H245_H2250LogicalChannelParameters&)(const H245_H2250LogicalChannelParameters&)
            fwd.m_multiplexParameters;

    if (isH264) {
        // Detect whether this OLC carries extendedVideoCapability (H.239 presentation stream).
        // VP9660 uses session=10 for H.239 — do NOT rely on session ID==3.
        // Instead, check the actual dataType tag in the OLC.
        bool isExtendedVideo =
            (fwd.m_dataType.GetTag() == H245_DataType::e_videoData) &&
            (((const H245_VideoCapability&)fwd.m_dataType).GetTag() ==
             H245_VideoCapability::e_extendedVideoCapability);

        if (isExtendedVideo) {
            h239Received_ = true;
            spdlog::info("RecorderConnection: H.239 OLC received, suppressing further TCS refresh");
            spdlog::warn("RecorderConnection: fallback-create H.239 extended-video RTP channel"
                         " (extendedVideoCapability OLC, session={})", (int)h2250.m_sessionID);
            H323CodecExtendedVideoCapability fallbackCap;
            fallbackCap.AddCapability(new H323_H264RecvCap(8)); // High profile
            channel = CreateRealTimeLogicalChannel(fallbackCap,
                                                   H323Channel::IsReceiver,
                                                   h2250.m_sessionID,
                                                   &h2250,
                                                   nullptr);

            // ── Trigger our outbound H.239 OLC if startPresentation() was called ──
            // VP9660 flow: we send tokenRequest → MCU sends us this OLC (granting
            // the session) → we MUST send our own OLC back to actually stream.
            if (h239Sender_ && !h239SendActive_.load() && !h239TokenGranted_.load()) {
                spdlog::info("RecorderConnection: MCU OLC(session=10) received — "
                             "triggering our H.239 send OLC");
                h239TokenGranted_ = true;
                sendH239OLC();
            }
        } else {
            spdlog::warn("RecorderConnection: fallback-create H.264 RTP channel"
                         " (genericVideoCapability OLC, session={})", (int)h2250.m_sessionID);
            H323_H264RecvCap fallbackCap(8); // High profile
            channel = CreateRealTimeLogicalChannel(fallbackCap,
                                                   H323Channel::IsReceiver,
                                                   h2250.m_sessionID,
                                                   &h2250,
                                                   nullptr);
        }
                                               
        // Ensure PTLib accepts this payload type and returns it in Ack
        if (channel != nullptr) {
            // The MCU often expects PT 106 or 108. Since we can't extract it easily from generic,
            // we let the channel accept it.
            channel->SetDynamicRTPPayloadType(106); 
            spdlog::info("RecorderConnection: Forced H264 fallback channel payload type to 106");
        }
    } else if (isAAC) {
        spdlog::warn("RecorderConnection: fallback-create AAC-LD RTP channel for genericAudioCapability OLC");
        H323_AACLD_RecvCap fallbackCap;
        channel = CreateRealTimeLogicalChannel(fallbackCap,
                                               H323Channel::IsReceiver,
                                               h2250.m_sessionID,
                                               &h2250,
                                               nullptr);
    }

    if (channel != nullptr)
        errorCode = 0;
    return channel;
}

// ── H.245 conference message handling ───────────────────────────────���────────
//
// VP9660 periodically sends "conferenceRequest enterH243TerminalID" to every
// terminal in the conference (every ~15–20 s).  This is how the MCU assigns
// floor-control labels (H.243 terminal numbers).  H.323Plus does not implement
// this PDU natively and falls back to sending "functionNotUnderstood".
//
// When that happens, VP9660 may NOT add our endpoint to its H.239 viewer
// distribution list for the current presentation session.  Responding with a
// proper "conferenceResponse terminalIDResponse" registers us as a full
// conference participant so VP9660 will send the H.239 OLC to us even when we
// join after the presentation has already started.
PBoolean RecorderConnection::OnHandleConferenceRequest(const H245_ConferenceRequest& req)
{
    if (req.GetTag() != H245_ConferenceRequest::e_enterH243TerminalID) {
        return FALSE;   // Let H.323Plus handle (or ignore) other conference requests
    }

    // enterH243TerminalID carries NO payload (the ASN.1 choice tag is bound to
    // NULL — confirmed by wire capture). Previous attempts to cast the request
    // to H245_TerminalLabel read uninitialized stack memory and produced
    // garbage values (e.g. mcu=2105376125). The correct behaviour — as seen
    // in Huawei TE terminal captures — is to reply with the label that was
    // previously *assigned* to us via conferenceIndication.terminalNumberAssign,
    // together with a human-readable terminalID OCTET STRING.
    const unsigned mcuNumber      = h243McuNumber_.load();
    const unsigned terminalNumber = h243TerminalNumber_.load();
    const std::string& termIdStr  = ep_.config().terminal_id;

    spdlog::info("RecorderConnection: recv enterH243TerminalID; reply with "
                 "mcu={} term={} id='{}'", mcuNumber, terminalNumber, termIdStr);

    // Build "conferenceResponse terminalIDResponse"
    H323ControlPDU pdu;
    H245_ResponseMessage& resp = pdu.Build(H245_ResponseMessage::e_conferenceResponse);

    H245_ConferenceResponse& confResp =
        (H245_ConferenceResponse&)(H245_ConferenceResponse&)resp;
    confResp.SetTag(H245_ConferenceResponse::e_terminalIDResponse);

    H245_ConferenceResponse_terminalIDResponse& termIDResp =
        (H245_ConferenceResponse_terminalIDResponse&)
        (H245_ConferenceResponse_terminalIDResponse&)confResp;

    // Echo back the MCU-assigned terminal label (or 0/0 if MCU never assigned).
    termIDResp.m_terminalLabel.m_mcuNumber      = mcuNumber;
    termIDResp.m_terminalLabel.m_terminalNumber = terminalNumber;

    // Terminal ID: UTF-8 byte string from config (e.g. "TE录播设备").
    // H245_TerminalID is a PASN_OctetString. DO NOT use the const char* ctor:
    // PTLib's PASN_OctetString::operator=(const char*) truncates at the first
    // non-printable byte (UTF-8 high bytes 0xE5, 0xE6 … get dropped), leaving
    // only the ASCII prefix "TE" on the wire. The raw-bytes API preserves all
    // 14 bytes verbatim — matching what Huawei TE sends (TE-h239.pcap).
    termIDResp.m_terminalID.SetValue(
        reinterpret_cast<const BYTE*>(termIdStr.data()),
        static_cast<PINDEX>(termIdStr.size()));

    WriteControlPDU(pdu);

    spdlog::info("RecorderConnection: sent terminalIDResponse id='{}' "
                 "mcu={} term={} ({} bytes)",
                 termIdStr, mcuNumber, terminalNumber,
                 static_cast<int>(termIdStr.size()));

    // 被动 H.239 模式：不在每次 terminalIDResponse 后触发 subscribe。
    // （原先会在此 arm 定时器发 raw OLC，让 MCU 始终显示"演示通道打开"，
    //  改为纯被动接收。）
    // capRefreshRetries_ = 0;
    // capRefreshTimer_.SetInterval(0, 1);

    return TRUE;   // Suppress the default functionNotUnderstood reply
}

// ── H.245 conferenceIndication handling ──────────────────────────────────────
//
// VP9660 (and any compliant H.243 MCU) sends a terminalNumberAssign
// indication shortly after H.245 session establishment to tell us which
// {mcuNumber, terminalNumber} we own in the conference. We cache those
// values and echo them back in every subsequent terminalIDResponse
// (see OnHandleConferenceRequest above). Returning FALSE lets H.323Plus
// continue its default indication dispatch (no side effects).
PBoolean RecorderConnection::OnHandleConferenceIndication(
    const H245_ConferenceIndication& ind)
{
    // Use the same non-const double C-cast pattern as OnHandleConferenceRequest
    // above — this invokes H.323Plus's choice→inner conversion operator, which
    // is the only way to access the PASN inner object without triggering the
    // "Invalid cast to non-descendant class" dynamic_cast assertion.
    H245_ConferenceIndication& mind = const_cast<H245_ConferenceIndication&>(ind);
    switch (mind.GetTag()) {
    case H245_ConferenceIndication::e_terminalNumberAssign: {
        const H245_TerminalLabel& label =
            (H245_TerminalLabel&)(H245_ConferenceIndication&)mind;
        h243McuNumber_.store(static_cast<unsigned>(label.m_mcuNumber));
        h243TerminalNumber_.store(static_cast<unsigned>(label.m_terminalNumber));
        spdlog::info("RecorderConnection: MCU assigned label mcu={} term={}",
                     static_cast<unsigned>(label.m_mcuNumber),
                     static_cast<unsigned>(label.m_terminalNumber));
        break;
    }
    default:
        // Other indications (seenByAll, requestForFloor, terminalLeftConference,
        // etc.) are ignored by the recorder; nothing to do.
        break;
    }
    return FALSE;   // Let the base class continue default dispatch
}

// ── H.245 request interceptor ────────────────────────────────────────────
// Inbound genericRequest from the MCU is unusual for the recorder; default
// dispatch is fine.  H.239 token grant arrives as a genericResponse and is
// handled in OnH245Response below.
PBoolean RecorderConnection::OnH245Request(const H323ControlPDU& pdu)
{
    return H323Connection::OnH245Request(pdu);
}

// ── H.245 response interceptor ──────────────────────────────────────────
//
// We send raw openLogicalChannel(extendedVideo, session=10) via
// WriteControlPDU(), bypassing H323Plus's internal channel tracking.
// When the MCU replies with openLogicalChannelAck or
// openLogicalChannelReject, H323Plus's default handler tries to look
// up the corresponding H323Channel object by forwardLogicalChannelNumber.
// Since we never created one, the lookup returns null and the subsequent
// PDU decode triggers "Invalid cast to non-descendant class" assertion
// (h245_1.cxx line 11180).
//
// By overriding OnH245Response, we intercept these Ack/Reject PDUs,
// log them, and return TRUE to prevent the default (crashing) handler
// from running.  Other response types are forwarded to the base class.
PBoolean RecorderConnection::OnH245Response(const H323ControlPDU& pdu)
{
    const H245_ResponseMessage& resp = pdu;
    unsigned tag = resp.GetTag();

    // ── H.239 presentationTokenResponse (subMsg=4 under OID 0.0.8.239.2) ──
    // MCU grants the presenter token.  TE devices send IndicateOwner + OLC
    // immediately after; we follow the same sequence.
    if (tag == H245_ResponseMessage::e_genericResponse) {
        const H245_GenericMessage& gm =
            (const H245_GenericMessage&)(const H245_GenericMessage&)resp;
        if (gm.m_messageIdentifier.GetTag() == H245_CapabilityIdentifier::e_standard) {
            const PASN_ObjectId& oid =
                (const PASN_ObjectId&)(const PASN_ObjectId&)gm.m_messageIdentifier;
            if (oid == "0.0.8.239.2" &&
                gm.HasOptionalField(H245_GenericMessage::e_subMessageIdentifier))
            {
                int subMsg = (int)gm.m_subMessageIdentifier;
                if (subMsg == 4) {
                    spdlog::info("RecorderConnection: received H.239 presentationTokenResponse (grant)");
                    h239TokenGranted_ = true;
                    sendH239IndicateOwner();
                    if (h239Sender_ && !h239SendActive_.load()) {
                        sendH239OLC();
                    }
                    return TRUE;
                }
            }
        }
    }

    if (tag == H245_ResponseMessage::e_openLogicalChannelAck) {
        const H245_OpenLogicalChannelAck& ack =
            (const H245_OpenLogicalChannelAck&)(const H245_OpenLogicalChannelAck&)resp;
        int chanNum = static_cast<int>(ack.m_forwardLogicalChannelNumber);
        spdlog::info("RecorderConnection: received openLogicalChannelAck "
                     "forwardChannel={}", chanNum);

        // ── Helper: extract MCU RTP receive address from OLC Ack ──────────
        // Parses forwardMultiplexAckParameters → h2250 → mediaChannel → IPv4:port,
        // and optionally extracts dynamicRTPPayloadType.
        // Returns empty string + 0 if not found (caller should use fallback).
        auto extractAckAddr = [&](std::string& ip, int& port, int& pt) {
            do {
                if (!ack.HasOptionalField(H245_OpenLogicalChannelAck::e_forwardMultiplexAckParameters))
                    break;
                const auto& fwdAck = ack.m_forwardMultiplexAckParameters;
                if (fwdAck.GetTag() !=
                    H245_OpenLogicalChannelAck_forwardMultiplexAckParameters::e_h2250LogicalChannelAckParameters)
                    break;
                const H245_H2250LogicalChannelAckParameters& h2250ack =
                    (const H245_H2250LogicalChannelAckParameters&)fwdAck;
                if (!h2250ack.HasOptionalField(H245_H2250LogicalChannelAckParameters::e_mediaChannel))
                    break;
                const H245_TransportAddress& ta = h2250ack.m_mediaChannel;
                if (ta.GetTag() != H245_TransportAddress::e_unicastAddress) break;
                const H245_UnicastAddress& uni = (const H245_UnicastAddress&)ta;
                if (uni.GetTag() != H245_UnicastAddress::e_iPAddress) break;
                const H245_UnicastAddress_iPAddress& ipAddr =
                    (const H245_UnicastAddress_iPAddress&)uni;
                const PASN_OctetString& net = ipAddr.m_network;
                if (net.GetSize() != 4) break;
                char buf[16];
                snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                    (int)net[0], (int)net[1], (int)net[2], (int)net[3]);
                ip   = buf;
                port = (int)ipAddr.m_tsapIdentifier;
                if (h2250ack.HasOptionalField(H245_H2250LogicalChannelAckParameters::e_dynamicRTPPayloadType)) {
                    pt = (int)h2250ack.m_dynamicRTPPayloadType;
                }
            } while (false);
        };

        // Fallback: use signalling-channel peer IP with a guessed port
        auto sigFallbackIP = [&]() -> std::string {
            PIPSocket::Address peerAddr;
            WORD peerPort = 0;
            GetSignallingChannel()->GetRemoteAddress().GetIpAndPort(peerAddr, peerPort);
            return std::string(static_cast<const char*>(peerAddr.AsString()));
        };

        // ── Main video sender OLC Ack (channel == kMainSendChannel) ───────
        if (chanNum == kMainSendChannel && mainSender_ && !mainSendActive_.load()) {
            std::string destIP; int destPort = 0; int pt = 106;
            extractAckAddr(destIP, destPort, pt);
            if (destIP.empty()) {
                destIP   = sigFallbackIP();
                destPort = 20000;   // fallback RTP port for primary video
                spdlog::warn("RecorderConnection: main video Ack has no mediaChannel — "
                             "fallback {}:{}", destIP, destPort);
            } else {
                spdlog::info("RecorderConnection: main video Ack — MCU RTP = {}:{}, PT={}",
                             destIP, destPort, pt);
            }
            if (mainSender_->startSending(destIP, destPort, pt)) {
                mainSendActive_ = true;
                spdlog::info("RecorderConnection: main VideoSender (ScreenSaver) started → {}:{} pt={}",
                             destIP, destPort, pt);
            } else {
                spdlog::error("RecorderConnection: main VideoSender startSending failed");
            }
            return TRUE;
        }

        // ── H.239 aux sender OLC Ack (channel == h239SendChannelNum_) ─────
        if (chanNum == h239SendChannelNum_ && h239Sender_ && !h239SendActive_.load()) {
            std::string destIP; int destPort = 0; int pt = 106;
            extractAckAddr(destIP, destPort, pt);
            if (destIP.empty()) {
                destIP   = sigFallbackIP();
                destPort = 20010;   // fallback RTP port for H.239
                spdlog::warn("RecorderConnection: H.239 Ack has no mediaChannel — "
                             "fallback {}:{}", destIP, destPort);
            } else {
                spdlog::info("RecorderConnection: H.239 send Ack — MCU RTP = {}:{}, PT={}",
                             destIP, destPort, pt);
            }
            if (h239Sender_->startSending(destIP, destPort, pt)) {
                h239SendActive_ = true;
                spdlog::info("RecorderConnection: H.239 VideoSender (NoSignal) started");
            } else {
                spdlog::error("RecorderConnection: H.239 VideoSender startSending failed");
            }
            return TRUE;
        }

        // Other raw OLCs (subscribe channel 100+) — absorb
        if (chanNum >= 100) {
            spdlog::info("RecorderConnection: absorbing Ack for raw OLC "
                         "(channel {})", chanNum);
            return TRUE;   // Absorb — don't let default handler crash
        }
    }
    else if (tag == H245_ResponseMessage::e_openLogicalChannelReject) {
        const H245_OpenLogicalChannelReject& rej =
            (const H245_OpenLogicalChannelReject&)(const H245_OpenLogicalChannelReject&)resp;
        spdlog::warn("RecorderConnection: received openLogicalChannelReject "
                     "forwardChannel={} cause={}",
                     (int)rej.m_forwardLogicalChannelNumber,
                     (int)rej.m_cause.GetTag());

        int rejChan = (int)rej.m_forwardLogicalChannelNumber;
        if (rejChan == kMainSendChannel) {
            spdlog::warn("RecorderConnection: main video OLC rejected — remote cannot receive");
            if (mainSender_) { mainSender_->stop(); mainSender_.reset(); }
            mainSendActive_ = false;
            return TRUE;
        }
        if (rejChan >= 100) {
            spdlog::info("RecorderConnection: absorbing Reject for our raw OLC "
                         "(channel {})", rejChan);
            h239Rejected_ = true;   // MCU does not support H.239 for this meeting
            return TRUE;   // Absorb
        }
    }
    else if (tag == H245_ResponseMessage::e_closeLogicalChannelAck) {
        // CloseLogicalChannelAck may also come for our raw OLCs — absorb
        spdlog::info("RecorderConnection: received closeLogicalChannelAck");
        // Let base class handle this normally
    }

    // For all other responses (including Ack/Reject for MCU-initiated
    // channels), forward to the base class for normal processing.
    return H323Connection::OnH245Response(pdu);
}

void RecorderConnection::OnCapRefreshTimer(PTimer&, INT)
{
    if (!IsEstablished())
        return;
    if (h239Received_.load()) {
        spdlog::info("RecorderConnection: H.239 already received, skip subscribe");
        return;
    }
    if (h239Rejected_.load()) {
        return;   // MCU rejected H.239 — stop retrying
    }
    // 我们自己正在发送演示，没必要订阅别人的演示
    if (h239SendActive_.load() || h239TokenGranted_.load()) {
        return;
    }

    int attempt = capRefreshRetries_.fetch_add(1) + 1;
    spdlog::info("RecorderConnection: H.239 subscribe attempt {} — re-sending TCS to nudge MCU", attempt);

    // ── Strategy: re-send TerminalCapabilitySet (TCS) ─────────────────
    //
    // 之前用 raw OLC(forward, extendedVideoCapability, session=10) 触发的副作用：
    // VP9660 把我们当作演示发送方（"会场发送=是"），即使我们没真的推流。
    // 改用 SendCapabilitySet(FALSE) 重发 TCS——我们的 capability 表里已声明
    // receiveVideoCapability extendedVideoCapability，VP9660 收到后会重新评估
    // H.239 分发列表，把现有演示流推给我们。这条消息不声明任何 sender 角色，
    // 所以"会场发送"通道不会被点亮。
    bool ok = false;
    try {
        SendCapabilitySet(FALSE);
        ok = true;
        spdlog::info("RecorderConnection: TCS re-sent (subscribe nudge)");
    } catch (...) {
        spdlog::error("RecorderConnection: exception in SendCapabilitySet");
    }

    if (!ok) {
        spdlog::warn("RecorderConnection: TCS re-send failed on attempt {}", attempt);
    }

    // 重试节奏：第 1 次 5s（OnEstablished 安排），#2 +8s, #3 +15s, #4 +30s 后放弃
    static constexpr int kMaxAttempts = 4;
    if (attempt < kMaxAttempts) {
        int nextInterval = (attempt == 1) ? 8 : (attempt == 2) ? 15 : 30;
        capRefreshTimer_.SetInterval(0, nextInterval);
        spdlog::info("RecorderConnection: H.239 retry #{} scheduled in {}s", attempt, nextInterval);
    } else {
        spdlog::warn("RecorderConnection: H.239 subscribe exhausted {} attempts, giving up", attempt);
    }
}

std::string RecorderConnection::buildOutputPath(const std::string& suffix) const
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << ep_.config().recorder.output_dir << "/"
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << "_" << ep_.config().gk.alias
        << suffix
        << ".mp4";
    return oss.str();
}

// ── Runtime state getters ──────────────────────────────────────────────────
bool        RecorderConnection::hasH239()        const { return h239Received_.load(); }
bool        RecorderConnection::isRecording()    const { return recorder_ && recorder_->isOpen(); }
bool        RecorderConnection::isAuxRecording() const { return auxRecorder_ && auxRecorder_->isOpen(); }
std::string RecorderConnection::mainFilePath()   const { return recorder_    ? recorder_->outputPath()    : ""; }
std::string RecorderConnection::auxFilePath()    const { return auxRecorder_ ? auxRecorder_->outputPath() : ""; }

// ── H.239 presenter send ───────────────────────────────────────────────────
//
// Per ITU-T H.239 Annex C, the presenter handshake is:
//   1. Sender → MCU : genericRequest, OID=0.0.8.239.2, subMsg=3 (presentationTokenRequest)
//   2. MCU → Sender : genericResponse,                  subMsg=4 (presentationTokenResponse, grant)
//   3. Sender → MCU : genericIndication,                subMsg=6 (presentationTokenIndicateOwner)
//   4. Sender → MCU : openLogicalChannel(session=10, extendedVideoCapability)
//   5. MCU → Sender : openLogicalChannelAck → start RTP
//
// stopPresentation() reverses with closeLogicalChannel + genericCommand
// subMsg=5 (presentationTokenRelease).
//
// Verified against TE50/VP9660 captures (cap/te-发送演示.pcap frames 204-209).

// H.239 generic-message OID (ITU-T H.239 Annex C, generic-message identifier)
static const char* kH239GenericMessageOid = "0.0.8.239.2";

// Token "channelId" parameter value used by Huawei TE/VP9660 for the
// presentation token (parameterIdentifier=44).  The exact value is opaque
// to MCU; we mirror the TE capture so VP9660 sees a familiar request.
static const int kH239TokenChannelId = 258;

// Build the messageContent for presentationToken{Request,IndicateOwner,Release}.
// withBitRate=true adds parameterIdentifier=43 (only present in Request).
static void buildH239TokenContent(H245_GenericMessage& gm,
                                  unsigned terminalNumber,
                                  bool withBitRate)
{
    gm.IncludeOptionalField(H245_GenericMessage::e_messageContent);
    int n = withBitRate ? 3 : 2;
    gm.m_messageContent.SetSize(n);
    int idx = 0;

    // pid=44 → token channelId
    {
        H245_GenericParameter& p = gm.m_messageContent[idx++];
        p.m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        ((PASN_Integer&)(PASN_Integer&)p.m_parameterIdentifier) = 44;
        p.m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        ((PASN_Integer&)(PASN_Integer&)p.m_parameterValue) = kH239TokenChannelId;
    }
    // pid=42 → terminalLabel.terminalNumber
    {
        H245_GenericParameter& p = gm.m_messageContent[idx++];
        p.m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        ((PASN_Integer&)(PASN_Integer&)p.m_parameterIdentifier) = 42;
        p.m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        ((PASN_Integer&)(PASN_Integer&)p.m_parameterValue) = terminalNumber;
    }
    if (withBitRate) {
        // pid=43 → bitRate (mirrors TE: 118 ≈ 1.18 Mbps in TE's units)
        H245_GenericParameter& p = gm.m_messageContent[idx++];
        p.m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        ((PASN_Integer&)(PASN_Integer&)p.m_parameterIdentifier) = 43;
        p.m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        ((PASN_Integer&)(PASN_Integer&)p.m_parameterValue) = 153;  // 1.5 Mbps
    }
}

bool RecorderConnection::startPresentation()
{
    if (!IsEstablished()) {
        spdlog::warn("RecorderConnection::startPresentation — not established");
        return false;
    }
    if (h239SendActive_.load()) {
        spdlog::info("RecorderConnection::startPresentation — already sending");
        return true;
    }

    // ── Phase 1: Create VideoSender + bind socket ─────────────────────────
    VideoSender::Config vsCfg;
    vsCfg.width   = 1280;
    vsCfg.height  = 720;
    vsCfg.fps     = 15;
    vsCfg.bitrate = 1536000;
    vsCfg.mode    = VideoSender::Mode::AuxStream;

    h239Sender_ = std::make_shared<VideoSender>(vsCfg);
    if (!h239Sender_->initNetwork()) {
        spdlog::error("RecorderConnection: VideoSender initNetwork failed");
        h239Sender_.reset();
        return false;
    }
    h239TokenGranted_ = false;
    spdlog::info("RecorderConnection: H.239 sender local UDP port={}",
                 h239Sender_->localPort());

    // 我们要发送演示，关掉迟加入订阅 timer，避免与我方 OLC 冲突
    capRefreshTimer_.Stop();

    // ── Phase 2: Send presentationTokenRequest ────────────────────────────
    // OLC is deferred until the matching presentationTokenResponse arrives
    // (handled in OnH245Response).
    try {
        H323ControlPDU pdu;
        H245_RequestMessage& req = pdu.Build(H245_RequestMessage::e_genericRequest);
        H245_GenericMessage& gm = (H245_GenericMessage&)(H245_GenericMessage&)req;

        gm.m_messageIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& oid = (PASN_ObjectId&)(PASN_ObjectId&)gm.m_messageIdentifier;
        oid.SetValue(kH239GenericMessageOid);

        gm.IncludeOptionalField(H245_GenericMessage::e_subMessageIdentifier);
        gm.m_subMessageIdentifier = 3;   // presentationTokenRequest

        buildH239TokenContent(gm, h243TerminalNumber_.load(), /*withBitRate=*/true);

        WriteControlPDU(pdu);
        spdlog::info("RecorderConnection: sent H.239 presentationTokenRequest "
                     "(term={})", h243TerminalNumber_.load());
    } catch (...) {
        spdlog::warn("RecorderConnection: failed to send presentationTokenRequest");
        h239Sender_.reset();
        return false;
    }

    return true;
}

void RecorderConnection::sendH239IndicateOwner()
{
    try {
        H323ControlPDU pdu;
        H245_IndicationMessage& ind =
            pdu.Build(H245_IndicationMessage::e_genericIndication);
        H245_GenericMessage& gm = (H245_GenericMessage&)(H245_GenericMessage&)ind;

        gm.m_messageIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& oid = (PASN_ObjectId&)(PASN_ObjectId&)gm.m_messageIdentifier;
        oid.SetValue(kH239GenericMessageOid);

        gm.IncludeOptionalField(H245_GenericMessage::e_subMessageIdentifier);
        gm.m_subMessageIdentifier = 6;   // presentationTokenIndicateOwner

        buildH239TokenContent(gm, h243TerminalNumber_.load(), /*withBitRate=*/false);

        WriteControlPDU(pdu);
        spdlog::info("RecorderConnection: sent H.239 presentationTokenIndicateOwner "
                     "(term={})", h243TerminalNumber_.load());
    } catch (...) {
        spdlog::warn("RecorderConnection: failed to send presentationTokenIndicateOwner");
    }
}

// ── sendH239OLC: called from OnH245Response after presentationTokenResponse
// (MCU grant).  Sends raw openLogicalChannel(session=10, extendedVideo) to
// the MCU.  VideoSender.startSending() runs when the OLC Ack arrives.
void RecorderConnection::sendH239OLC()
{

    // ── 3. Get our local LAN IP ───────────────────────────────────────────
    // UDP routing trick (same as startMainVideo): connect toward MCU, read
    // back local interface IP via getsockname.
    std::string localIP = "127.0.0.1";
    {
        PIPSocket::Address peerAddr;
        WORD peerPort = 0;
        GetSignallingChannel()->GetRemoteAddress().GetIpAndPort(peerAddr, peerPort);
        std::string remoteIPStr =
            std::string(static_cast<const char*>(peerAddr.AsString()));
        int probeSock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probeSock >= 0) {
            struct sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(1);
            inet_aton(remoteIPStr.c_str(), &dest.sin_addr);
            if (::connect(probeSock,
                          reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0) {
                struct sockaddr_in local{};
                socklen_t slen = sizeof(local);
                ::getsockname(probeSock,
                              reinterpret_cast<sockaddr*>(&local), &slen);
                char buf[INET_ADDRSTRLEN];
                ::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                localIP = std::string(buf);
            }
            ::close(probeSock);
        }
    }
    spdlog::info("RecorderConnection: H.239 sender local IP={}", localIP);

    // ── 4. Send raw OLC(session=10, extendedVideo) with our RTP endpoint ──
    try {
        H323ControlPDU pdu;
        H245_RequestMessage& req = pdu.Build(H245_RequestMessage::e_openLogicalChannel);
        H245_OpenLogicalChannel& olc =
            (H245_OpenLogicalChannel&)(H245_OpenLogicalChannel&)req;

        h239SendChannelNum_ = 200;
        olc.m_forwardLogicalChannelNumber = h239SendChannelNum_;

        auto& fwd = olc.m_forwardLogicalChannelParameters;

        // dataType = videoData(extendedVideoCapability(H.264, H.239 OID))
        fwd.m_dataType.SetTag(H245_DataType::e_videoData);
        H245_VideoCapability& vc = (H245_VideoCapability&)(H245_VideoCapability&)fwd.m_dataType;
        vc.SetTag(H245_VideoCapability::e_extendedVideoCapability);

        H245_ExtendedVideoCapability& extCap =
            (H245_ExtendedVideoCapability&)(H245_ExtendedVideoCapability&)vc;

        extCap.m_videoCapability.SetSize(1);
        H245_VideoCapability& innerVc = extCap.m_videoCapability[0];
        // Use H323_H264RecvCap to generate full H.241 collapsing params
        H323_H264RecvCap h264cap(8);  // High Profile
        h264cap.OnSendingPDU(innerVc, H323Capability::e_OLC);
        // Override maxBitRate to 1.5 Mbps
        if (innerVc.GetTag() == H245_VideoCapability::e_genericVideoCapability) {
            H245_GenericCapability& g = (H245_GenericCapability&)(H245_GenericCapability&)innerVc;
            g.IncludeOptionalField(H245_GenericCapability::e_maxBitRate);
            g.m_maxBitRate = 15360;   // 1.5 Mbps (unit=100bps)
        }

        extCap.IncludeOptionalField(H245_ExtendedVideoCapability::e_videoCapabilityExtension);
        extCap.m_videoCapabilityExtension.SetSize(1);
        H245_GenericCapability& h239Gen = extCap.m_videoCapabilityExtension[0];
        h239Gen.m_capabilityIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& h239Oid =
            (PASN_ObjectId&)(PASN_ObjectId&)h239Gen.m_capabilityIdentifier;
        h239Oid.SetValue("0.0.8.239.1.2");

        // multiplexParameters = h2250(sessionID=10)
        // NOTE: mediaChannel is intentionally omitted — the MCU provides
        // its actual receive address in the OLC Ack (same as main video).
        fwd.m_multiplexParameters.SetTag(
            H245_OpenLogicalChannel_forwardLogicalChannelParameters_multiplexParameters
                ::e_h2250LogicalChannelParameters);

        H245_H2250LogicalChannelParameters& h2250 =
            (H245_H2250LogicalChannelParameters&)(H245_H2250LogicalChannelParameters&)
                fwd.m_multiplexParameters;
        h2250.m_sessionID = 10;

        WriteControlPDU(pdu);
        spdlog::info("RecorderConnection: sent H.239 send OLC "
                     "(channel={}, session=10)",
                     h239SendChannelNum_);
    } catch (const std::exception& e) {
        spdlog::error("RecorderConnection: exception building H.239 send OLC: {}", e.what());
        h239Sender_.reset();
        return;  // void
    }

    // VideoSender.startSending() will be called from OnH245Response when Ack arrives.
}

bool RecorderConnection::stopPresentation()
{
    if (!h239SendActive_.load() && !h239Sender_) {
        spdlog::info("RecorderConnection::stopPresentation — not sending");
        return true;
    }

    // ── 1. Stop VideoSender ───────────────────────────────────────────────
    if (h239Sender_) {
        h239Sender_->stop();
        h239Sender_.reset();
    }
    h239SendActive_ = false;
    h239TokenGranted_ = false;

    // ── 2. Send closeLogicalChannel ───────────────────────────────────────
    if (IsEstablished()) {
        try {
            H323ControlPDU pdu;
            H245_RequestMessage& req = pdu.Build(H245_RequestMessage::e_closeLogicalChannel);
            H245_CloseLogicalChannel& clc =
                (H245_CloseLogicalChannel&)(H245_CloseLogicalChannel&)req;
            clc.m_forwardLogicalChannelNumber = h239SendChannelNum_;
            clc.m_source.SetTag(H245_CloseLogicalChannel_source::e_user);
            WriteControlPDU(pdu);
            spdlog::info("RecorderConnection: sent CLC for H.239 send channel {}",
                         h239SendChannelNum_);
        } catch (...) {
            spdlog::warn("RecorderConnection: failed to send CLC");
        }

        // ── 3. Send presentationTokenRelease (genericCommand, subMsg=5) ──
        try {
            H323ControlPDU pdu;
            H245_CommandMessage& cmd = pdu.Build(H245_CommandMessage::e_genericCommand);
            H245_GenericMessage& gm = (H245_GenericMessage&)(H245_GenericMessage&)cmd;

            gm.m_messageIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
            PASN_ObjectId& oid =
                (PASN_ObjectId&)(PASN_ObjectId&)gm.m_messageIdentifier;
            oid.SetValue(kH239GenericMessageOid);
            gm.IncludeOptionalField(H245_GenericMessage::e_subMessageIdentifier);
            gm.m_subMessageIdentifier = 5;  // presentationTokenRelease

            buildH239TokenContent(gm, h243TerminalNumber_.load(), /*withBitRate=*/false);

            WriteControlPDU(pdu);
            spdlog::info("RecorderConnection: sent H.239 presentationTokenRelease");
        } catch (...) {
            spdlog::warn("RecorderConnection: failed to send presentationTokenRelease");
        }
    }

    return true;
}

// ─── Main video send ──────────────────────────────────────────────────────────
//
// startMainVideo():
//   1. Creates VideoSender(ScreenSaver) + binds local UDP socket.
//   2. Sends raw OLC(session=2, H.264) carrying our local RTP port.
//   3. OnH245Response() absorbs the Ack and starts VideoSender → real RTP flow.
//
// stopMainVideo():
//   1. Stops VideoSender.
//   2. Sends CLC for the main video channel.

bool RecorderConnection::startMainVideo()
{
    if (!IsEstablished()) {
        spdlog::warn("RecorderConnection::startMainVideo — not established");
        return false;
    }
    if (mainSendActive_.load()) {
        spdlog::info("RecorderConnection::startMainVideo — already sending");
        return true;
    }

    // ── 1. Create VideoSender(ScreenSaver) + bind socket ─────────────────
    // Use standard CIF (352×288) — non-standard dimensions (e.g. 320×240)
    // may confuse VP9660's H.264 decoder.  15fps is the H.323 standard rate
    // for CIF.  repeat_headers=1 ensures SPS+PPS precede every IDR frame.
    VideoSender::Config vsCfg;
    vsCfg.width   = 1280;
    vsCfg.height  = 720;
    vsCfg.fps     = 15;
    vsCfg.bitrate = 1536000;
    vsCfg.mode    = VideoSender::Mode::MainStream;

    mainSender_ = std::make_shared<VideoSender>(vsCfg);
    if (!mainSender_->initNetwork()) {
        spdlog::error("RecorderConnection: mainSender initNetwork failed");
        mainSender_.reset();
        return false;
    }
    int localRtpPort = mainSender_->localPort();

    // ── 2. Local IP ───────────────────────────────────────────────────────
    // UDP routing trick: connect() a UDP socket toward the MCU IP (sends
    // nothing), then getsockname() reveals which local interface the kernel
    // will use.  Avoids hostname→127.0.0.1 pitfalls in /etc/hosts.
    std::string localIP = "127.0.0.1";
    {
        PIPSocket::Address peerAddr;
        WORD peerPort = 0;
        GetSignallingChannel()->GetRemoteAddress().GetIpAndPort(peerAddr, peerPort);
        std::string remoteIPStr =
            std::string(static_cast<const char*>(peerAddr.AsString()));
        int probeSock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probeSock >= 0) {
            struct sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(1);
            inet_aton(remoteIPStr.c_str(), &dest.sin_addr);
            if (::connect(probeSock,
                          reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0) {
                struct sockaddr_in local{};
                socklen_t slen = sizeof(local);
                ::getsockname(probeSock,
                              reinterpret_cast<sockaddr*>(&local), &slen);
                char buf[INET_ADDRSTRLEN];
                ::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                localIP = std::string(buf);
            }
            ::close(probeSock);
        }
    }
    spdlog::info("RecorderConnection: main video sender local={}:{}", localIP, localRtpPort);

    // ── 3. Send raw OLC(session=2, H.264 genericVideoCapability) ─────────
    try {
        H323ControlPDU pdu;
        H245_RequestMessage& req = pdu.Build(H245_RequestMessage::e_openLogicalChannel);
        H245_OpenLogicalChannel& olc =
            (H245_OpenLogicalChannel&)(H245_OpenLogicalChannel&)req;

        olc.m_forwardLogicalChannelNumber = kMainSendChannel;

        auto& fwd = olc.m_forwardLogicalChannelParameters;

        // Build dataType = videoData(genericVideoCapability(H.264 HP))
        // using H323_H264RecvCap which fills all H.241 collapsing params
        // (profile, level, CustomMaxMBPS, CustomMaxFS) that MCU requires.
        fwd.m_dataType.SetTag(H245_DataType::e_videoData);
        H245_VideoCapability& vc =
            (H245_VideoCapability&)(H245_VideoCapability&)fwd.m_dataType;
        H323_H264RecvCap cap(8);  // High Profile (8)
        cap.OnSendingPDU(vc, H323Capability::e_OLC);

        // multiplexParameters = h2250(sessionID=2 = primary video)
        fwd.m_multiplexParameters.SetTag(
            H245_OpenLogicalChannel_forwardLogicalChannelParameters_multiplexParameters
                ::e_h2250LogicalChannelParameters);
        H245_H2250LogicalChannelParameters& h2250 =
            (H245_H2250LogicalChannelParameters&)(H245_H2250LogicalChannelParameters&)
                fwd.m_multiplexParameters;
        h2250.m_sessionID = 2;   // primary video session
        // NOTE: We intentionally omit h2250.mediaChannel here.
        // In H.245, mediaChannel in a sender's OLC should be the RECEIVER's
        // RTP address — but we don't know the MCU's port until the OLC Ack.
        // The MCU will provide its actual receive address in the OLC Ack's
        // forwardMultiplexAckParameters.h2250.mediaChannel; we parse that
        // in OnH245Response() and pass it to VideoSender::startSending().

        WriteControlPDU(pdu);
        spdlog::info("RecorderConnection: sent main video OLC "
                     "(channel={}, session=2)",
                     kMainSendChannel);
    } catch (const std::exception& e) {
        spdlog::error("RecorderConnection: exception building main video OLC: {}", e.what());
        mainSender_.reset();
        return false;
    }

    // VideoSender.startSending() will be called from OnH245Response when Ack arrives.
    return true;
}

bool RecorderConnection::stopMainVideo()
{
    if (!mainSendActive_.load() && !mainSender_) {
        spdlog::info("RecorderConnection::stopMainVideo — not sending");
        return true;
    }

    // ── 1. Stop VideoSender ───────────────────────────────────────────────
    if (mainSender_) {
        mainSender_->stop();
        mainSender_.reset();
    }
    mainSendActive_ = false;

    // ── 2. Send CLC ───────────────────────────────────────────────────────
    if (IsEstablished()) {
        try {
            H323ControlPDU pdu;
            H245_RequestMessage& req = pdu.Build(H245_RequestMessage::e_closeLogicalChannel);
            H245_CloseLogicalChannel& clc =
                (H245_CloseLogicalChannel&)(H245_CloseLogicalChannel&)req;
            clc.m_forwardLogicalChannelNumber = kMainSendChannel;
            clc.m_source.SetTag(H245_CloseLogicalChannel_source::e_lcse);
            WriteControlPDU(pdu);
            spdlog::info("RecorderConnection: sent CLC for main video channel {}",
                         kMainSendChannel);
        } catch (...) {
            spdlog::warn("RecorderConnection: failed to send CLC for main video");
        }
    }

    return true;
}
