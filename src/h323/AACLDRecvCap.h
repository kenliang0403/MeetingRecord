#pragma once
#include <ptlib.h>
#include <h323.h>
#include <h245.h>

/**
 * H323_AACLD_RecvCap — H.245 genericAudioCapability for AAC-LD.
 *
 * Uses H323GenericAudioCapability as the base to advertise ISO/IEC 14496-3 MPEG-4 audio.
 * OID: 0.0.8.245.1.1.11 (As observed in VP9660 TCS for AAC-LD/AAC-LC)
 */
class H323_AACLD_RecvCap : public H323GenericAudioCapability
{
    PCLASSINFO(H323_AACLD_RecvCap, H323GenericAudioCapability);
public:
    H323_AACLD_RecvCap()
        : H323GenericAudioCapability(256, 160, "0.0.8.245.1.1.11") {}

    PObject* Clone() const override { return new H323_AACLD_RecvCap(*this); }
    PString  GetFormatName() const override { return "AAC-LD"; }

    // ── TCS: advertise AAC-LD parameters matching VP9660 ─────────
    PBoolean OnSendingPDU(H245_AudioCapability& pdu) const
    {
        pdu.SetTag(H245_AudioCapability::e_genericAudioCapability);
        H245_GenericCapability& gen =
            (H245_GenericCapability&)(const H245_GenericCapability&)pdu;

        gen.m_capabilityIdentifier.SetTag(H245_CapabilityIdentifier::e_standard);
        PASN_ObjectId& oid =
            (PASN_ObjectId&)(const PASN_ObjectId&)gen.m_capabilityIdentifier;
        // The standard MPEG-4 Audio OID
        oid = "0.0.8.245.1.1.0";

        gen.IncludeOptionalField(H245_GenericCapability::e_maxBitRate);
        gen.m_maxBitRate = 640; // 64k

        gen.IncludeOptionalField(H245_GenericCapability::e_collapsing);
        H245_ArrayOf_GenericParameter& cp = gen.m_collapsing;
        cp.SetSize(2);

        // Parameter ID 0 (standard 2) -> unsignedMin 2
        cp[0].m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        (PASN_Integer&)(const PASN_Integer&)cp[0].m_parameterIdentifier = 2;
        cp[0].m_parameterValue.SetTag(H245_ParameterValue::e_unsignedMin);
        (PASN_Integer&)(const PASN_Integer&)cp[0].m_parameterValue = 2;

        // Parameter ID 1 -> formatType/audioSpecificConfig
        // We pass the hex from the pcap trace
        cp[1].m_parameterIdentifier.SetTag(H245_ParameterIdentifier::e_standard);
        (PASN_Integer&)(const PASN_Integer&)cp[1].m_parameterIdentifier = 4;
        cp[1].m_parameterValue.SetTag(H245_ParameterValue::e_octetString);
        PASN_OctetString& octets = (PASN_OctetString&)(const PASN_OctetString&)cp[1].m_parameterValue;
        
        // 48kHz Mono for AAC-LD: 0xB9 0x88
        BYTE buf[2] = { 0xB9, 0x88 };
        octets.SetValue(buf, 2);

        return TRUE;
    }

    PBoolean OnReceivedPDU(const H245_DataType& pdu,
                           PBoolean /*receiver*/) override
    {
        if (pdu.GetTag() != H245_DataType::e_audioData)
            return FALSE;
        const H245_AudioCapability& vc = (const H245_AudioCapability&)pdu;
        if (vc.GetTag() != H245_AudioCapability::e_genericAudioCapability)
            return FALSE;
        const H245_GenericCapability& gen =
            (const H245_GenericCapability&)(const H245_GenericCapability&)vc;
        if (gen.m_capabilityIdentifier.GetTag() != H245_CapabilityIdentifier::e_standard)
            return FALSE;
        const PASN_ObjectId& oid =
            (const PASN_ObjectId&)(const PASN_ObjectId&)gen.m_capabilityIdentifier;
        return (oid == "0.0.8.245.1.1.0" || oid == "0.0.8.245.1.1.11");
    }

    PBoolean IsMatch(const PASN_Choice& subTypePDU) const override
    {
        const H245_AudioCapability& vc = (const H245_AudioCapability&)subTypePDU;
        if (vc.GetTag() != H245_AudioCapability::e_genericAudioCapability)
            return FALSE;
        const H245_GenericCapability& gen =
            (const H245_GenericCapability&)(const H245_GenericCapability&)vc;
        if (gen.m_capabilityIdentifier.GetTag() != H245_CapabilityIdentifier::e_standard)
            return FALSE;
        const PASN_ObjectId& oid =
            (const PASN_ObjectId&)(const PASN_ObjectId&)gen.m_capabilityIdentifier;
        return (oid == "0.0.8.245.1.1.0" || oid == "0.0.8.245.1.1.11");
    }

    PBoolean OnReceivedPDU(const H245_AudioCapability& pdu)
    {
        return pdu.GetTag() == H245_AudioCapability::e_genericAudioCapability;
    }

    H323Codec* CreateCodec(H323Codec::Direction /*dir*/) const override
    {
        return nullptr;
    }
};