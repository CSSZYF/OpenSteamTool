#include "SteamAuthService.h"
#include "AppInfoParser.h"
#include "I18n.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#if defined(_WIN32)
#include <conio.h>
#endif

namespace OST::ExtractTickets {

namespace {
    std::string UrlEncode(std::string_view str) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : str) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << '%' << std::setw(2) << std::uppercase
                        << static_cast<int>(static_cast<unsigned char>(c));
            }
        }
        return escaped.str();
    }
} // namespace

SteamAuthService::SteamAuthService() = default;

std::optional<SteamRsaKey> SteamAuthService::GetPasswordRsaKey(std::string_view accountName) {
    std::string url = "https://api.steampowered.com/IAuthenticationService/GetPasswordRSAPublicKey/v1?account_name=" + UrlEncode(accountName);
    LOG_DEBUG("SteamAuth", "正在请求 RSA 公钥: {}", MaskAccount(accountName));

    HttpResponse resp = m_http.Get(url);
    if (!resp.IsSuccess()) {
        LOG_ERROR("SteamAuth", "获取 RSA 公钥失败 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    auto modOpt = JsonHelper::GetString(resp.body, "publickey_mod");
    auto expOpt = JsonHelper::GetString(resp.body, "publickey_exp");
    auto timeOpt = JsonHelper::GetUInt64(resp.body, "timestamp");

    if (!modOpt || !expOpt || !timeOpt) {
        LOG_ERROR("SteamAuth", "RSA 公钥响应解析失败");
        return std::nullopt;
    }

    SteamRsaKey rsaKey;
    rsaKey.modHex = *modOpt;
    rsaKey.expHex = *expOpt;
    rsaKey.timestamp = *timeOpt;
    LOG_DEBUG("SteamAuth", "成功获取 RSA 公钥 (timestamp={})", rsaKey.timestamp);
    return rsaKey;
}

std::optional<SteamAuthSession> SteamAuthService::BeginAuthSession(
    std::string_view accountName,
    const SecureString& password,
    const SteamRsaKey& rsaKey) {

    std::string encryptedPassword = EncryptPasswordWithRSA(password, rsaKey.modHex, rsaKey.expHex);
    if (encryptedPassword.empty()) {
        LOG_ERROR("SteamAuth", "密码 RSA 加密失败");
        return std::nullopt;
    }

    std::string postData = "account_name=" + UrlEncode(accountName) +
                           "&encrypted_password=" + UrlEncode(encryptedPassword) +
                           "&encryption_timestamp=" + std::to_string(rsaKey.timestamp) +
                           "&set_remember_login=true&platform_type=1&persistence=1&website_id=Client";

    std::string url = "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaCredentials/v1";
    LOG_DEBUG("SteamAuth", "发起身份认证会话: {}", MaskAccount(accountName));

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    if (!resp.IsSuccess()) {
        LOG_ERROR("SteamAuth", "发起到 Steam 认证会话失败 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    auto clientIdOpt = JsonHelper::GetString(resp.body, "client_id");
    auto requestIdOpt = JsonHelper::GetString(resp.body, "request_id");
    auto steamIdOpt = JsonHelper::GetUInt64(resp.body, "steamid");

    if (!clientIdOpt || !requestIdOpt) {
        LOG_ERROR("SteamAuth", "认证响应缺少 client_id 或 request_id (HTTP {})", resp.statusCode);
        return std::nullopt;
    }

    SteamAuthSession session;
    session.clientId = *clientIdOpt;
    session.requestId = *requestIdOpt;
    session.steamId = steamIdOpt.value_or(0);
    session.allowedConfirmations = JsonHelper::GetConfirmations(resp.body);

    LOG_DEBUG("SteamAuth", "会话已创建 (steamId={}, confirmations_count={})",
              MaskSteamId(session.steamId), session.allowedConfirmations.size());
    return session;
}

bool SteamAuthService::SubmitSteamGuardCode(
    const SteamAuthSession& session,
    std::string_view code,
    int codeType) {

    std::string postData = "client_id=" + UrlEncode(session.clientId) +
                           "&steamid=" + std::to_string(session.steamId) +
                           "&code=" + UrlEncode(code) +
                           "&code_type=" + std::to_string(codeType);

    std::string url = "https://api.steampowered.com/IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1";
    LOG_DEBUG("SteamAuth", "正在提交 2FA 动态码 (codeType={})", codeType);

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    if (!resp.IsSuccess()) {
        LOG_WARN("SteamAuth", "提交 2FA 动态码响应非成功 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
        return false;
    }

    LOG_DEBUG("SteamAuth", "已成功提交动态码至 Valve 服务器");
    return true;
}

SteamLoginResult SteamAuthService::PollAuthSession(
    const SteamAuthSession& session,
    std::string_view accountName,
    int maxAttempts,
    int delayMs) {

    SteamLoginResult result;
    result.accountName = accountName;
    result.steamId = session.steamId;

    std::string postData = "client_id=" + UrlEncode(session.clientId) +
                           "&request_id=" + UrlEncode(session.requestId);

    std::string url = "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1";

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        constexpr int sliceMs = 50;
        const int slices = delayMs / sliceMs;
        for (int s = 0; s < slices; ++s) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));
#if defined(_WIN32)
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 27) { // ESC key
                    TuiEngine::FlushInputBuffer();
                    result.cancelled = true;
                    result.errorMessage = std::string(TR(MsgKey::ErrUserCancelledPoll));
                    LOG_INFO("SteamAuth", "用户按下 ESC 取消认证轮询");
                    return result;
                }
            }
#endif
        }

        HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
        if (!resp.IsSuccess()) {
            continue;
        }

        auto refreshOpt = JsonHelper::GetString(resp.body, "refresh_token");
        auto accessOpt = JsonHelper::GetString(resp.body, "access_token");

        if (refreshOpt && !refreshOpt->empty() && accessOpt && !accessOpt->empty()) {
            result.success = true;
            result.refreshToken = *refreshOpt;
            result.accessToken = *accessOpt;
            LOG_DEBUG("SteamAuth", "授权状态轮询成功！获取到 Token (refresh={}, access={})",
                      MaskToken(result.refreshToken), MaskToken(result.accessToken));
            return result;
        }
    }

    result.errorMessage = std::string(TR(MsgKey::ErrAuthTimeoutOrCancelled));
    LOG_WARN("SteamAuth", "{}", result.errorMessage);
    return result;
}

std::optional<std::string> SteamAuthService::RefreshAccessToken(
    uint64_t steamId,
    std::string_view refreshToken) {

    std::string postData = "refresh_token=" + UrlEncode(refreshToken) +
                           "&steamid=" + std::to_string(steamId);

    std::string url = "https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1";
    LOG_DEBUG("SteamAuth", "使用本地缓存 refresh_token 刷新 access_token (SteamID={})", MaskSteamId(steamId));

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    if (!resp.IsSuccess()) {
        LOG_WARN("SteamAuth", "刷新 AccessToken 失败 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    LOG_DEBUG("SteamAuth", "GenerateAccessTokenForApp 响应成功 (HTTP {})", resp.statusCode);

    auto accessOpt = JsonHelper::GetString(resp.body, "access_token");
    if (accessOpt && !accessOpt->empty()) {
        LOG_DEBUG("SteamAuth", "AccessToken 刷新成功: {}", MaskToken(*accessOpt));
        return accessOpt;
    }

    LOG_WARN("SteamAuth", "响应体中未包含 access_token (HTTP {})", resp.statusCode);
    return std::nullopt;
}

std::vector<OwnedGameInfo> SteamAuthService::FetchOwnedGames(
    uint64_t steamId,
    std::string_view accessToken) {

    std::string url = "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?access_token=" +
                      UrlEncode(accessToken) +
                      "&steamid=" + std::to_string(steamId) +
                      "&include_appinfo=1&include_played_free_games=0";

    LOG_DEBUG("SteamAuth", "正在通过官方 WebAPI 拉取拥有的游戏列表...");
    HttpResponse resp = m_http.Get(url);

    std::vector<OwnedGameInfo> games;
    if (resp.IsSuccess()) {
        games = JsonHelper::ParseOwnedGames(resp.body);
        LOG_DEBUG("SteamAuth", "成功拉取到 {} 款个人游戏", games.size());
    } else {
        LOG_WARN("SteamAuth", "拉取个人游戏列表失败 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
    }

    // 查询当前账号的 Steam 家庭组 (IFamilyGroupsService)
    std::string familyUrl = "https://api.steampowered.com/IFamilyGroupsService/GetFamilyGroupForUser/v1/?access_token=" +
                            UrlEncode(accessToken) +
                            "&include_family_group_response=true";
    HttpResponse famResp = m_http.Get(familyUrl);
    if (famResp.IsSuccess()) {
        auto familyGroupIdOpt = JsonHelper::GetString(famResp.body, "family_groupid");
        if (!familyGroupIdOpt || familyGroupIdOpt->empty() || *familyGroupIdOpt == "0") {
            auto famGroupNum = JsonHelper::GetUInt64(famResp.body, "family_groupid");
            if (famGroupNum && *famGroupNum > 0) {
                familyGroupIdOpt = std::to_string(*famGroupNum);
            }
        }

        if (familyGroupIdOpt && !familyGroupIdOpt->empty() && *familyGroupIdOpt != "0") {
            LOG_INFO("SteamAuth", "检测到当前账号加入的 Steam 家庭群组 (GroupID={})", MaskGroupId(*familyGroupIdOpt));
            std::string sharedUrl = "https://api.steampowered.com/IFamilyGroupsService/GetSharedLibraryApps/v1/?access_token=" +
                                    UrlEncode(accessToken) +
                                    "&family_groupid=" + *familyGroupIdOpt +
                                    "&include_own=false&include_non_games=false";
            HttpResponse sharedResp = m_http.Get(sharedUrl);
            if (sharedResp.IsSuccess()) {
                auto sharedGames = JsonHelper::ParseSharedLibraryApps(sharedResp.body);
                LOG_INFO("SteamAuth", "从 Steam 家庭共享库中发现 {} 款游戏", sharedGames.size());

                std::unordered_set<uint32_t> existing;
                for (const auto& g : games) {
                    existing.insert(g.appId);
                }

                // Batch resolve app names from local appinfo.vdf if shared games lack names
                std::unordered_set<uint32_t> missingNameAppIds;
                for (const auto& sg : sharedGames) {
                    if (!existing.contains(sg.appId) && sg.name.empty()) {
                        missingNameAppIds.insert(sg.appId);
                    }
                }

                std::unordered_map<uint32_t, std::string> resolvedNames;
                if (!missingNameAppIds.empty()) {
                    auto steamPathOpt = FindSteamInstallPath();
                    if (steamPathOpt && !steamPathOpt->empty()) {
                        resolvedNames = ParseAppNames(*steamPathOpt, missingNameAppIds);
                    }
                    for (uint32_t mId : missingNameAppIds) {
                        if (!resolvedNames.contains(mId) || resolvedNames[mId].empty()) {
                            std::string storeUrl = std::format("https://store.steampowered.com/api/appdetails?appids={}&filters=basic", mId);
                            HttpResponse sResp = m_http.Get(storeUrl);
                            if (sResp.IsSuccess()) {
                                auto nameOpt = JsonHelper::GetString(sResp.body, "name");
                                if (nameOpt && !nameOpt->empty()) {
                                    resolvedNames[mId] = std::move(*nameOpt);
                                }
                            }
                        }
                    }
                }

                for (auto& sg : sharedGames) {
                    if (!existing.contains(sg.appId)) {
                        existing.insert(sg.appId);
                        if (sg.name.empty()) {
                            auto it = resolvedNames.find(sg.appId);
                            if (it != resolvedNames.end() && !it->second.empty()) {
                                sg.name = it->second;
                            } else {
                                sg.name = "App " + std::to_string(sg.appId);
                            }
                        }
                        sg.isShared = true;
                        games.push_back(std::move(sg));
                    }
                }
            } else {
                LOG_WARN("SteamAuth", "查询家庭共享游戏列表未成功 (HTTP {}): {}", sharedResp.statusCode, sharedResp.errorMessage);
            }
        }
    }

    return games;
}

SteamLoginResult SteamAuthService::LoginWithCredentials(
    std::string_view accountName,
    const SecureString& password) {

    SteamLoginResult failResult;
    failResult.accountName = accountName;

    auto rsaKey = GetPasswordRsaKey(accountName);
    if (!rsaKey) {
        failResult.errorMessage = std::string(TR(MsgKey::ErrRsaKeyFailed));
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    auto session = BeginAuthSession(accountName, password, *rsaKey);
    if (!session) {
        failResult.errorMessage = std::string(TR(MsgKey::ErrAuthSessionFailed));
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    for (const auto& conf : session->allowedConfirmations) {
        LOG_INFO("SteamAuth", "检测到需要二次验证: type={}, message={}", conf.type,
                 conf.type == 2 ? MaskEmail(conf.associatedMessage) : conf.associatedMessage);
        if (conf.type == 3) { // k_EAuthSessionGuardType_DeviceCode (Steam Mobile Authenticator TOTP)
            TuiEngine::ClearScreen();
            auto codeOpt = TuiEngine::PromptInputModal(TR(MsgKey::GuardMobileTitle), TR(MsgKey::GuardMobilePrompt));
            if (!codeOpt) {
                failResult.cancelled = true;
                failResult.errorMessage = std::string(TR(MsgKey::ErrUserCancelledInput));
                LOG_INFO("SteamAuth", "用户在 2FA 手机令牌界面按 ESC 取消登录");
                return failResult;
            }
            if (!codeOpt->empty()) {
                SubmitSteamGuardCode(*session, *codeOpt, 3);
            }
            break;
        } else if (conf.type == 2) { // k_EAuthSessionGuardType_EmailCode (Steam Guard Email Code)
            TuiEngine::ClearScreen();
            std::string prompt = conf.associatedMessage.empty()
                ? std::string(TR(MsgKey::GuardEmailTitle))
                : TR_FMT(MsgKey::GuardEmailPrompt, MaskEmail(conf.associatedMessage));
            auto codeOpt = TuiEngine::PromptInputModal(TR(MsgKey::GuardEmailTitle), prompt);
            if (!codeOpt) {
                failResult.cancelled = true;
                failResult.errorMessage = std::string(TR(MsgKey::ErrUserCancelledInput));
                LOG_INFO("SteamAuth", "用户在邮箱验证码界面按 ESC 取消登录");
                return failResult;
            }
            if (!codeOpt->empty()) {
                SubmitSteamGuardCode(*session, *codeOpt, 2);
            }
            break;
        } else if (conf.type == 4) { // k_EAuthSessionGuardType_DeviceConfirmation (Steam App 1-tap)
            TuiEngine::ClearScreen();
            bool proceed = TuiEngine::ShowMessageModal(TR(MsgKey::GuardDeviceTitle), TR(MsgKey::GuardDevicePrompt), TR(MsgKey::GuardDeviceDetail));
            if (!proceed) {
                failResult.cancelled = true;
                failResult.errorMessage = std::string(TR(MsgKey::ErrUserCancelledDevice));
                LOG_INFO("SteamAuth", "用户在手机确认提示界面按 ESC 取消登录");
                return failResult;
            }
            break;
        }
    }

    auto result = PollAuthSession(*session, accountName);
    if (!result.success) {
        if (!result.cancelled) {
            LOG_WARN("SteamAuth", "{}", result.errorMessage);
        }
        return result;
    }

    CachedAccount cached;
    cached.accountName = std::string{accountName};
    cached.steamId = result.steamId;
    cached.refreshToken = result.refreshToken;
    cached.accessToken = result.accessToken;
    cached.lastLoginTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    TokenStorage::UpsertAccount(cached);
    return result;
}

SteamLoginResult SteamAuthService::InteractiveLogin(std::string_view accountName) {
    SecureString password = ReadPasswordFromConsole("Password: ");
    if (password.Empty()) {
        SteamLoginResult fail;
        fail.accountName = accountName;
        fail.cancelled = true;
        fail.errorMessage = std::string(TR(MsgKey::ErrUserCancelledPwd));
        return fail;
    }
    return LoginWithCredentials(accountName, password);
}

} // namespace OST::ExtractTickets
