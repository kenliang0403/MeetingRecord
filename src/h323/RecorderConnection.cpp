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

    // conferenceRequests + TE nonStandard
    try { H323ControlPDU p; H245_RequestMessage& r=p.Build(H245_RequestMessage::e_conferenceRequest); H245_ConferenceRequest& c=(H245_ConferenceRequest&)r; c.SetTag(H245_ConferenceRequest::e_terminalListRequest); WriteControlPDU(p); } catch(...) {}
    try { H323ControlPDU p; H245_RequestMessage& r=p.Build(H245_RequestMessage::e_conferenceRequest); H245_ConferenceRequest& c=(H245_ConferenceRequest&)r; c.SetTag(H245_ConferenceRequest::e_requestAllTerminalIDs); WriteControlPDU(p); } catch(...) {}
    try { H323ControlPDU p; H245_RequestMessage& r=p.Build(H245_RequestMessage::e_conferenceRequest); H245_ConferenceRequest& c=(H245_ConferenceRequest&)r; c.SetTag(H245_ConferenceRequest::e_requestChairTokenOwner); WriteControlPDU(p); } catch(...) {}
    try { H323ControlPDU p; H245_RequestMessage& r=p.Build(H245_RequestMessage::e_nonStandard); H245_NonStandardMessage& n=(H245_NonStandardMessage&)r; n.m_nonStandardData.m_nonStandardIdentifier.SetTag(H245_NonStandardIdentifier::e_h221NonStandard); H245_NonStandardIdentifier_h221NonStandard& h=(H245_NonStandardIdentifier_h221NonStandard&)n.m_nonStandardData.m_nonStandardIdentifier; h.m_t35CountryCode=86;h.m_t35Extension=1;h.m_manufacturerCode=1; static const BYTE q[]={0x01,0x06,0x00,0x08,0x81,0x75,0x00,0x02,0x48,0x00}; n.m_nonStandardData.m_data.SetValue(q,sizeof(q)); WriteControlPDU(p); } catch(...) {}

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
    const H245_RequestMessage& req = pdu;
    spdlog::info("RecorderConnection: ← H245Request tag={}", (int)req.GetTag());
    return H323Connection::OnH245Request(pdu);
}

// ── H.245 command/indication: detect SMC-initiated H.239 close ────────
//
// 当 SMC 控制台关掉我们的演示时，MCU 会向我们发送一个控制消息。可能形态：
//   A) genericCommand,    OID=0.0.8.239.2, subMsg=5 (presentationTokenRelease 命令我们释放)
//   B) genericIndication, OID=0.0.8.239.2, subMsg=6 (IndicateOwner，告诉我们当前所有者变了)
//   C) closeLogicalChannel(channel=200) 直接关掉我们的发送通道
//
// 这两个 hook 先把所有 H.239 OID 的 generic 消息打印出来，再判断 subMsg
// 决定是否触发 stopPresentation()。channel 关闭路径已经在 OnClosedLogicalChannel
// 处理（session=10）。
namespace {
bool isH239GenericMessage(const H245_GenericMessage& gm)
{
    if (gm.m_messageIdentifier.GetTag() != H245_CapabilityIdentifier::e_standard)
        return false;
    const PASN_ObjectId& oid =
        (const PASN_ObjectId&)(const PASN_ObjectId&)gm.m_messageIdentifier;
    return oid.AsString() == "0.0.8.239.2";
}

int h239SubMsg(const H245_GenericMessage& gm)
{
    return (int)gm.m_subMessageIdentifier.GetValue();
}
}  // namespace

PBoolean RecorderConnection::OnH245Command(const H323ControlPDU& pdu)
{
    const H245_CommandMessage& cmd = pdu;
    spdlog::info("RecorderConnection: ← H245Command tag={}", (int)cmd.GetTag());
    if (cmd.GetTag() == H245_CommandMessage::e_genericCommand) {
        const H245_GenericMessage& gm =
            (const H245_GenericMessage&)(const H245_GenericMessage&)cmd;
        if (isH239GenericMessage(gm)) {
            int sub = h239SubMsg(gm);
            spdlog::info("RecorderConnection: ← genericCommand H.239 subMsg={}", sub);
            // subMsg=5 = presentationTokenRelease — MCU 命令我们释放演示
            if (sub == 5 && h239SendActive_.load()) {
                spdlog::info("RecorderConnection: SMC/MCU forced presentationTokenRelease "
                             "→ stopping our H.239 send");
                stopPresentation();
                return TRUE;
            }
        } else {
            spdlog::info("RecorderConnection: ← genericCommand (non-H.239)");
        }
    }
    // Huawei nonStandardCommand：SMC 远端控制开/关演示
    // 0429 信令抓包确认: SMC "发送/停止演示" 操作前 14-22ms，MCU→TE 发送
    // h221 86/1/2457 nonStandardCommand，data 18B:
    //   50 42 50 42 00 0C 07 0E 02 00 00 00 00 00 00 00 00 ZZ
    //   PBPB    len   cmd070E param fields           action
    // ZZ=01 → start presentation, ZZ=00 → stop presentation
    else if (cmd.GetTag() == H245_CommandMessage::e_nonStandard) {
        const H245_NonStandardMessage& nsm =
            (const H245_NonStandardMessage&)(const H245_NonStandardMessage&)cmd;
        const H245_NonStandardParameter& nsp = nsm.m_nonStandardData;
        if (nsp.m_nonStandardIdentifier.GetTag() ==
            H245_NonStandardIdentifier::e_h221NonStandard) {
            const H245_NonStandardIdentifier_h221NonStandard& h221 =
                (const H245_NonStandardIdentifier_h221NonStandard&)
                (const H245_NonStandardIdentifier_h221NonStandard&)
                nsp.m_nonStandardIdentifier;
            unsigned cc    = (unsigned)h221.m_t35CountryCode;
            unsigned ext   = (unsigned)h221.m_t35Extension;
            unsigned manuf = (unsigned)h221.m_manufacturerCode;
            const PBYTEArray& data = nsp.m_data.GetValue();
            PINDEX dlen = data.GetSize();
            spdlog::info("RecorderConnection: ← nonStandardCommand h221 "
                         "{}/{}/{} data {} bytes", cc, ext, manuf, (int)dlen);
            // Huawei vendor (86/1/2457) + PBPB...070E... 模式
            if (cc == 86 && ext == 1 && manuf == 2457 && dlen >= 18) {
                const BYTE* p = (const BYTE*)data;
                bool isPbpb = (p[0] == 0x50 && p[1] == 0x42 &&
                               p[2] == 0x50 && p[3] == 0x42);
                bool is070E = (p[6] == 0x07 && p[7] == 0x0E);
                if (isPbpb && is070E) {
                    BYTE action = p[dlen - 1];
                    spdlog::info("RecorderConnection: SMC 070E action=0x{:02X}", action);
                    if (action == 0x01) {
                        if (!h239SendActive_.load()) {
                            spdlog::info("RecorderConnection: SMC remote-start presentation");
                            startPresentation();
                        } else {
                            spdlog::info("RecorderConnection: SMC remote-start ignored "
                                         "(already presenting)");
                        }
                        return TRUE;
                    } else if (action == 0x00) {
                        if (h239SendActive_.load()) {
                            spdlog::info("RecorderConnection: SMC remote-stop presentation");
                            stopPresentation();
                        } else {
                            spdlog::info("RecorderConnection: SMC remote-stop ignored "
                                         "(not presenting)");
                        }
                        return TRUE;
                    }
                }
            }
            if (cc==86 && ext==1 && (manuf==4608||manuf==4614||manuf==4610||manuf==4616||manuf==4612||manuf==4607||manuf==4609||manuf==4615)) {
                try { H323ControlPDU epdu; H245_CommandMessage& ec=epdu.Build(H245_CommandMessage::e_nonStandard); H245_NonStandardMessage& en=(H245_NonStandardMessage&)ec; en.m_nonStandardData.m_nonStandardIdentifier.SetTag(H245_NonStandardIdentifier::e_h221NonStandard); H245_NonStandardIdentifier_h221NonStandard& eh=(H245_NonStandardIdentifier_h221NonStandard&)en.m_nonStandardData.m_nonStandardIdentifier; eh.m_t35CountryCode=cc; eh.m_t35Extension=ext; eh.m_manufacturerCode=manuf; en.m_nonStandardData.m_data=nsp.m_data; WriteControlPDU(epdu); } catch(...) {} return TRUE; }
            // absorb other Huawei
            if (cc == 86 && ext == 1) { return TRUE; }
        }
    }
    return H323Connection::OnH245Command(pdu);
}

// ── TCS injection: append Huawei vendor markers so MCU treats us as Huawei ──
//
// 0429 抓包对比 TE52 vs 我们的 TCS, TE52 含三条 nonStandardCapability:
//   A) receiveAndTransmitAudioCapability:nonStandard h221 38/0/8209 56B
//   B) receiveVideoCapability:nonStandard           h221 38/0/8209 32B
//   C) 顶层 capability:nonStandard (vendor 标识)    h221 28/21/555  16B
// 第一次只注入 (C) MCU 仍判定我们 "不支持 SMC 侧发辅流"。补充 (A) 和 (B):
// 这两条声明 "我有华为私有音/视频编解码"，是 MCU 识别 Huawei 端的关键。
// 注意：MCU 可能尝试以这些 codec 给我们建 OLC—— 我们需要在
// OnCreateLogicalChannel 拒绝（或允许后忽略 RTP）。先按 receive-only 方向声明。
namespace {
void appendNonStandardEntry(H245_TerminalCapabilitySet& pdu,
                             unsigned& nextNum,
                             unsigned t35CC,
                             unsigned t35Ext,
                             unsigned mfr,
                             const BYTE* data, PINDEX dataLen,
                             int subTag /* -1 = top-level capability:nonStandard;
                                           else H245_Capability::e_xxx */)
{
    PINDEX i = pdu.m_capabilityTable.GetSize();
    pdu.m_capabilityTable.SetSize(i + 1);
    H245_CapabilityTableEntry& entry = pdu.m_capabilityTable[i];
    entry.m_capabilityTableEntryNumber = nextNum++;
    entry.IncludeOptionalField(H245_CapabilityTableEntry::e_capability);

    if (subTag < 0) {
        // 顶层 nonStandard capability (Entry C)
        entry.m_capability.SetTag(H245_Capability::e_nonStandard);
        H245_NonStandardParameter& ns =
            (H245_NonStandardParameter&)(H245_NonStandardParameter&)entry.m_capability;
        ns.m_nonStandardIdentifier.SetTag(H245_NonStandardIdentifier::e_h221NonStandard);
        H245_NonStandardIdentifier_h221NonStandard& h221 =
            (H245_NonStandardIdentifier_h221NonStandard&)
            (H245_NonStandardIdentifier_h221NonStandard&)
            ns.m_nonStandardIdentifier;
        h221.m_t35CountryCode   = t35CC;
        h221.m_t35Extension     = t35Ext;
        h221.m_manufacturerCode = mfr;
        ns.m_data.SetValue(data, dataLen);
    } else if (subTag == H245_Capability::e_receiveAndTransmitAudioCapability) {
        // Entry A: receiveAndTransmitAudioCapability → AudioCapability::nonStandard
        entry.m_capability.SetTag(H245_Capability::e_receiveAndTransmitAudioCapability);
        H245_AudioCapability& ac =
            (H245_AudioCapability&)(H245_AudioCapability&)entry.m_capability;
        ac.SetTag(H245_AudioCapability::e_nonStandard);
        H245_NonStandardParameter& ns =
            (H245_NonStandardParameter&)(H245_NonStandardParameter&)ac;
        ns.m_nonStandardIdentifier.SetTag(H245_NonStandardIdentifier::e_h221NonStandard);
        H245_NonStandardIdentifier_h221NonStandard& h221 =
            (H245_NonStandardIdentifier_h221NonStandard&)
            (H245_NonStandardIdentifier_h221NonStandard&)
            ns.m_nonStandardIdentifier;
        h221.m_t35CountryCode   = t35CC;
        h221.m_t35Extension     = t35Ext;
        h221.m_manufacturerCode = mfr;
        ns.m_data.SetValue(data, dataLen);
    } else if (subTag == H245_Capability::e_receiveVideoCapability) {
        // Entry B: receiveVideoCapability → VideoCapability::nonStandard
        entry.m_capability.SetTag(H245_Capability::e_receiveVideoCapability);
        H245_VideoCapability& vc =
            (H245_VideoCapability&)(H245_VideoCapability&)entry.m_capability;
        vc.SetTag(H245_VideoCapability::e_nonStandard);
        H245_NonStandardParameter& ns =
            (H245_NonStandardParameter&)(H245_NonStandardParameter&)vc;
        ns.m_nonStandardIdentifier.SetTag(H245_NonStandardIdentifier::e_h221NonStandard);
        H245_NonStandardIdentifier_h221NonStandard& h221 =
            (H245_NonStandardIdentifier_h221NonStandard&)
            (H245_NonStandardIdentifier_h221NonStandard&)
            ns.m_nonStandardIdentifier;
        h221.m_t35CountryCode   = t35CC;
        h221.m_t35Extension     = t35Ext;
        h221.m_manufacturerCode = mfr;
        ns.m_data.SetValue(data, dataLen);
    }
}
}  // namespace

void RecorderConnection::OnSendCapabilitySet(H245_TerminalCapabilitySet& pdu)
{
    H323Connection::OnSendCapabilitySet(pdu);

    if (!pdu.HasOptionalField(H245_TerminalCapabilitySet::e_capabilityTable)) {
        pdu.IncludeOptionalField(H245_TerminalCapabilitySet::e_capabilityTable);
    }

    // Audio promotion
    { PINDEX s=pdu.m_capabilityTable.GetSize(); for(PINDEX i=0;i<s;++i) if(pdu.m_capabilityTable[i].m_capability.GetTag()==H245_Capability::e_receiveAudioCapability){ PINDEX idx=pdu.m_capabilityTable.GetSize(); pdu.m_capabilityTable.SetSize(idx+1); pdu.m_capabilityTable[idx]=pdu.m_capabilityTable[i]; pdu.m_capabilityTable[idx].m_capability.SetTag(H245_Capability::e_receiveAndTransmitAudioCapability); } }

    unsigned maxNum = 0;
    for (PINDEX i = 0; i < pdu.m_capabilityTable.GetSize(); ++i) {
        unsigned n = (unsigned)pdu.m_capabilityTable[i].m_capabilityTableEntryNumber;
        if (n > maxNum) maxNum = n;
    }
    unsigned nextNum = maxNum + 1;
    PINDEX startSize = pdu.m_capabilityTable.GetSize();

    // Entry A (华为私有音频 codec 标识) 已禁用 ──
    // 0430 抓包：声明该 cap 后 MCU 立即用 nonStd h221 38/0/8209 发 OLC,
    // 而我们没有真正的 receiver → reject(unknownDataType) → SMC 报
    // "通道没有打开"导致整个呼叫失败。
    // 现在依赖 H.225 vendor 伪装成华为 TE52 (38/21/555 + "HUAWEI TEx0")
    // 让 MCU 把我们当 Huawei 终端，但 audio 协商仍用标准 G.711/G.722/AAC-LD。
    // 若后续发现 MCU 对此终端类型仍跳过 070E，再考虑实现一个伪造的
    // NonStandardAudioRecvCap (accept-and-discard) 把 Entry A 重新打开。

    // Entry B: 华为私有视频 codec 标识 (h221 38/0/8209, 32B 含 "RTRT")
    static const BYTE huaweiVideoCodec[] = {
        0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x18,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x04,
        0x52,0x54,0x52,0x54, 0x00,0x00,0x00,0x03,
        0x00,0x00,0x00,0x04, 0x02,0x94,0x3d,0xee
    };
    appendNonStandardEntry(pdu, nextNum, 38, 0, 8209,
                           huaweiVideoCodec, sizeof(huaweiVideoCodec),
                           H245_Capability::e_receiveVideoCapability);

    // Entry C: 顶层 vendor marker (h221 28/21/555, 16B)
    static const BYTE huaweiVendorMarker[] = {
        0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x08,
        0x00,0x01,0x00,0x00, 0x00,0x00,0x00,0x00
    };
    appendNonStandardEntry(pdu, nextNum, 28, 21, 555,
                           huaweiVendorMarker, sizeof(huaweiVendorMarker),
                           -1);

    // 现在只有 Entry B (video 38/0/8209) 和 Entry C (vendor marker 28/21/555)
    unsigned entryB = maxNum + 1;
    unsigned entryC = maxNum + 2;

    // 仅放入 capabilityTable 不够 — H.245 spec 要求每个用到的 entry 必须被
    // capabilityDescriptors[].simultaneousCapabilities[].alternativeCapability
    // 引用，否则 MCU 视为无效 / 未声明。追加新 descriptor，里面 B/C 单 cap
    // simultaneous-set，仅作 vendor 识别 hint。
    if (!pdu.HasOptionalField(H245_TerminalCapabilitySet::e_capabilityDescriptors)) {
        pdu.IncludeOptionalField(H245_TerminalCapabilitySet::e_capabilityDescriptors);
    }
    PINDEX descIdx = pdu.m_capabilityDescriptors.GetSize();
    pdu.m_capabilityDescriptors.SetSize(descIdx + 1);
    H245_CapabilityDescriptor& desc = pdu.m_capabilityDescriptors[descIdx];

    unsigned maxDescNum = 0;
    for (PINDEX i = 0; i < descIdx; ++i) {
        unsigned n = (unsigned)pdu.m_capabilityDescriptors[i].m_capabilityDescriptorNumber;
        if (n > maxDescNum) maxDescNum = n;
    }
    desc.m_capabilityDescriptorNumber = maxDescNum + 1;
    desc.IncludeOptionalField(H245_CapabilityDescriptor::e_simultaneousCapabilities);
    desc.m_simultaneousCapabilities.SetSize(2);
    auto fillSet = [](H245_AlternativeCapabilitySet& s, unsigned n) {
        s.SetSize(1);
        s[0] = n;
    };
    fillSet(desc.m_simultaneousCapabilities[0], entryB);
    fillSet(desc.m_simultaneousCapabilities[1], entryC);

    spdlog::info("RecorderConnection: nonStandard entries B=#{} C=#{}", entryB, entryC);
    // T.120
    { PINDEX idx=pdu.m_capabilityTable.GetSize(); pdu.m_capabilityTable.SetSize(idx+1); H245_CapabilityTableEntry& e=pdu.m_capabilityTable[idx]; e.m_capabilityTableEntryNumber=nextNum++; e.m_capability.SetTag(H245_Capability::e_receiveAndTransmitDataApplicationCapability); H245_DataApplicationCapability& c=(H245_DataApplicationCapability&)e.m_capability; c.m_application.SetTag(1); ((PASN_Choice&)c.m_application).SetTag(1); c.m_maxBitRate=640; }
}

PBoolean RecorderConnection::OnH245Indication(const H323ControlPDU& pdu)
{
    const H245_IndicationMessage& ind = pdu;
    spdlog::info("RecorderConnection: ← H245Indication tag={}", (int)ind.GetTag());
    if (ind.GetTag() == H245_IndicationMessage::e_genericIndication) {
        const H245_GenericMessage& gm =
            (const H245_GenericMessage&)(const H245_GenericMessage&)ind;
        if (isH239GenericMessage(gm)) {
            int sub = h239SubMsg(gm);
            spdlog::info("RecorderConnection: ← genericIndication H.239 subMsg={}", sub);
            // subMsg=6 = IndicateOwner — 当前所有者变更。如果新 owner 不是我们
            // (terminalNumber 不同)，说明被夺权了，停止发送。
            // 暂时先只打日志，等观察到具体内容再决定是否动作。
        } else {
            spdlog::info("RecorderConnection: ← genericIndication (non-H.239)");
        }
    }
    // 同 OnH245Command：吞掉 Huawei 私有 nonStandardIndication，避免基类回 functionNotUnderstood
    else if (ind.GetTag() == H245_IndicationMessage::e_nonStandard) {
        const H245_NonStandardMessage& nsm =
            (const H245_NonStandardMessage&)(const H245_NonStandardMessage&)ind;
        const H245_NonStandardParameter& nsp = nsm.m_nonStandardData;
        if (nsp.m_nonStandardIdentifier.GetTag() ==
            H245_NonStandardIdentifier::e_h221NonStandard) {
            const H245_NonStandardIdentifier_h221NonStandard& h221 =
                (const H245_NonStandardIdentifier_h221NonStandard&)
                (const H245_NonStandardIdentifier_h221NonStandard&)
                nsp.m_nonStandardIdentifier;
            unsigned cc    = (unsigned)h221.m_t35CountryCode;
            unsigned ext   = (unsigned)h221.m_t35Extension;
            unsigned manuf = (unsigned)h221.m_manufacturerCode;
            spdlog::info("RecorderConnection: ← nonStandardIndication h221 {}/{}/{}",
                         cc, ext, manuf);
            if (cc == 86 && ext == 1) {
                return TRUE;
            }
        }
    }
    return H323Connection::OnH245Indication(pdu);
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

        // Other raw OLCs (subscribe channel 100+) — absorb, then immediately close.
        //
        // 我们的 raw OLC 只是为了"唤醒"VP9660 重新评估 H.239 分发表（让它把演示推给我们）。
        // 拿到 ack 时 MCU 已经把我们加进接收列表并开始推 OLC(session=10) 给我们。
        // 此时立刻 closeLogicalChannel 我们的 forward channel：VP9660 把"演示发送方"
        // 标记清掉（"会场发送=否"），但它推给我们的那条独立 channel 不受影响。
        if (chanNum >= 100) {
            spdlog::info("RecorderConnection: absorbing Ack for raw OLC "
                         "(channel {}), sending closeLogicalChannel", chanNum);
            try {
                H323ControlPDU closePdu;
                H245_RequestMessage& closeReq = closePdu.Build(
                    H245_RequestMessage::e_closeLogicalChannel);
                H245_CloseLogicalChannel& clc =
                    (H245_CloseLogicalChannel&)(H245_CloseLogicalChannel&)closeReq;
                clc.m_forwardLogicalChannelNumber = chanNum;
                clc.m_source.SetTag(H245_CloseLogicalChannel_source::e_lcse);
                WriteControlPDU(closePdu);
                spdlog::info("RecorderConnection: closeLogicalChannel sent for raw OLC "
                             "channel {}", chanNum);
            } catch (...) {
                spdlog::warn("RecorderConnection: closeLogicalChannel send failed for "
                             "channel {}", chanNum);
            }
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
        const H245_CloseLogicalChannelAck& clcAck =
            (const H245_CloseLogicalChannelAck&)(const H245_CloseLogicalChannelAck&)resp;
        int ackChan = (int)clcAck.m_forwardLogicalChannelNumber;
        spdlog::info("RecorderConnection: received closeLogicalChannelAck channel={}", ackChan);
        // 我们自己 close 的 raw OLC（>=100）的 ack — 直接吸收，避免 base 找不到 channel 报错
        if (ackChan >= 100) {
            return TRUE;
        }
        // 其它 ack 交给 base
    }
    // ── conferenceResponse (tag=20): MCU 在通话开始时会推送 terminalListResponse、
    //   requestAllTerminalIDsResponse 这类会议成员通报信息。PTLib 基类对这些
    //   "未关联请求"的 conferenceResponse 默认回 indication: functionNotUnderstood，
    //   这正是 0429 16:46:28 抓包 frame 341/342 的源头。
    //   MCU 收到 functionNotUnderstood 后会判定我们不是华为终端，之后 SMC
    //   "发送/停止演示"操作再也不会向我们下发 070E nonStandardCommand。
    //   解决：吞掉所有 conferenceResponse，仅记日志，不回 functionNotUnderstood。
    else if (tag == H245_ResponseMessage::e_conferenceResponse) {
        const H245_ConferenceResponse& cr =
            (const H245_ConferenceResponse&)(const H245_ConferenceResponse&)resp;
        spdlog::info("RecorderConnection: absorbing conferenceResponse subTag={} "
                     "to avoid functionNotUnderstood reply", (int)cr.GetTag());
        return TRUE;
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
    spdlog::info("RecorderConnection: H.239 subscribe attempt {} — sending raw OLC extendedVideoCapability session=10", attempt);

    // ── Strategy: bypass H323Plus::OpenLogicalChannel() entirely ──────
    //
    // 验证结论（2026-04-27）：单纯 SendCapabilitySet(FALSE) 重发 TCS 不能让
    // VP9660 把我们加进 H.239 分发表 —— 内容相同的 TCS 被去重了。必须发一条
    // 它认为"演示相关"的 H.245 消息才会触发重新评估。raw OLC(extendedVideo)
    // 是目前已知唯一可靠的方式，副作用是 VP9660 UI 把我们标为"演示发送方"
    // （"会场发送=是"）。功能优先：保留 raw OLC，UI 显示是次要。
    //
    // H323Plus 的 OpenLogicalChannel() 内部会创建 H323Channel、分配本地 RTP
    // 端口、走 codec 工厂，对 extendedVideoCapability 总是返回 FALSE。
    // 所以我们绕过它，直接用 WriteControlPDU() 把 PDU 发出去。MCU 收到后会
    // 单独开一条 OLC 给我们推 H.239 流，由 CreateLogicalChannel() 处理。
    bool ok = false;
    try {
        H323ControlPDU pdu;
        H245_RequestMessage& req = pdu.Build(H245_RequestMessage::e_openLogicalChannel);
        H245_OpenLogicalChannel& olc = (H245_OpenLogicalChannel&)(H245_OpenLogicalChannel&)req;

        olc.m_forwardLogicalChannelNumber = 100 + attempt;

        auto& fwd = olc.m_forwardLogicalChannelParameters;
        fwd.m_dataType.SetTag(H245_DataType::e_videoData);
        H245_VideoCapability& vc = fwd.m_dataType;
        vc.SetTag(H245_VideoCapability::e_extendedVideoCapability);

        H245_ExtendedVideoCapability& extCap =
            (H245_ExtendedVideoCapability&)(H245_ExtendedVideoCapability&)vc;

        extCap.m_videoCapability.SetSize(1);
        H245_VideoCapability& innerVc = extCap.m_videoCapability[0];
        innerVc.SetTag(H245_VideoCapability::e_genericVideoCapability);

        H245_GenericCapability& h264Gen =
            (H245_GenericCapability&)(H245_GenericCapability&)innerVc;
        h264Gen.m_capabilityIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& h264Oid = (PASN_ObjectId&)(PASN_ObjectId&)h264Gen.m_capabilityIdentifier;
        h264Oid.SetValue("0.0.8.241.0.0.1");
        h264Gen.IncludeOptionalField(H245_GenericCapability::e_maxBitRate);
        h264Gen.m_maxBitRate = 4096;

        extCap.IncludeOptionalField(H245_ExtendedVideoCapability::e_videoCapabilityExtension);
        extCap.m_videoCapabilityExtension.SetSize(1);
        H245_GenericCapability& h239Gen = extCap.m_videoCapabilityExtension[0];
        h239Gen.m_capabilityIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& h239Oid = (PASN_ObjectId&)(PASN_ObjectId&)h239Gen.m_capabilityIdentifier;
        h239Oid.SetValue("0.0.8.239.1.2");

        fwd.m_multiplexParameters.SetTag(
            H245_OpenLogicalChannel_forwardLogicalChannelParameters_multiplexParameters::e_h2250LogicalChannelParameters);

        H245_H2250LogicalChannelParameters& h2250fwd =
            (H245_H2250LogicalChannelParameters&)(H245_H2250LogicalChannelParameters&)
                fwd.m_multiplexParameters;
        h2250fwd.m_sessionID = 10;

        // 不填 reverseLogicalChannelParameters：我们是 receive-only，加上反向参数
        // 会让 H323Plus 把 Ack 当成双向通道打开，找不到内部 channel 触发断言。

        WriteControlPDU(pdu);
        ok = true;
        spdlog::info("RecorderConnection: raw OLC(extendedVideo, session=10) sent successfully");
    } catch (const std::exception& e) {
        spdlog::error("RecorderConnection: exception constructing raw OLC: {}", e.what());
    } catch (...) {
        spdlog::error("RecorderConnection: unknown exception constructing raw OLC");
    }

    if (!ok) {
        spdlog::warn("RecorderConnection: raw OLC send failed on attempt {}", attempt);
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
