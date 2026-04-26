// PTLib must be the very first include
#include <ptlib.h>
#include <h323.h>
#include <h235auth.h>
#include <h323pdu.h>         // H323SetAliasAddress
#include <ptclib/cypher.h>   // PMessageDigest5

#include "RecorderEndpoint.h"

// ─── Huawei-compatible H.235 MD5 authenticator ───────────────────────────────
class H235AuthHuaweiMD5 : public H235AuthSimpleMD5
{
    PCLASSINFO(H235AuthHuaweiMD5, H235AuthSimpleMD5);
public:
    H235AuthHuaweiMD5() {}
    PObject* Clone() const override { return new H235AuthHuaweiMD5(*this); }

    H225_CryptoH323Token* CreateCryptoToken() override
    {
        PMessageDigest5 md5;
        PString pw = GetPassword();
        md5.Process((const char*)pw, pw.GetLength());
        PMessageDigest5::Code digest;
        md5.Complete(digest);

        H225_CryptoH323Token* token = new H225_CryptoH323Token();
        token->SetTag(H225_CryptoH323Token::e_cryptoEPPwdHash);
        H225_CryptoH323Token_cryptoEPPwdHash& pwdHash =
            (H225_CryptoH323Token_cryptoEPPwdHash&)*token;
        H323SetAliasAddress(GetLocalId(), pwdHash.m_alias);
        pwdHash.m_timeStamp = (unsigned)PTime().GetTimeInSeconds();
        pwdHash.m_token.m_algorithmOID = "1.2.840.113549.2.5";
        pwdHash.m_token.m_hash.SetData(sizeof(digest)*8, (const BYTE*)&digest);
        return token;
    }
};
#include "RecorderEndpoint.h"
#include "RecorderConnection.h"
#include "H264RecvCodec.h"
#include "H261RecvCodec.h"
#include "AACLDRecvCap.h"
#include <h323pluginmgr.h>
#include <h323caps.h>   // H239Control / H323CodecExtendedVideoCapability
#include <g711.h>
#include "../media/VideoCapturePChannel.h"
#include "../media/VideoSourceChannel.h"
#include "../media/AudioCapturePChannel.h"
#include "../media/FfmpegRecorder.h"
#include "../meeting/MeetingRegistry.h"
#include <spdlog/spdlog.h>
#include <chrono>

// ─── PTLib thread: sleeps then re-dials ──────────────────────────────────────
class ReconnectThread : public PThread
{
public:
    ReconnectThread(RecorderEndpoint& ep, int delaySec)
        : PThread(1000, AutoDeleteThread, NormalPriority, "Reconnect"),
          ep_(ep), delaySec_(delaySec)
    {
        Resume();
    }
    void Main() override
    {
        Sleep(delaySec_ * 1000);
        ep_.dial();
    }
private:
    RecorderEndpoint& ep_;
    int               delaySec_;
};

// ─────────────────────────────────────────────────────────────────────────────
RecorderEndpoint::RecorderEndpoint(const AppConfig& cfg)
    : cfg_(cfg)
    , meetingRegistry_(std::make_unique<MeetingRegistry>(cfg.recorder.output_dir))
{
    SetLocalUserName("RecorderNode");

    // Disable Fast Start: VP9660 spends ~6 s waiting for FS acks;
    // disabling gets Connect in ~100 ms.
    DisableFastStart(TRUE);
    // Keep H.245 on a separate TCP channel (not tunneled)
    DisableH245Tunneling(TRUE);

    // Set RTP port range for media sessions
    unsigned base = cfg_.recorder.rtp_port_base;
    unsigned max  = base + 200;  // enough for audio + video + H.239 sessions
    SetRtpIpPorts(base, max);
    spdlog::info("RecorderEndpoint: RTP port range set to {}-{}", base, max);
}

RecorderEndpoint::~RecorderEndpoint()
{
    stop();
}

// ─── Capability registration ─────────────────────────────────────────────────
bool RecorderEndpoint::start()
{
    // ── Register receive capabilities ────────────────────────────────────
    // Layout (all in descriptor 0 so IsAllowed(audio,video)=TRUE):
    //   sim-set 0: AAC-LD, G.722, G.711-ALaw, G.711-uLaw  (audio alternatives)
    //   sim-set 1: H.264 (HP), H.264 (BP/MP), H.261 (video alternatives)
    //   sim-set 2: H.239(H.261)(H.264)            (VP9660 H239CapsCmp #1)
    //   sim-set 3: H.239 Control                  (VP9660 H239CapsCmp #2)

    SetCapability(0, 0, new H323_AACLD_RecvCap());
    
    // Load G.722 from H323Plus capability dictionary. 
    // H323Plus maintains a registry of capabilities.
    // Add G.722 capability from plugins
    AddAllCapabilities(0, 0, "G.722-64k");


    SetCapability(0, 0, new H323_G711Capability(H323_G711Capability::ALaw,
                                                H323_G711Capability::At64k));
    SetCapability(0, 0, new H323_G711Capability(H323_G711Capability::muLaw,
                                                H323_G711Capability::At64k));

    SetCapability(0, 1, new H323_H264RecvCap(8));    // High Profile
    SetCapability(0, 1, new H323_H264RecvCap(64));   // Baseline Profile
    SetCapability(0, 1, new H323_H261RecvCap());

    // H.239 Extended Video (OID 0.0.8.239.1.2): VP9660's H239CapsCmp check
    // requires the remote to advertise extendedVideoCapability. Wrap our plain
    // H.261/H.264 inside H323CodecExtendedVideoCapability so VP9660 sees it.
    {
        auto* extH261 = new H323CodecExtendedVideoCapability();
        extH261->AddCapability(new H323_H261RecvCap());
        SetCapability(0, 2, extH261);

        auto* extH264HP = new H323CodecExtendedVideoCapability();
        extH264HP->AddCapability(new H323_H264RecvCap(8));
        SetCapability(0, 2, extH264HP);

        auto* extH264BP = new H323CodecExtendedVideoCapability();
        extH264BP->AddCapability(new H323_H264RecvCap(64));
        SetCapability(0, 2, extH264BP);
    }

    // H.239 Control (OID 0.0.8.239.1.1)
    SetCapability(0, 3, new H239Control());

    // ── Start H.323 listener ─────────────────────────────────────────────
    int h323Port = cfg_.gk.h323_port > 0 ? cfg_.gk.h323_port : 1720;
    std::string listenStr = "tcp$*:" + std::to_string(h323Port);
    if (!StartListeners(PStringList(listenStr.c_str()))) {
        spdlog::error("RecorderEndpoint: failed to start listener on TCP:{}", h323Port);
        return false;
    }
    spdlog::info("RecorderEndpoint: listening on TCP:{}", h323Port);

    // Set alias
    AddAliasName(PString(cfg_.gk.alias.c_str()));
    if (!cfg_.gk.e164.empty())
        SetLocalUserName(PString(cfg_.gk.e164.c_str()));

    // Register with GK
    if (!cfg_.gk.host.empty()) {
        if (!cfg_.gk.password.empty()) {
            SetGatekeeperPassword(PString(cfg_.gk.password.c_str()));
        }

        if (!UseGatekeeper(PString(cfg_.gk.host.c_str()))) {
            H323Gatekeeper* gk = GetGatekeeper();
            if (gk) {
                unsigned baseReason = (unsigned)gk->GetRegistrationFailReason()
                                      & ~(unsigned)H323Gatekeeper::RegistrationRejectReasonMask;
                const char* reasonStr = "Unknown";
                switch (baseReason) {
                    case H323Gatekeeper::DuplicateAlias:             reasonStr = "DuplicateAlias"; break;
                    case H323Gatekeeper::SecurityDenied:             reasonStr = "SecurityDenied"; break;
                    case H323Gatekeeper::TransportError:             reasonStr = "TransportError"; break;
                    default: break;
                }
                spdlog::warn("RecorderEndpoint: GK registration FAILED — {}", reasonStr);
            } else {
                spdlog::warn("RecorderEndpoint: GK registration failed (transport/discovery)");
            }
        } else {
            registered_ = true;
            spdlog::info("RecorderEndpoint: registered with GK {} alias='{}'",
                         cfg_.gk.host, cfg_.gk.alias);
        }
    }

    // ── Auto-dial if configured ──────────────────────────────────────────
    if (cfg_.outgoing.enabled && !cfg_.outgoing.dial_number.empty()) {
        spdlog::info("RecorderEndpoint: outgoing call enabled — dialing {}",
                     cfg_.outgoing.dial_number);
        dial();
    }

    return true;
}

// ─── Outgoing call ────────────────────────────────────────────────────────────
std::string RecorderEndpoint::dial()
{
    if (stopping_) return {};

    // Build destination: "h323:number@host" or just "number" (GK routes it)
    std::string dest = cfg_.outgoing.dial_number;
    if (!cfg_.outgoing.mcu_host.empty())
        dest = "h323:" + cfg_.outgoing.dial_number + "@" + cfg_.outgoing.mcu_host;

    PString token;
    H323Connection* conn = MakeCall(PString(dest.c_str()), token);
    if (conn == nullptr || token.IsEmpty()) {
        spdlog::error("RecorderEndpoint: MakeCall({}) failed", dest);
        return {};
    }
    currentToken_ = token;
    // For outgoing calls OnAnswerCall is never invoked on our side, so
    // populate callerId from the dialed number — it's the meeting key.
    if (auto* rc = dynamic_cast<RecorderConnection*>(conn)) {
        rc->setCallerId(cfg_.outgoing.dial_number);
    }
    spdlog::info("RecorderEndpoint: outgoing call to '{}' token={}",
                 dest, (const char*)token);
    return std::string(token.GetPointer());
}

// ─── Manual outgoing call (explicit destination) ─────────────────────────────
std::string RecorderEndpoint::dialTo(const std::string& number, const std::string& host)
{
    if (stopping_) return {};
    if (number.empty()) {
        spdlog::error("RecorderEndpoint: dialTo — number is empty");
        return {};
    }

    std::string dest = number;
    if (!host.empty())
        dest = "h323:" + number + "@" + host;

    PString token;
    H323Connection* conn = MakeCall(PString(dest.c_str()), token);
    if (conn == nullptr || token.IsEmpty()) {
        spdlog::error("RecorderEndpoint: dialTo({}) failed", dest);
        return {};
    }
    currentToken_ = token;
    if (auto* rc = dynamic_cast<RecorderConnection*>(conn)) {
        rc->setCallerId(number);
    }
    spdlog::info("RecorderEndpoint: manual outgoing call to '{}' token={}",
                 dest, (const char*)token);
    return std::string(token.GetPointer());
}

void RecorderEndpoint::stop()
{
    stopping_ = true;
    if (registered_) {
        RemoveGatekeeper();
        registered_ = false;
    }
    ClearAllCalls(H323Connection::EndedByLocalUser, TRUE);
    spdlog::info("RecorderEndpoint: stopped");
}

// ─── Connection lifecycle ─────────────────────────────────────────────────────
H323Connection* RecorderEndpoint::CreateConnection(
    unsigned callReference, void* /*userData*/)
{
    return new RecorderConnection(callReference, *this);
}

H323Connection::AnswerCallResponse
RecorderEndpoint::OnAnswerCall(H323Connection& conn,
                               const PString& callerName,
                               const H323SignalPDU& setupPDU,
                               H323SignalPDU& /*connectPDU*/)
{
    spdlog::info("RecorderEndpoint: incoming call from '{}'",
                 (const char*)callerName);

    auto* rc = dynamic_cast<RecorderConnection*>(&conn);
    if (rc) {
        rc->setCallerId(std::string((const char*)callerName));
        
        // Extract DisplayInformation (Meeting Name) from Setup PDU
        const Q931& q931 = setupPDU.GetQ931();
        PString displayInfo = q931.GetDisplayName();
        if (!displayInfo.IsEmpty()) {
            rc->setMeetingName(std::string((const char*)displayInfo));
            spdlog::info("RecorderEndpoint: meeting name '{}'", (const char*)displayInfo);
        } else {
            spdlog::info("RecorderEndpoint: no display info found in Setup PDU");
        }
    }

    return H323Connection::AnswerCallNow;
}

void RecorderEndpoint::OnConnectionEstablished(
    H323Connection& conn, const PString& token)
{
    currentToken_ = token;
    reconnectCount_ = 0;
    spdlog::info("RecorderEndpoint: call established token={}",
                 (const char*)token);

    // 方案 4：TCS 重发触发 H.239 OLC，probe 已移至 RecorderConnection::OnCapRefreshTimer。
    // 原 SendGenericMessage(e_h245request) 发送的是 presentationTokenRequest (subMsg=3)，
    // 语义错误，VP9660 必然拒绝，故移除。
}

void RecorderEndpoint::OnConnectionCleared(
    H323Connection& conn, const PString& clearedToken)
{
    H323Connection::CallEndReason reason = conn.GetCallEndReason();
    spdlog::info("RecorderEndpoint: call cleared token={} reason={}",
                 (const char*)clearedToken, (int)reason);

    // Clear active-call state so status/isInCall reflects reality.
    // Only reset if the cleared token is still the one we track — a new
    // dial() may already have overwritten currentToken_ with a newer call.
    if (currentToken_ == clearedToken) {
        currentToken_ = PString::Empty();
    }

    // Reconnect logic for outgoing calls
    if (!stopping_
        && cfg_.outgoing.enabled
        && !cfg_.outgoing.dial_number.empty()
        && cfg_.outgoing.reconnect)
    {
        int maxR = cfg_.outgoing.max_reconnects;
        int cur  = reconnectCount_.fetch_add(1);
        if (maxR < 0 || cur < maxR) {
            spdlog::info("RecorderEndpoint: reconnecting in {}s (attempt {})",
                         cfg_.outgoing.reconnect_delay_s, cur + 1);
            new ReconnectThread(*this, cfg_.outgoing.reconnect_delay_s);
        } else {
            spdlog::warn("RecorderEndpoint: max reconnects ({}) reached, giving up", maxR);
        }
    }
}

// ─── Media channels ───────────────────────────────────────────────────────────
PBoolean RecorderEndpoint::OpenVideoChannel(
    H323Connection& conn, PBoolean isEncoding, H323VideoCodec& codec)
{
    // ── Encode side: attach screen-saver source (for two-instance self-test) ─
    if (isEncoding) {
        unsigned w = codec.GetWidth();
        unsigned h = codec.GetHeight();
        if (w == 0 || h == 0) { w = 352; h = 288; }
        spdlog::info("RecorderEndpoint: opening video ENCODE source {}x{} codec='{}'",
                     w, h, (const char*)codec.GetMediaFormat());
        auto* ch = new VideoSourceChannel(w, h, cfg_.recorder.video_fps);
        return codec.AttachChannel(ch, TRUE);
    }

    unsigned sessionId = 2;
    if (codec.GetLogicalChannel() != nullptr) {
        sessionId = codec.GetLogicalChannel()->GetSessionID();
    }

    // Session 2 = primary video; any other session (VP9660 uses 10 for H.239)
    // is a secondary/extended video stream. PTLib normally routes these to
    // OpenExtendedVideoChannel directly, but guard here just in case.
    if (sessionId != 2) {
        spdlog::info("RecorderEndpoint: OpenVideoChannel session={} (non-primary) → routing to OpenExtendedVideoChannel",
                     sessionId);
        return OpenExtendedVideoChannel(conn, isEncoding, codec);
    }

    auto* rc = dynamic_cast<RecorderConnection*>(&conn);
    if (!rc || !rc->recorderPtr()) {
        spdlog::warn("RecorderEndpoint: OpenVideoChannel — no recorder");
        return FALSE;
    }

    unsigned w = codec.GetWidth();
    unsigned h = codec.GetHeight();
    if (w == 0 || h == 0) {
        bool isH264 = (codec.GetMediaFormat() == "H.264");
        // Force 1920x1080 for H.264 since MCU will send 1080p and FfmpegRecorder
        // needs the max resolution at initialization to avoid blur.
        w = isH264 ? 1920 : 352;
        h = isH264 ? 1080 : 288;
    }

    spdlog::info("RecorderEndpoint: opening primary video {}x{} codec='{}'",
                 w, h, (const char*)codec.GetMediaFormat());
    auto* ch = new VideoCapturePChannel(rc->recorderPtr(), w, h);
    PBoolean ok = codec.AttachChannel(ch, TRUE);

    // Ask MCU to send an I-frame immediately (H.245 videoFastUpdatePicture).
    // Without this the MCU may only send inter-frames until the next intra period.
    if (ok) {
        codec.SendMiscCommand(H245_MiscellaneousCommand_type::e_videoFastUpdatePicture);
        spdlog::info("RecorderEndpoint: sent videoFastUpdatePicture (primary video)");
    }
    return ok;
}

// H.239 secondary (presentation/content) video channel
PBoolean RecorderEndpoint::OpenExtendedVideoChannel(
    H323Connection& conn, PBoolean isEncoding, H323VideoCodec& codec)
{
    // Encode side for H.239: not used (aux is sent via VideoSender raw UDP)
    if (isEncoding) return FALSE;

    auto* rc = dynamic_cast<RecorderConnection*>(&conn);
    if (!rc || !rc->auxRecorderPtr()) {
        spdlog::warn("RecorderEndpoint: OpenExtendedVideoChannel — no aux recorder");
        return FALSE;
    }

    unsigned w = codec.GetWidth();
    unsigned h = codec.GetHeight();
    if (w == 0 || h == 0) { w = 1280; h = 720; }

    // ── Aux segmentation ──────────────────────────────────────────────────
    // Every H.239 OLC opens a NEW aux segment (aux_NN.mp4). If a previous
    // aux file is still open here, the MCU sent a new OLC without first
    // sending a CLC — a common presenter-switch pattern on VP9660. We
    // close the previous segment first so the meeting.json duration is
    // accurate.
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (rc->auxRecorderPtr()->isOpen()) {
        spdlog::info("RecorderEndpoint: aux still open on new OLC — "
                     "closing previous segment {}", rc->activeAuxPath());
        rc->closeActiveAux(nowMs);
    }

    const auto& cfg = config().recorder;
    std::string auxPath = rc->allocateAuxPath();
    if (auxPath.empty()) {
        // Fallback: legacy naming if meeting context not available
        auxPath = rc->buildOutputPath("_aux");
    }

    if (!rc->auxRecorderPtr()->open(auxPath,
                                    cfg.video_width, cfg.video_height, cfg.video_fps,
                                    cfg.audio_sample_rate, cfg.audio_channels, false)) {
        spdlog::error("RecorderEndpoint: failed to open aux recorder {}", auxPath);
        return FALSE;
    }
    rc->recordAuxStart(nowMs);
    spdlog::info("RecorderEndpoint: H.239 aux recording started to {}", auxPath);

    spdlog::info("RecorderEndpoint: opening H.239 extended video {}x{} codec='{}'",
                 w, h, (const char*)codec.GetMediaFormat());
    
    auto* ch = new VideoCapturePChannel(rc->auxRecorderPtr(), w, h);
    PBoolean ok = codec.AttachChannel(ch, TRUE);

    if (ok) {
        codec.SendMiscCommand(H245_MiscellaneousCommand_type::e_videoFastUpdatePicture);
        spdlog::info("RecorderEndpoint: sent videoFastUpdatePicture (H.239 extended video)");
    }
    return ok;
}

PBoolean RecorderEndpoint::OpenAudioChannel(
    H323Connection& conn, PBoolean isEncoding,
    unsigned /*bufferSize*/, H323AudioCodec& codec)
{
    if (isEncoding) return FALSE;

    auto* rc = dynamic_cast<RecorderConnection*>(&conn);
    if (!rc || !rc->recorderPtr()) {
        spdlog::warn("RecorderEndpoint: OpenAudioChannel — no recorder");
        return FALSE;
    }

    int sampleRate = codec.GetMediaFormat().GetTimeUnits() * 1000;
    if (sampleRate <= 0) sampleRate = cfg_.recorder.audio_sample_rate;

    spdlog::info("RecorderEndpoint: attaching audio capture {}Hz", sampleRate);
    auto* ch = new AudioCapturePChannel(rc->recorderPtr(), sampleRate, 1);
    ch->Open("recorder", PSoundChannel::Player, 1, (unsigned)sampleRate, 16);
    codec.AttachChannel(ch, TRUE);
    return TRUE;
}

void RecorderEndpoint::onRecordingStarted(const PString& token,
                                          const std::string& filePath)
{
    spdlog::info("RecorderEndpoint: recording started token={} file={}",
                 (const char*)token, filePath);
}

void RecorderEndpoint::onRecordingStopped(const PString& token)
{
    spdlog::info("RecorderEndpoint: recording stopped token={}",
                 (const char*)token);
}

bool RecorderEndpoint::startMainVideo()
{
    auto* conn = currentConnection();
    if (!conn) {
        spdlog::warn("RecorderEndpoint::startMainVideo — no active connection");
        return false;
    }
    return conn->startMainVideo();
}

bool RecorderEndpoint::stopMainVideo()
{
    auto* conn = currentConnection();
    if (!conn) {
        spdlog::warn("RecorderEndpoint::stopMainVideo — no active connection");
        return false;
    }
    return conn->stopMainVideo();
}

bool RecorderEndpoint::startPresentation()
{
    auto* conn = currentConnection();
    if (!conn) {
        spdlog::warn("RecorderEndpoint::startPresentation — no active connection");
        return false;
    }
    return conn->startPresentation();
}

bool RecorderEndpoint::stopPresentation()
{
    auto* conn = currentConnection();
    if (!conn) {
        spdlog::warn("RecorderEndpoint::stopPresentation — no active connection");
        return false;
    }
    return conn->stopPresentation();
}

RecorderConnection* RecorderEndpoint::currentConnection()
{
    if (currentToken_.IsEmpty())
        return nullptr;
    H323Connection* conn = FindConnectionWithLock(currentToken_);
    if (conn) {
        conn->Unlock();
        return dynamic_cast<RecorderConnection*>(conn);
    }
    return nullptr;
}

std::string RecorderEndpoint::currentTokenStr()
{
    if (currentToken_.IsEmpty())
        return "";
    return std::string(currentToken_.GetPointer());
}

H235Authenticators RecorderEndpoint::CreateAuthenticators()
{
    H235Authenticators auths;
    if (cfg_.gk.password.empty()) return auths;

    PString localId = cfg_.gk.username.empty()
        ? PString(cfg_.gk.alias.c_str())
        : PString(cfg_.gk.username.c_str());
    PString pw(cfg_.gk.password.c_str());

    H235AuthCAT* cat = new H235AuthCAT();
    cat->SetLocalId(localId);
    cat->SetPassword(pw);
    auths.Append(cat);

    H235AuthHuaweiMD5* hwMd5 = new H235AuthHuaweiMD5();
    hwMd5->SetLocalId(localId);
    hwMd5->SetPassword(pw);
    auths.Append(hwMd5);

    spdlog::info("RecorderEndpoint: H.235 auth (CAT+Huawei-MD5) id='{}'",
                 (const char*)localId);
    return auths;
}
