#include "SteamAuthService.h"
#include "Log.h"
#include "TuiEngine.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

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
        LOG_ERROR("SteamAuth", "认证响应缺少 client_id 或 request_id: {}", resp.body);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

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

    result.errorMessage = "认证轮询超时或用户取消授权";
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

    LOG_DEBUG("SteamAuth", "GenerateAccessTokenForApp 响应 (HTTP {}): {}", resp.statusCode, resp.body);

    auto accessOpt = JsonHelper::GetString(resp.body, "access_token");
    if (accessOpt && !accessOpt->empty()) {
        LOG_DEBUG("SteamAuth", "AccessToken 刷新成功: {}", MaskToken(*accessOpt));
        return accessOpt;
    }

    LOG_WARN("SteamAuth", "响应体中未包含 access_token: {}", resp.body);
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

    if (!resp.IsSuccess()) {
        LOG_WARN("SteamAuth", "拉取游戏列表失败 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
        return {};
    }

    auto games = JsonHelper::ParseOwnedGames(resp.body);
    LOG_DEBUG("SteamAuth", "成功拉取到 {} 款游戏", games.size());
    return games;
}

SteamLoginResult SteamAuthService::LoginWithCredentials(
    std::string_view accountName,
    const SecureString& password) {

    SteamLoginResult failResult;
    failResult.accountName = accountName;

    auto rsaKey = GetPasswordRsaKey(accountName);
    if (!rsaKey) {
        failResult.errorMessage = "无法从 Steam 服务器获取安全 RSA 公钥";
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    auto session = BeginAuthSession(accountName, password, *rsaKey);
    if (!session) {
        failResult.errorMessage = "发起认证会话失败，请检查账号密码是否正确";
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    for (const auto& conf : session->allowedConfirmations) {
        LOG_INFO("SteamAuth", "检测到需要二次验证: type={}, message={}", conf.type, conf.associatedMessage);
        if (conf.type == 3) { // k_EAuthSessionGuardType_DeviceCode (Steam Mobile Authenticator TOTP)
            TuiEngine::ClearScreen();
            auto codeOpt = TuiEngine::PromptInputModal("Steam Guard 手机令牌", "账号已启用手机令牌，请输入手机 App 上的 5 位动态验证码 (TOTP):");
            if (codeOpt && !codeOpt->empty()) {
                SubmitSteamGuardCode(*session, *codeOpt, 3);
            }
            break;
        } else if (conf.type == 2) { // k_EAuthSessionGuardType_EmailCode (Steam Guard Email Code)
            TuiEngine::ClearScreen();
            std::string prompt = conf.associatedMessage.empty()
                ? "验证码已发送至您的注册邮箱，请输入邮件中的验证码:"
                : std::format("验证码已发送至邮箱 ({})，请输入邮件中的验证码:", conf.associatedMessage);
            auto codeOpt = TuiEngine::PromptInputModal("Steam Guard 邮箱验证码", prompt);
            if (codeOpt && !codeOpt->empty()) {
                SubmitSteamGuardCode(*session, *codeOpt, 2);
            }
            break;
        } else if (conf.type == 4) { // k_EAuthSessionGuardType_DeviceConfirmation (Steam App 1-tap)
            TuiEngine::ClearScreen();
            TuiEngine::ShowMessageModal("Steam 手机确认", "请在手机 Steam App 上点击【确认登录】", "确认通过后按 Enter 键继续...");
            break;
        }
    }

    auto result = PollAuthSession(*session, accountName);
    if (!result.success) {
        LOG_WARN("SteamAuth", "{}", result.errorMessage);
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
    SecureString password = ReadPasswordFromConsole("请输入 Steam 登录密码: ");
    if (password.Empty()) {
        SteamLoginResult fail;
        fail.accountName = accountName;
        fail.errorMessage = "密码不能为空";
        return fail;
    }
    return LoginWithCredentials(accountName, password);
}

} // namespace OST::ExtractTickets
