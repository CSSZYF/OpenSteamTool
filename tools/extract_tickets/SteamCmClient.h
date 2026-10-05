#pragma once

#include "SteamSession.h"
#include "SteamWire.h"
#include "VdfParser.h"
#include "WinHttpTransport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OST::ExtractTickets {

struct ExtractedAppCredentials {
    uint32_t appId{0};
    std::optional<std::vector<uint8_t>> appOwnershipTicket;
    std::optional<std::vector<uint8_t>> encryptedAppTicket;
    std::vector<DepotKeyInfo> depotKeys;
    std::vector<DlcInfo> dlcs;
    std::unordered_map<uint32_t, uint64_t> appTokens;
};

class SteamCmClient {
public:
    SteamCmClient();
    ~SteamCmClient();

    // Connects to Steam CM WebSocket and executes CMsgClientLogon with accessToken
    [[nodiscard]] bool ConnectAndLogon(uint64_t steamId, std::string_view accessToken);

    // Disconnects and shuts down WebSocket session
    void Disconnect();

    // Requests server-signed AppOwnershipTicket (eMsg 5560)
    [[nodiscard]] std::optional<std::vector<uint8_t>> RequestAppOwnershipTicket(uint32_t appId);

    // Requests EncryptedAppTicket (eMsg 5527)
    [[nodiscard]] std::optional<std::vector<uint8_t>> RequestEncryptedAppTicket(uint32_t appId);

    // Requests 32-byte AES depot decryption keys for depot IDs (eMsg 5438)
    [[nodiscard]] std::vector<DepotKeyInfo> RequestDepotKeys(uint32_t appId, const std::vector<uint32_t>& depotIds);

    // Requests 64-bit PICS access tokens for app IDs (eMsg 8901)
    [[nodiscard]] std::unordered_map<uint32_t, uint64_t> RequestAppTokens(const std::vector<uint32_t>& appIds);

    // High-level extraction pipeline for a single target AppID
    [[nodiscard]] ExtractedAppCredentials ExtractFullCredentials(uint32_t appId);

    [[nodiscard]] bool IsConnected() const noexcept { return m_ws.IsConnected() && m_isLoggedOn; }

private:
    [[nodiscard]] bool SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId = 0);
    [[nodiscard]] bool ReadMatchingMsg(uint32_t expectedEMsg, std::vector<uint8_t>& outBody, DWORD timeoutMs = 8000);

    WebSocketClient m_ws;
    uint64_t m_steamId{0};
    uint64_t m_nextJobId{100};
    bool m_isLoggedOn{false};
};

} // namespace OST::ExtractTickets
