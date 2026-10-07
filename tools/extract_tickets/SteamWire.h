#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

// Steam EMsg constants
enum class ESteamMsg : uint32_t {
    Invalid = 0,
    ServiceMethodResponse = 147,
    ServiceMethodCallFromClient = 151,
    ServiceMethodSendToClient = 152,
    ClientHeartBeat = 703,
    ClientGamesPlayed = 742,
    ClientLogonResponse = 751,
    ClientGetAppOwnershipTicket = 857,
    ClientGetAppOwnershipTicketResponse = 858,
    ClientGetDepotDecryptionKey = 5438,
    ClientGetDepotDecryptionKeyResponse = 5439,
    ClientLogon = 5514,
    ClientRequestEncryptedAppTicket = 5526,
    ClientRequestEncryptedAppTicketResponse = 5527,
    ClientServiceMethod = 5594,
    ClientServiceMethodResponse = 5595,
    ClientPICSProductInfoRequest = 8903,
    ClientPICSProductInfoResponse = 8904,
    ClientPICSAccessTokenRequest = 8905,
    ClientPICSAccessTokenResponse = 8906,
};

constexpr uint32_t kSteamProtoMask = 0x80000000;

// ============================================================================
// Lightweight Zero-Dependency Protobuf Wire Encoder / Decoder
// ============================================================================

class ProtoWriter {
public:
    ProtoWriter() {
        m_buf.reserve(128);
    }

    void WriteVarint(uint64_t val) {
        while (val >= 0x80) {
            m_buf.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
            val >>= 7;
        }
        m_buf.push_back(static_cast<uint8_t>(val));
    }

    void WriteTag(uint32_t fieldNumber, uint8_t wireType) {
        WriteVarint((static_cast<uint64_t>(fieldNumber) << 3) | (wireType & 0x7));
    }

    void WriteUInt32(uint32_t fieldNumber, uint32_t val) {
        WriteTag(fieldNumber, 0);
        WriteVarint(val);
    }

    void WriteBool(uint32_t fieldNumber, bool val) {
        WriteTag(fieldNumber, 0);
        WriteVarint(val ? 1 : 0);
    }

    void WriteUInt64(uint32_t fieldNumber, uint64_t val) {
        WriteTag(fieldNumber, 0);
        WriteVarint(val);
    }

    void WriteInt32(uint32_t fieldNumber, int32_t val) {
        WriteTag(fieldNumber, 0);
        WriteVarint(static_cast<uint64_t>(val));
    }

    void WriteFixed64(uint32_t fieldNumber, uint64_t val) {
        WriteTag(fieldNumber, 1);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        m_buf.insert(m_buf.end(), p, p + 8);
    }

    void WriteString(uint32_t fieldNumber, std::string_view str) {
        WriteTag(fieldNumber, 2);
        WriteVarint(str.size());
        m_buf.insert(m_buf.end(), str.begin(), str.end());
    }

    void WriteBytes(uint32_t fieldNumber, std::span<const uint8_t> data) {
        WriteTag(fieldNumber, 2);
        WriteVarint(data.size());
        m_buf.insert(m_buf.end(), data.begin(), data.end());
    }

    void WriteSubMessage(uint32_t fieldNumber, const ProtoWriter& sub) {
        WriteTag(fieldNumber, 2);
        WriteVarint(sub.Size());
        m_buf.insert(m_buf.end(), sub.Data().begin(), sub.Data().end());
    }

    [[nodiscard]] const std::vector<uint8_t>& Data() const noexcept { return m_buf; }
    [[nodiscard]] size_t Size() const noexcept { return m_buf.size(); }

private:
    std::vector<uint8_t> m_buf;
};

struct ProtoField {
    uint32_t fieldNumber{0};
    uint8_t wireType{0};
    uint64_t varintVal{0};
    uint64_t fixed64Val{0};
    uint32_t fixed32Val{0};
    std::span<const uint8_t> bytesVal;
};

class ProtoReader {
public:
    explicit ProtoReader(std::span<const uint8_t> data)
        : m_ptr(data.data()), m_end(data.data() + data.size()) {}

    bool ReadNext(ProtoField& outField) {
        outField = {};
        if (m_ptr >= m_end) return false;

        uint64_t tag = 0;
        if (!ReadVarint(tag)) return false;

        outField.fieldNumber = static_cast<uint32_t>(tag >> 3);
        outField.wireType = static_cast<uint8_t>(tag & 0x7);
        if (outField.fieldNumber == 0) return false;

        switch (outField.wireType) {
            case 0: // Varint
                return ReadVarint(outField.varintVal);
            case 1: // 64-bit
                if (m_end - m_ptr < 8) return false;
                std::memcpy(&outField.fixed64Val, m_ptr, 8);
                m_ptr += 8;
                return true;
            case 2: { // Length-delimited
                uint64_t len = 0;
                if (!ReadVarint(len)) return false;
                if (len > static_cast<size_t>(m_end - m_ptr)) return false;
                outField.bytesVal = std::span<const uint8_t>(m_ptr, static_cast<size_t>(len));
                m_ptr += len;
                return true;
            }
            case 5: // 32-bit
                if (m_end - m_ptr < 4) return false;
                std::memcpy(&outField.fixed32Val, m_ptr, 4);
                m_ptr += 4;
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] bool HasMore() const noexcept { return m_ptr < m_end; }

private:
    bool ReadVarint(uint64_t& outVal) {
        outVal = 0;
        int shift = 0;
        while (m_ptr < m_end && shift < 64) {
            uint8_t b = *m_ptr++;
            outVal |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }

    const uint8_t* m_ptr{nullptr};
    const uint8_t* m_end{nullptr};
};

// Builds a complete Steam Protobuf frame:
// [uint32 eMsg | 0x80000000][uint32 headerLen][CMsgProtoBufHeader][Body]
inline std::vector<uint8_t> PackSteamMsg(
    ESteamMsg eMsg,
    uint64_t steamId,
    uint64_t jobIdSource,
    std::span<const uint8_t> bodyBytes,
    int32_t clientSessionId = 0,
    std::string_view targetJobName = {}) {

    // 1. Build CMsgProtoBufHeader
    ProtoWriter hdrWriter;
    if (steamId != 0) {
        hdrWriter.WriteFixed64(1, steamId);
    }
    if (clientSessionId != 0) {
        hdrWriter.WriteInt32(2, clientSessionId);
    }
    if (jobIdSource != 0 && jobIdSource != UINT64_MAX) {
        hdrWriter.WriteFixed64(10, jobIdSource);
    }
    if (!targetJobName.empty()) {
        hdrWriter.WriteString(12, targetJobName);
    }

    const auto& hdrBytes = hdrWriter.Data();
    const uint32_t rawMsg = static_cast<uint32_t>(eMsg) | kSteamProtoMask;
    const uint32_t hdrLen = static_cast<uint32_t>(hdrBytes.size());

    std::vector<uint8_t> packet(8 + hdrLen + bodyBytes.size());
    std::memcpy(packet.data(), &rawMsg, 4);
    std::memcpy(packet.data() + 4, &hdrLen, 4);
    if (hdrLen > 0) {
        std::memcpy(packet.data() + 8, hdrBytes.data(), hdrLen);
    }
    if (!bodyBytes.empty()) {
        std::memcpy(packet.data() + 8 + hdrLen, bodyBytes.data(), bodyBytes.size());
    }

    return packet;
}

// Unpacks a Steam frame into eMsg, header span, body span
inline bool UnpackSteamMsg(
    std::span<const uint8_t> packet,
    uint32_t& outEMsg,
    std::span<const uint8_t>& outHdr,
    std::span<const uint8_t>& outBody) {

    if (packet.size() < 8) return false;

    uint32_t rawMsg = 0;
    std::memcpy(&rawMsg, packet.data(), 4);
    uint32_t hdrLen = 0;
    std::memcpy(&hdrLen, packet.data() + 4, 4);

    outEMsg = rawMsg & ~kSteamProtoMask;
    if (hdrLen > packet.size() - 8) return false;

    outHdr = packet.subspan(8, hdrLen);
    outBody = packet.subspan(8 + hdrLen);
    return true;
}

// Extracts EResult (field 13) from CMsgProtoBufHeader
inline int32_t ExtractHeaderEResult(std::span<const uint8_t> hdrBytes) {
    if (hdrBytes.empty()) return 1;
    ProtoReader r(hdrBytes);
    ProtoField f;
    while (r.ReadNext(f)) {
        if (f.fieldNumber == 13) { // eresult
            return static_cast<int32_t>(f.varintVal);
        }
    }
    return 1;
}

// Extracts job_id_target (field 11) from CMsgProtoBufHeader
inline uint64_t ExtractHeaderTargetJobId(std::span<const uint8_t> hdrBytes) {
    if (hdrBytes.empty()) return 0;
    ProtoReader r(hdrBytes);
    ProtoField f;
    while (r.ReadNext(f)) {
        if (f.fieldNumber == 11) { // job_id_target
            uint64_t id = (f.wireType == 1) ? f.fixed64Val : f.varintVal;
            return (id == UINT64_MAX) ? 0 : id;
        }
    }
    return 0;
}

} // namespace OST::ExtractTickets
