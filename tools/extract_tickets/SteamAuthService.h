#pragma once

#include "Crypto.h"
#include "JsonHelper.h"
#include "TokenStorage.h"
#include "WinHttpTransport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

struct SteamRsaKey {
    std::string modHex;
    std::string expHex;
    uint64_t timestamp{0};
};

struct SteamAuthSession {
    std::string clientId;
    std::string requestId;
    uint64_t steamId{0};
    std::vector<AllowedConfirmation> allowedConfirmations;
};

struct SteamLoginResult {
    bool success{false};
    bool cancelled{false};
    std::string accountName;
    uint64_t steamId{0};
    std::string refreshToken;
    std::string accessToken;
    std::string errorMessage;
};

class SteamAuthService {
public:
    SteamAuthService();
    ~SteamAuthService() = default;

    // Step 1: Fetches Steam's RSA public key for username
    [[nodiscard]] std::optional<SteamRsaKey> GetPasswordRsaKey(std::string_view accountName);

    // Step 2: Initiates authentication session with RSA-encrypted password
    [[nodiscard]] std::optional<SteamAuthSession> BeginAuthSession(
        std::string_view accountName,
        const SecureString& password,
        const SteamRsaKey& rsaKey);

    // Step 3: Submits 2FA code (TOTP or Email code)
    bool SubmitSteamGuardCode(
        const SteamAuthSession& session,
        std::string_view code,
        int codeType = 2);

    // Step 4: Polls auth session status until completed or timed out
    [[nodiscard]] SteamLoginResult PollAuthSession(
        const SteamAuthSession& session,
        std::string_view accountName,
        int maxAttempts = 30,
        int delayMs = 1500);

    // Refreshes an access token using cached refresh_token
    [[nodiscard]] std::optional<std::string> RefreshAccessToken(
        uint64_t steamId,
        std::string_view refreshToken);

    // High-level login with credentials (handles 2FA prompt in TUI/console)
    [[nodiscard]] SteamLoginResult LoginWithCredentials(
        std::string_view accountName,
        const SecureString& password);

    // Fetches owned games list via official WebAPI
    [[nodiscard]] std::vector<OwnedGameInfo> FetchOwnedGames(
        uint64_t steamId,
        std::string_view accessToken);

private:
    WinHttpTransport m_http;
};

} // namespace OST::ExtractTickets
