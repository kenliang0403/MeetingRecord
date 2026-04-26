#pragma once

// PTLib must be first
#include <ptlib.h>
#include <h323.h>

#include <atomic>
#include <memory>
#include <string>

class RecorderEndpoint;
class FfmpegRecorder;
class MeetingContext;
class SrsStreamer;
class VideoSender;

/**
 * RecorderConnection — one H.323 call session.
 * Owns a FfmpegRecorder; the endpoint's Open*Channel overrides access it.
 */
class RecorderConnection : public H323Connection
{
    PCLASSINFO(RecorderConnection, H323Connection);

public:
    RecorderConnection(unsigned callReference,
                       RecorderEndpoint& ep,
                       unsigned options = 0);
    ~RecorderConnection() override;

    // Return raw pointer for use by endpoint's Open*Channel overrides
    std::shared_ptr<FfmpegRecorder> recorderPtr() const { return recorder_; }
    std::shared_ptr<FfmpegRecorder> auxRecorderPtr() const { return auxRecorder_; }

    void setMeetingName(const std::string& name) { meetingName_ = name; }
    void setCallerId(const std::string& callerId) { callerId_ = callerId; }

    // Runtime state getters (defined in RecorderConnection.cpp)
    bool        hasH239()           const;
    bool        isRecording()       const;
    bool        isAuxRecording()    const;
    bool        hasPresentation()   const { return h239SendActive_.load(); }
    std::string mainFilePath()      const;
    std::string auxFilePath()       const;

    // Main video send commands (auto-started on outgoing call, or manual).
    // startMainVideo() sends OLC(session=2, H.264) + starts VideoSender(ScreenSaver).
    // stopMainVideo() stops VideoSender + sends CLC.
    bool startMainVideo();
    bool stopMainVideo();
    bool isMainSending() const { return mainSendActive_.load(); }

    // H.239 presenter commands (called from TCP control handler).
    // startPresentation() sends presentationTokenRequest + H.239 OLC, then
    // VideoSender starts after MCU acks. stopPresentation() tears everything down.
    bool startPresentation();
    bool stopPresentation();
    std::string meetingName()    const { return meetingName_; }
    std::string callerId()       const { return callerId_; }

    std::string buildOutputPath(const std::string& suffix = "") const;

    // Meeting-folder integration. meetingCtx() is non-null only between
    // OnEstablished and OnCleared. connectionIdx() is the monotonic id
    // this connection holds within the endpoint (written into meeting.json).
    std::shared_ptr<MeetingContext> meetingCtx() const { return meeting_; }
    int  connectionIdx()  const { return connectionIdx_; }

    // Called by RecorderEndpoint::OpenExtendedVideoChannel to allocate /
    // finalize aux segment file paths. The connection tracks the currently
    // open aux path so we can close it from OnClosedLogicalChannel / OnCleared.
    std::string allocateAuxPath();                       // side-effect: activeAuxPath_ set
    void        recordAuxStart(int64_t wallStartMs);     // uses activeAuxPath_
    void        closeActiveAux(int64_t wallEndMs);       // closes recorder + records end
    const std::string& activeAuxPath() const { return activeAuxPath_; }

protected:
    void OnEstablished() override;
    void OnCleared()     override;

    // Detect H.239 logical-channel close (session=10). When the MCU tears
    // down the presentation channel (presenter stopped sharing) we finalize
    // the current aux segment immediately rather than waiting for call end.
    void OnClosedLogicalChannel(const H323Channel& channel) override;

    // Override to allow any inbound receive channel regardless of capability
    // descriptor constraints.  The default H323Connection::OnCreateLogicalChannel
    // rejects inbound video with dataTypeALCombinationNotSupported when the
    // audio+video caps are not in the same descriptor simultaneous-set pair.
    // As a receive-only recorder we accept everything the MCU offers us.
    PBoolean OnCreateLogicalChannel(const H323Capability& capability,
                                    H323Channel::Directions dir,
                                    unsigned& errorCode) override;

    H323Channel* CreateRealTimeLogicalChannel(
                                    const H323Capability& capability,
                                    H323Channel::Directions dir,
                                    unsigned sessionID,
                                    const H245_H2250LogicalChannelParameters* param,
                                    RTP_QOS* rtpqos) override;
    H323Channel* CreateLogicalChannel(const H245_OpenLogicalChannel& open,
                                      PBoolean startingFast,
                                      unsigned& errorCode) override;

    // Respond to VP9660's enterH243TerminalID conference request.
    // Without this, H.323Plus returns functionNotUnderstood, which may prevent
    // VP9660 from including us in the H.239 distribution list for late-joining.
    PBoolean OnHandleConferenceRequest(const H245_ConferenceRequest& req) override;

    // Capture MCU-assigned H.243 labels (terminalNumberAssign / mCUNumber)
    // so we can echo them back verbatim in terminalIDResponse. Without this,
    // we have no way to know our terminalNumber and must reply 0/0, which
    // strict MCUs may reject (Huawei VP9660 tolerates it; Polycom/Pexip may not).
    PBoolean OnHandleConferenceIndication(const H245_ConferenceIndication& ind) override;

    // Intercept H.245 responses (openLogicalChannelAck/Reject) for our raw OLC.
    // Since we send the OLC via WriteControlPDU bypassing H323Plus channel
    // management, the default handler would try to find an internal channel
    // object and crash with "Invalid cast to non-descendant class" assertion.
    // By overriding, we can safely absorb the Ack/Reject for our raw OLCs.
    PBoolean OnH245Response(const H323ControlPDU& pdu) override;

    // Intercept H.245 requests: catch H.239 presentationTokenAck (genericResponse
    // subMessage=2) from MCU so we know we're authorized to send the OLC.
    PBoolean OnH245Request(const H323ControlPDU& pdu) override;

    // Phase-2 helper: send raw H.239 OLC after token is granted.
    void sendH239OLC();

private:
    RecorderEndpoint&               ep_;
    std::shared_ptr<FfmpegRecorder> recorder_;
    std::shared_ptr<FfmpegRecorder> auxRecorder_;
    // Live RTMP streamers. main: created in OnEstablished, lives till OnCleared.
    // aux: created lazily on first aux segment, reused across presenter-stop/
    // restart so the RTMP stream URL stays continuous for viewers.
    std::shared_ptr<SrsStreamer>    mainStreamer_;
    std::shared_ptr<SrsStreamer>    auxStreamer_;

    // Main video raw-UDP sender (ScreenSaver mode, auto-start on outgoing call)
    std::shared_ptr<VideoSender>    mainSender_;
    std::atomic<bool>               mainSendActive_{false};
    static constexpr int            kMainSendChannel = 150;

    // H.239 presenter send state
    std::shared_ptr<VideoSender>    h239Sender_;
    std::atomic<bool>               h239SendActive_{false};
    int                             h239SendChannelNum_{200};  // raw OLC channel number
    std::atomic<bool>               h239TokenGranted_{false};  // MCU replied tokenAck
    std::string                     meetingName_;
    std::string                     callerId_;

    // Meeting folder / metadata tracking
    std::shared_ptr<MeetingContext> meeting_;
    int                             connectionIdx_  = 0;
    std::string                     activeMainPath_;
    std::string                     activeAuxPath_;

    // 方案 4：TCS 重发触发 VP9660 补推 H.239
    PTimer                     capRefreshTimer_;
    std::atomic<int>           capRefreshRetries_{0};
    std::atomic<bool>          h239Received_{false};
    std::atomic<bool>          h239Rejected_{false};   // MCU rejected H.239 — stop retrying
    PDECLARE_NOTIFIER(PTimer, RecorderConnection, OnCapRefreshTimer);

    // H.243 terminal label assigned to us by the MCU (via conferenceIndication
    // terminalNumberAssign / mCUNumber). Echoed back in terminalIDResponse.
    // 0 means "not yet assigned" — we still respond so VP9660 sees liveness.
    std::atomic<unsigned>      h243McuNumber_{0};
    std::atomic<unsigned>      h243TerminalNumber_{0};
};
