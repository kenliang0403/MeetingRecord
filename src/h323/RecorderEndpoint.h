#pragma once

// PTLib must be first
#include <ptlib.h>
#include <h323.h>

#include "../config/AppConfig.h"
#include <string>
#include <atomic>
#include <memory>

class FfmpegRecorder;
class MeetingRegistry;

/**
 * RecorderEndpoint — H.323 endpoint that can both RECEIVE calls (MCU calls us)
 * and PLACE outgoing calls (we call the MCU conference room).
 *
 * Key capabilities declared in TCS:
 *   sim-set 0: G.711-ALaw + G.711-uLaw  (audio, alternatives)
 *   sim-set 1: H.264 + H.261            (video, alternatives)
 *   sim-set 2: H.239 Extended Video     (VP9660 H239CapsCmp)
 *   sim-set 3: H.239 Control            (VP9660 H239CapsCmp)
 */
class RecorderEndpoint : public H323EndPoint
{
    PCLASSINFO(RecorderEndpoint, H323EndPoint);

public:
    explicit RecorderEndpoint(const AppConfig& cfg);
    ~RecorderEndpoint() override;

    bool start();
    void stop();

    // Place an outgoing call to the configured dial_number (from config).
    // Returns the call token, or empty string on failure.
    std::string dial();

    // Place a manual outgoing call to an explicit destination.
    //   number — E.164/H.323 alias, e.g. "<dial-number>"
    //   host   — optional MCU IP; empty means route via GK
    // Returns the call token, or empty string on failure.
    std::string dialTo(const std::string& number, const std::string& host = {});

    const AppConfig& config() const { return cfg_; }

    // Runtime state getters
    bool isRegistered()  const { return registered_; }
    bool isInCall()      const { return !currentToken_.IsEmpty(); }
    std::string currentTokenStr();  // non-const: PString::GetPointer() is non-const
    int  reconnectCount() const { return reconnectCount_.load(); }

    // Access the current active RecorderConnection (if any)
    // Non-const because H323Plus's FindConnectionWithLock is non-const.
    class RecorderConnection* currentConnection();

    // Called by RecorderConnection
    void onRecordingStarted(const PString& token, const std::string& filePath);
    void onRecordingStopped(const PString& token);

    // Main video sender control — delegates to currentConnection().
    bool startMainVideo();      // send OLC(session=2, H.264) + start VideoSender(ScreenSaver)
    bool stopMainVideo();       // stop VideoSender + send CLC

    // H.239 presenter control — delegates to currentConnection().
    bool startPresentation();   // send OLC(session=10) + start VideoSender(NoSignal)
    bool stopPresentation();    // stop VideoSender + send CLC + release token

    // Meeting-folder registry (owned by the endpoint, lives as long as it).
    MeetingRegistry& meetingRegistry() { return *meetingRegistry_; }

    // Monotonically increasing counter — each established connection gets
    // the next index, used in meeting.json to label main_NN segments.
    int nextConnectionIdx() { return ++connectionCounter_; }

protected:
    // ── H323EndPoint overrides ────────────────────────────────────────────────
    H323Connection* CreateConnection(
        unsigned callReference,
        void* userData = nullptr) override;

    H323Connection::AnswerCallResponse OnAnswerCall(
        H323Connection& conn,
        const PString& callerName,
        const H323SignalPDU& setupPDU,
        H323SignalPDU& connectPDU) override;

    // Called when a call is fully established (H.245 complete)
    void OnConnectionEstablished(
        H323Connection& conn,
        const PString& token) override;

    // Called when a call is cleared — triggers reconnect if configured
    void OnConnectionCleared(
        H323Connection& conn,
        const PString& clearedToken) override;

    PBoolean OpenVideoChannel(
        H323Connection& conn,
        PBoolean isEncoding,
        H323VideoCodec& codec) override;

    // H.239 secondary video channel (presentation / content sharing)
    PBoolean OpenExtendedVideoChannel(
        H323Connection& conn,
        PBoolean isEncoding,
        H323VideoCodec& codec) override;

    PBoolean OpenAudioChannel(
        H323Connection& conn,
        PBoolean isEncoding,
        unsigned bufferSize,
        H323AudioCodec& codec) override;

    H235Authenticators CreateAuthenticators() override;

private:
    // Reconnect helper — runs in a separate PTLib thread so it can sleep
    void reconnectThread();

    const AppConfig& cfg_;
    bool             registered_      = false;
    std::atomic<int> reconnectCount_  {0};
    PString          currentToken_;       // active call token
    std::atomic<bool> stopping_      {false};
    std::atomic<int> connectionCounter_{0};
    std::unique_ptr<MeetingRegistry> meetingRegistry_;
};
