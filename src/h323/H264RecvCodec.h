#pragma once
// PTLib must be first
#include <ptlib.h>
#include <h323.h>
#include <h245.h>

#include <vector>
#include <cstdint>

// Forward-declare FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

/**
 * H264RecvCodec — H.264 receive-only codec for H.323Plus.
 *
 * De-packetizes RFC 6184 RTP (single NAL, FU-A, STAP-A), decodes with
 * FFmpeg libavcodec (AV_CODEC_ID_H264), and writes raw YUV420P frames
 * to rawDataChannel (→ VideoCapturePChannel → FfmpegRecorder).
 */
#include <mutex>

class H264RecvCodec : public H323VideoCodec
{
    PCLASSINFO(H264RecvCodec, H323VideoCodec);
public:
    explicit H264RecvCodec(Direction dir);
    ~H264RecvCodec() override;

    void Close() override;

    // Encoder path — unused (receive-only)
    PBoolean Read(BYTE* /*buf*/, unsigned& length,
                  RTP_DataFrame& /*frame*/) override
    { length = 0; return FALSE; }

    // Decoder path — called per RTP packet with H.264 payload
    PBoolean Write(const BYTE* buf, unsigned length,
                   const RTP_DataFrame& rtpFrame,
                   unsigned& written) override;

private:
    bool initDecoder();
    void decodeAndDeliver();
    void appendNAL(const uint8_t* data, size_t len);

    std::mutex mu_;
    AVCodecContext* decCtx_   = nullptr;
    AVFrame*        decFrame_ = nullptr;
    SwsContext*     swsCtx_   = nullptr;

    std::vector<uint8_t> annexBuf_;
    bool fuInProgress_ = false;
    std::vector<uint8_t> fuBuf_;
    bool decoderReady_ = false;
};


/**
 * H323_H264RecvCap — H.245 genericVideoCapability for H.264.
 *
 * Uses H323GenericVideoCapability as the base so H323Plus correctly:
 *   1. Serialises it into the TCS as a genericVideoCapability entry
 *   2. Matches incoming OLC by OID (0.0.8.241.0.0.1)
 *   3. Calls CreateCodec → OpenVideoChannel when MCU opens H.264
 *
 * OnSendingPDU() fills the H.245 GenericCapability with the H.241 Annex E
 * collapsing parameters (profile + level) required by the VP9660 MCU.
 */
class H323_H264RecvCap : public H323GenericVideoCapability
{
    PCLASSINFO(H323_H264RecvCap, H323GenericVideoCapability);
public:
    // OID 0.0.8.241.0.0.1 = H.264 per ITU-T H.241 Annex A
    // maxBitRate 18560 = 1856 kbps (units: 100 bps)
    H323_H264RecvCap(unsigned profileMask = 8)
        : H323GenericVideoCapability("0.0.8.241.0.0.1", 18560), m_profileMask(profileMask) {}

    PObject* Clone() const override { return new H323_H264RecvCap(*this); }
    PString  GetFormatName() const override { 
        return (m_profileMask == 8) ? "H.264(HP)" : "H.264(BP)"; 
    }

    // ── TCS: advertise H.264 with H.241 profile/level parameters ─────────
    // H323Plus calls the 2-argument version for TCS PDU building.
    PBoolean OnSendingPDU(H245_VideoCapability& pdu,
                          CommandType /*ct*/) const override
    {
        pdu.SetTag(H245_VideoCapability::e_genericVideoCapability);
        H245_GenericCapability& gen =
            (H245_GenericCapability&)(const H245_GenericCapability&)pdu;

        // Capability identifier: H.264 OID per ITU-T H.241 Annex A
        gen.m_capabilityIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& oid =
            (PASN_ObjectId&)(const PASN_ObjectId&)gen.m_capabilityIdentifier;
        oid = "0.0.8.241.0.0.1";

        // maxBitRate: 1920 kbps (unit = 100 bps, so 19200)
        // Set this exactly to the 1.92M template
        gen.IncludeOptionalField(H245_GenericCapability::e_maxBitRate);
        gen.m_maxBitRate = 19200;

        // Collapsing parameters per ITU-T H.241 Table 1 (and matching MCU's 1080p60 format)
        gen.IncludeOptionalField(H245_GenericCapability::e_collapsing);
        H245_ArrayOf_GenericParameter& cp = gen.m_collapsing;
        cp.SetSize(4);

        // Parameter ID 41 — Profile (booleanArray)
        cp[0].m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        (PASN_Integer&)(const PASN_Integer&)cp[0].m_parameterIdentifier = 41;
        cp[0].m_parameterValue.SetTag(H245_ParameterValue::e_booleanArray);
        (PASN_Integer&)(const PASN_Integer&)cp[0].m_parameterValue = m_profileMask;

        // Parameter ID 42 — Level (unsignedMin)
        // MCU uses 85 (Level 4.0)
        cp[1].m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        (PASN_Integer&)(const PASN_Integer&)cp[1].m_parameterIdentifier = 42;
        cp[1].m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        (PASN_Integer&)(const PASN_Integer&)cp[1].m_parameterValue = 85;

        // Parameter ID 3 — CustomMaxMBPS (unsignedMin)
        // 984 * 500 = 492,000 MBPS (covers 1080p60)
        cp[2].m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        (PASN_Integer&)(const PASN_Integer&)cp[2].m_parameterIdentifier = 3;
        cp[2].m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        (PASN_Integer&)(const PASN_Integer&)cp[2].m_parameterValue = 984;

        // Parameter ID 4 — CustomMaxFS (unsignedMin)
        // 32 * 256 = 8,192 FS (covers 1080p)
        cp[3].m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        (PASN_Integer&)(const PASN_Integer&)cp[3].m_parameterIdentifier = 4;
        cp[3].m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        (PASN_Integer&)(const PASN_Integer&)cp[3].m_parameterValue = 32;

        // =================================================================
        // No nonStandard parameter hacks here.
        // =================================================================
        
        return TRUE;
    }

    PBoolean OnSendingPDU(H245_VideoMode& pdu) const override
    {
        pdu.SetTag(H245_VideoMode::e_genericVideoMode);
        return TRUE;
    }

    // ── OLC matching: called when MCU opens a video logical channel ───────
    PBoolean OnReceivedPDU(const H245_DataType& pdu,
                           PBoolean /*receiver*/) override
    {
        if (pdu.GetTag() != H245_DataType::e_videoData)
            return FALSE;
        const H245_VideoCapability& vc = pdu;
        if (vc.GetTag() != H245_VideoCapability::e_genericVideoCapability)
            return FALSE;
        const H245_GenericCapability& gen =
            (const H245_GenericCapability&)(const H245_GenericCapability&)vc;
        if (gen.m_capabilityIdentifier.GetTag() != H245_CapabilityIdentifier::e_standard)
            return FALSE;
        const PASN_ObjectId& oid =
            (const PASN_ObjectId&)(const PASN_ObjectId&)gen.m_capabilityIdentifier;
        return oid == "0.0.8.241.0.0.1";
    }

    PBoolean IsMatch(const PASN_Choice& subTypePDU) const override
    {
        const H245_VideoCapability& vc = (const H245_VideoCapability&)subTypePDU;
        if (vc.GetTag() != H245_VideoCapability::e_genericVideoCapability)
            return FALSE;
        const H245_GenericCapability& gen =
            (const H245_GenericCapability&)(const H245_GenericCapability&)vc;
        if (gen.m_capabilityIdentifier.GetTag() != H245_CapabilityIdentifier::e_standard)
            return FALSE;
        const PASN_ObjectId& oid =
            (const PASN_ObjectId&)(const PASN_ObjectId&)gen.m_capabilityIdentifier;
        return oid == "0.0.8.241.0.0.1";
    }

    PBoolean OnReceivedPDU(const H245_VideoCapability& pdu) override
    {
        return pdu.GetTag() == H245_VideoCapability::e_genericVideoCapability;
    }

    PBoolean OnReceivedPDU(const H245_VideoCapability& pdu,
                            CommandType /*ct*/) override
    {
        return pdu.GetTag() == H245_VideoCapability::e_genericVideoCapability;
    }

    H323Codec* CreateCodec(H323Codec::Direction dir) const override
    {
        if (dir == H323Codec::Encoder) return nullptr;
        return new H264RecvCodec(dir);
    }

private:
    unsigned m_profileMask;
};
