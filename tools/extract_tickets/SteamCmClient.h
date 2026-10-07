#pragma once

#include "AppInfoParser.h"
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

    // Connects to Steam CM WebSocket and executes CMsgClientLogon with refreshToken
    [[nodiscard]] bool ConnectAndLogon(uint64_t steamId, std::string_view refreshToken, std::string_view accessToken = "");

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

    // Requests PICS AppInfo product metadata (eMsg 8903)
    [[nodiscard]] std::optional<ParsedAppInfoData> RequestPicsProductInfo(uint32_t appId, uint64_t accessToken = 0);

    // Fast batch resolution of official app/DLC names via Steam CM PICS (eMsg 8903)
    [[nodiscard]] std::unordered_map<uint32_t, std::string> RequestPicsAppNames(const std::vector<uint32_t>& appIds);

    // Sets or clears playing state via CMsgClientGamesPlayed (eMsg 742) for session license activation
    bool SetGamePlayed(uint32_t appId);

    // Resolves manifest request code via CM ServiceMethod or legacy RPC
    [[nodiscard]] std::string FetchManifestRequestCode(
        uint32_t appId, uint32_t depotId, const std::string& manifestId, bool* outAccessDenied = nullptr);

    // Dynamically queries active Steam CDN servers from GetServersForSteamPipe
    [[nodiscard]] static std::vector<std::string> GetCdnServers(uint32_t cellId = 0);

    // Downloads manifest binary directly from Steam CDN via GetManifestRequestCode
    [[nodiscard]] std::optional<std::string> DownloadManifestOnline(
        uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& destDir,
        bool* outAccessDenied = nullptr, WinHttpTransport* sharedHttp = nullptr);

    // Downloads and decompresses manifest binary directly from Steam CDN via an already resolved reqCode
    [[nodiscard]] static std::optional<std::string> DownloadManifestPayload(
        uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& reqCode,
        const std::string& destDir, const std::vector<std::string>& cdnServers, WinHttpTransport* http = nullptr);

    // Promotes a validated fast CDN server to the head of the cached server list
    static void PromoteWorkingCdnServer(std::string_view server);

    // Queries official Steam Store API for drm_notice and caches the result
    [[nodiscard]] static bool DetectDenuvoFromStore(uint32_t appId);

    // High-level extraction pipeline for a single target AppID
    [[nodiscard]] ExtractedAppCredentials ExtractFullCredentials(uint32_t appId, bool forceEticket = false);

    void SetAccessToken(std::string_view token) { m_accessToken = token; }
    [[nodiscard]] bool IsConnected() const noexcept { return m_ws.IsConnected() && m_isLoggedOn; }
    [[nodiscard]] bool EnsureConnected();

private:
    [[nodiscard]] bool SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId = 0, std::string_view targetJobName = {});
    [[nodiscard]] bool ReadMatchingMsg(
        uint32_t expectedEMsg,
        std::vector<uint8_t>& outBody,
        DWORD timeoutMs = 8000,
        int32_t* outEResult = nullptr,
        uint64_t expectedJobId = 0);
    void UnpackMultiMsg(std::span<const uint8_t> bodySpan);

    struct QueuedMsg {
        uint32_t eMsg{0};
        uint64_t targetJobId{0};
        int32_t eresult{1};
        std::vector<uint8_t> body;
    };

    WebSocketClient m_ws;
    std::vector<QueuedMsg> m_msgQueue;
    std::string m_refreshToken;
    std::string m_accessToken;
    uint64_t m_steamId{0};
    uint64_t m_nextJobId{100};
    int32_t m_clientSessionId{0};
    uint32_t m_cellId{0};
    bool m_isLoggedOn{false};
};

} // namespace OST::ExtractTickets
