// PTLib must be the very first include
#pragma once
#include <ptlib.h>
#include <h323.h>
#include <h245.h>

#include <vector>
#include <cstdint>

// Forward-declare FFmpeg types to keep this header PTLib-clean
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

/**
 * H261RecvCodec — H.261 receive-only codec for H.323Plus.
 *
 * Receives compressed H.261 RTP payloads (RFC 4587), assembles
 * complete pictures from RTP fragments, decodes with FFmpeg libavcodec,
 * and writes raw YUV420P frames to rawDataChannel (→ VideoCapturePChannel
 * → FfmpegRecorder).
 */
class H261RecvCodec : public H323VideoCodec
{
    PCLASSINFO(H261RecvCodec, H323VideoCodec);
public:
    explicit H261RecvCodec(Direction dir);
    ~H261RecvCodec() override;

    void Close() override;

    // Encoder path — unused (receive-only)
    PBoolean Read(BYTE* /*buf*/, unsigned& length,
                  RTP_DataFrame& /*frame*/) override
    { length = 0; return FALSE; }

    // Decoder path — called per RTP packet with H.261 payload
    PBoolean Write(const BYTE* buf, unsigned length,
                   const RTP_DataFrame& rtpFrame,
                   unsigned& written) override;

private:
    bool initDecoder();
    void decodeAndDeliver(int64_t rtpTimestamp);

    AVCodecContext* decCtx_  = nullptr;
    AVFrame*        decFrame_= nullptr;   // raw decoded frame
    SwsContext*     swsCtx_  = nullptr;   // YUV format conversion

    std::vector<uint8_t> frameBuf_;       // accumulates RTP payloads per picture
    bool decoderReady_ = false;
};


/**
 * H323_H261RecvCap — H.245 capability that advertises H.261 QCIF+CIF receive
 * and creates H261RecvCodec instances.
 */
class H323_H261RecvCap : public H323VideoCapability
{
    PCLASSINFO(H323_H261RecvCap, H323VideoCapability);
public:
    H323_H261RecvCap() {}
    PObject* Clone() const override { return new H323_H261RecvCap(*this); }

    unsigned GetSubType() const override
        { return H245_VideoCapability::e_h261VideoCapability; }
    PString  GetFormatName() const override { return "H.261"; }

    PBoolean OnSendingPDU(H245_VideoCapability& pdu,
                          CommandType /*ct*/) const override
    {
        pdu.SetTag(H245_VideoCapability::e_h261VideoCapability);
        H245_H261VideoCapability& h261 =
            (H245_H261VideoCapability&)(const H245_H261VideoCapability&)pdu;
        h261.IncludeOptionalField(H245_H261VideoCapability::e_qcifMPI);
        h261.m_qcifMPI = 1;
        h261.IncludeOptionalField(H245_H261VideoCapability::e_cifMPI);
        h261.m_cifMPI  = 1;
        h261.m_maxBitRate             = 1920;   // 192 kbps
        h261.m_stillImageTransmission = FALSE;
        return TRUE;
    }

    PBoolean OnSendingPDU(H245_VideoMode& pdu) const override
    {
        pdu.SetTag(H245_VideoMode::e_h261VideoMode);
        return TRUE;
    }

    // Called from OLC path: OnReceivedPDU(H245_DataType) → OnReceivedPDU(H245_VideoCapability)
    // Base class returns FALSE by default — must override both signatures.
    PBoolean OnReceivedPDU(const H245_VideoCapability& pdu) override
    {
        return pdu.GetTag() == H245_VideoCapability::e_h261VideoCapability;
    }

    PBoolean OnReceivedPDU(const H245_VideoCapability& pdu,
                            CommandType /*ct*/) override
    {
        return pdu.GetTag() == H245_VideoCapability::e_h261VideoCapability;
    }

    H323Codec* CreateCodec(H323Codec::Direction dir) const override
    {
        if (dir == H323Codec::Encoder) return nullptr;
        return new H261RecvCodec(dir);
    }
};
