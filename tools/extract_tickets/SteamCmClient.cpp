#include "SteamCmClient.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace OST::ExtractTickets {

namespace {
    std::string BytesToHex(std::span<const uint8_t> bytes) {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            hex += std::format("{:02x}", b);
        }
        return hex;
    }
} // namespace

SteamCmClient::SteamCmClient() = default;

SteamCmClient::~SteamCmClient() {
    Disconnect();
}

void SteamCmClient::Disconnect() {
    if (m_ws.IsConnected()) {
        m_ws.Close();
    }
    m_isLoggedOn = false;
}

bool SteamCmClient::SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId) {
    if (!m_ws.IsConnected()) return false;

    auto packet = PackSteamMsg(eMsg, m_steamId, jobId, body.Data());
    LOG_TRACE("SteamCM", "发送 eMsg: {} ({} 字节)", static_cast<uint32_t>(eMsg), packet.size());
    return m_ws.Send(packet, true);
}

bool SteamCmClient::ReadMatchingMsg(uint32_t expectedEMsg, std::vector<uint8_t>& outBody, DWORD timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
        if (elapsed >= static_cast<long long>(timeoutMs)) {
            LOG_WARN("SteamCM", "等待 eMsg {} 响应超时 ({}ms)", expectedEMsg, timeoutMs);
            return false;
        }

        std::vector<uint8_t> frame;
        bool isBinary = false;
        if (!m_ws.Receive(frame, isBinary, timeoutMs - static_cast<DWORD>(elapsed))) {
            return false;
        }

        uint32_t eMsg = 0;
        std::span<const uint8_t> hdrSpan;
        std::span<const uint8_t> bodySpan;

        if (UnpackSteamMsg(frame, eMsg, hdrSpan, bodySpan)) {
            LOG_TRACE("SteamCM", "收到 eMsg: {} (Body: {} 字节)", eMsg, bodySpan.size());

            // If heartbeat request from server, reply ClientHeartBeat (5503)
            if (eMsg == 5503) {
                ProtoWriter hb;
                (void)SendProtoMsg(ESteamMsg::ClientHeartBeat, hb);
                continue;
            }

            if (eMsg == expectedEMsg) {
                outBody.assign(bodySpan.begin(), bodySpan.end());
                return true;
            }
        }
    }
}

bool SteamCmClient::ConnectAndLogon(uint64_t steamId, std::string_view accessToken) {
    Disconnect();
    m_steamId = steamId;

    LOG_DEBUG("SteamCM", "正在建立 WebSocket 通道连接 Steam CM 服务器 (wss://cm.steampowered.com/cmsocket/)...");
    if (!TuiEngine::IsActive()) {
        std::cout << "[NET] 正在连接 Steam CM 服务器 (wss://cm.steampowered.com/cmsocket/)...\n";
    }

    if (!m_ws.Connect("wss://cm.steampowered.com/cmsocket/", 10000)) {
        LOG_ERROR("SteamCM", "连接 Steam CM 服务器失败");
        if (!TuiEngine::IsActive()) {
            std::cerr << "[ERROR] 无法连接 Steam CM 网关服务器，请检查网络。\n";
        }
        return false;
    }

    LOG_DEBUG("SteamCM", "通信信道已建立，正在执行会话握手登录 (steamId={}, token={})...",
              MaskSteamId(m_steamId), MaskToken(accessToken));
    if (!TuiEngine::IsActive()) {
        std::cout << "[NET] 通信信道已建立，正在执行会话握手登录...\n";
    }

    ProtoWriter logonBody;
    logonBody.WriteUInt32(1, 65580);                      // protocol_version
    logonBody.WriteUInt32(6, 16);                         // client_os_type (Windows 10/11)
    logonBody.WriteString(38, accessToken);               // access_token

    if (!SendProtoMsg(ESteamMsg::ClientLogon, logonBody)) {
        LOG_ERROR("SteamCM", "发送 CMsgClientLogon 失败");
        Disconnect();
        return false;
    }

    std::vector<uint8_t> respBody;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientLogonResponse), respBody, 12000)) {
        LOG_ERROR("SteamCM", "未收到 CMsgClientLogonResponse 响应 (握手超时)");
        Disconnect();
        return false;
    }

    ProtoReader reader(respBody);
    ProtoField field;
    int32_t eresult = 2; // k_EResultFail

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1) { // eresult
            eresult = static_cast<int32_t>(field.varintVal);
        } else if (field.fieldNumber == 9) { // client_supplied_steamid
            m_steamId = field.fixed64Val;
        }
    }

    if (eresult != 1) { // 1 == k_EResultOK
        std::string reason;
        switch (eresult) {
            case 2:  reason = "通用失败 (Fail)"; break;
            case 5:  reason = "访问被拒绝 / 权限不足 (AccessDenied)"; break;
            case 6:  reason = "账号已在其他位置登录 (LoggedInElsewhere)"; break;
            case 8:  reason = "无效或过期的授权令牌 (InvalidToken)"; break;
            case 15: reason = "无权限 (AccessDenied)"; break;
            case 20: reason = "服务暂不可用 (ServiceUnavailable)"; break;
            case 25: reason = "登录频率受限 (LimitExceeded)"; break;
            default: reason = std::format("错误代码 {}", eresult); break;
        }
        LOG_ERROR("SteamCM", "CM 登录被拒绝 (eresult={}, {})", eresult, reason);
        if (!TuiEngine::IsActive()) {
            std::cerr << "[ERROR] CM 登录失败 (EResult=" << eresult << ", " << reason << ")，可能授权已过期。\n";
        }
        Disconnect();
        return false;
    }

    m_isLoggedOn = true;
    LOG_INFO("SteamCM", "CM WebSocket 登录成功！SteamID: {}", MaskSteamId(m_steamId));
    if (!TuiEngine::IsActive()) {
        std::cout << "[OK] Steam CM 登录就绪！(SteamID: " << m_steamId << ")\n";
    }
    return true;
}

std::optional<std::vector<uint8_t>> SteamCmClient::RequestAppOwnershipTicket(uint32_t appId) {
    if (!IsConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", "正在请求 AppOwnershipTicket (AppID={})", appId);

    ProtoWriter body;
    body.WriteUInt32(1, appId); // app_id = 1

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientGetAppOwnershipTicket, body, jobId)) {
        return std::nullopt;
    }

    std::vector<uint8_t> respBody;
    // Expected response eMsg is 858 (CMsgClientGetAppOwnershipTicketResponse)
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientGetAppOwnershipTicketResponse), respBody, 8000)) {
        return std::nullopt;
    }

    ProtoReader reader(respBody);
    ProtoField field;
    int32_t eresult = 2;
    std::vector<uint8_t> ticket;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1) { // eresult
            eresult = static_cast<int32_t>(field.varintVal);
        } else if (field.fieldNumber == 3) { // ticket bytes
            ticket.assign(field.bytesVal.begin(), field.bytesVal.end());
        }
    }

    if (eresult == 1 && !ticket.empty()) {
        LOG_INFO("SteamCM", "成功提取 AppOwnershipTicket (AppID={}, 大小={} 字节, {})",
                 appId, ticket.size(), MaskTicketHex(ticket));
        return ticket;
    } else {
        LOG_WARN("SteamCM", "提取 AppOwnershipTicket 失败 (AppID={}, eresult={})", appId, eresult);
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> SteamCmClient::RequestEncryptedAppTicket(uint32_t appId) {
    if (!IsConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", "正在请求 EncryptedAppTicket (AppID={})", appId);

    ProtoWriter body;
    body.WriteUInt32(1, appId); // app_id = 1

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientRequestEncryptedAppTicket, body, jobId)) {
        return std::nullopt;
    }

    std::vector<uint8_t> respBody;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientRequestEncryptedAppTicketResponse), respBody, 8000)) {
        return std::nullopt;
    }

    ProtoReader reader(respBody);
    ProtoField field;
    int32_t eresult = 2;
    std::vector<uint8_t> ticket;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 2) { // eresult
            eresult = static_cast<int32_t>(field.varintVal);
        } else if (field.fieldNumber == 3) { // encrypted_app_ticket submessage
            ProtoReader subReader(field.bytesVal);
            ProtoField subField;
            while (subReader.ReadNext(subField)) {
                if (subField.fieldNumber == 5) { // encrypted_ticket bytes
                    ticket.assign(subField.bytesVal.begin(), subField.bytesVal.end());
                }
            }
        }
    }

    if (eresult == 1 && !ticket.empty()) {
        LOG_INFO("SteamCM", "成功提取 EncryptedAppTicket (AppID={}, 大小={} 字节, {})",
                 appId, ticket.size(), MaskTicketHex(ticket));
        return ticket;
    } else {
        LOG_DEBUG("SteamCM", "当前账号无该 App 的 EncryptedAppTicket 授权 (eresult={})", eresult);
        return std::nullopt;
    }
}

std::vector<DepotKeyInfo> SteamCmClient::RequestDepotKeys(uint32_t appId, const std::vector<uint32_t>& depotIds) {
    std::vector<DepotKeyInfo> keys;
    if (!IsConnected()) return keys;

    for (uint32_t depotId : depotIds) {
        LOG_DEBUG("SteamCM", "正在请求 Depot 密钥: DepotID={}, AppID={}", depotId, appId);

        ProtoWriter body;
        body.WriteUInt32(1, depotId); // depot_id
        body.WriteUInt32(2, appId);   // app_id

        const uint64_t jobId = ++m_nextJobId;
        if (!SendProtoMsg(ESteamMsg::ClientGetDepotDecryptionKey, body, jobId)) {
            continue;
        }

        std::vector<uint8_t> respBody;
        if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientGetDepotDecryptionKeyResponse), respBody, 6000)) {
            continue;
        }

        ProtoReader reader(respBody);
        ProtoField field;
        int32_t eresult = 2;
        std::vector<uint8_t> keyBytes;

        while (reader.ReadNext(field)) {
            if (field.fieldNumber == 1) { // eresult
                eresult = static_cast<int32_t>(field.varintVal);
            } else if (field.fieldNumber == 3) { // depot_encryption_key
                keyBytes.assign(field.bytesVal.begin(), field.bytesVal.end());
            }
        }

        if (eresult == 1 && !keyBytes.empty()) {
            DepotKeyInfo info;
            info.depotId = depotId;
            info.hexKey = BytesToHex(keyBytes);
            LOG_INFO("SteamCM", "获取到 Depot {} 解密密钥: {}", depotId, MaskKeyHex(info.hexKey));
            keys.push_back(std::move(info));
        } else {
            LOG_DEBUG("SteamCM", "Depot {} 无密钥或未授权 (eresult={})", depotId, eresult);
        }

        // Polite rate pacing
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return keys;
}

std::unordered_map<uint32_t, uint64_t> SteamCmClient::RequestAppTokens(const std::vector<uint32_t>& appIds) {
    std::unordered_map<uint32_t, uint64_t> tokens;
    if (!IsConnected() || appIds.empty()) return tokens;

    ProtoWriter body;
    for (uint32_t appId : appIds) {
        body.WriteUInt32(2, appId); // appids = 2 (repeated)
    }

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientPICSAccessTokenRequest, body, jobId)) {
        return tokens;
    }

    std::vector<uint8_t> respBody;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSAccessTokenResponse), respBody, 8000)) {
        return tokens;
    }

    ProtoReader reader(respBody);
    ProtoField field;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 3) { // app_access_tokens (repeated)
            ProtoReader subReader(field.bytesVal);
            ProtoField subField;
            uint32_t aId = 0;
            uint64_t aToken = 0;
            while (subReader.ReadNext(subField)) {
                if (subField.fieldNumber == 1) aId = static_cast<uint32_t>(subField.varintVal);
                if (subField.fieldNumber == 2) aToken = subField.fixed64Val ? subField.fixed64Val : subField.varintVal;
            }
            if (aId > 0 && aToken > 0) {
                tokens[aId] = aToken;
                LOG_DEBUG("SteamCM", "获得 AppID {} 的 PICS AccessToken: {}", aId, aToken);
            }
        }
    }

    return tokens;
}

ExtractedAppCredentials SteamCmClient::ExtractFullCredentials(uint32_t appId) {
    ExtractedAppCredentials creds;
    creds.appId = appId;

    LOG_DEBUG("SteamCM", "正在向 Steam CM 请求 AppOwnershipTicket (AppID={})...", appId);
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在向 Steam CM 请求 AppOwnershipTicket...\n";
    }
    creds.appOwnershipTicket = RequestAppOwnershipTicket(appId);
    if (creds.appOwnershipTicket) {
        LOG_INFO("SteamCM", "提取到所有权票据 ({} 字节)", creds.appOwnershipTicket->size());
        if (!TuiEngine::IsActive()) {
            std::cout << "     [OK] 提取到所有权票据 (" << creds.appOwnershipTicket->size() << " 字节)\n";
        }
    } else {
        LOG_INFO("SteamCM", "未能获取所有权票据 (账号可能未直接拥有该独立包)");
        if (!TuiEngine::IsActive()) {
            std::cout << "     [INFO] 未能获取所有权票据 (账号可能未直接拥有该独立包)\n";
        }
    }

    LOG_DEBUG("SteamCM", "正在向 Steam CM 请求 EncryptedAppTicket (AppID={})...", appId);
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在向 Steam CM 请求 EncryptedAppTicket...\n";
    }
    creds.encryptedAppTicket = RequestEncryptedAppTicket(appId);
    if (creds.encryptedAppTicket) {
        LOG_INFO("SteamCM", "提取到加密票据 ({} 字节)", creds.encryptedAppTicket->size());
        if (!TuiEngine::IsActive()) {
            std::cout << "     [OK] 提取到加密票据 (" << creds.encryptedAppTicket->size() << " 字节)\n";
        }
    } else {
        LOG_INFO("SteamCM", "未能获取加密票据 (无加密运行时授权需求或未拥有)");
        if (!TuiEngine::IsActive()) {
            std::cout << "     [INFO] 未能获取加密票据 (无加密运行时授权需求或未拥有)\n";
        }
    }

    LOG_DEBUG("SteamCM", "正在向 Steam CM 查询 Depot 解密密钥 (AppID={})...", appId);
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在向 Steam CM 查询 Depot 解密密钥...\n";
    }
    std::vector<uint32_t> initialDepots = { appId };
    creds.depotKeys = RequestDepotKeys(appId, initialDepots);
    if (!creds.depotKeys.empty()) {
        LOG_INFO("SteamCM", "提取到 {} 个 Depot 解密密钥", creds.depotKeys.size());
        if (!TuiEngine::IsActive()) {
            std::cout << "     [OK] 提取到 " << creds.depotKeys.size() << " 个 Depot 解密密钥\n";
        }
    }

    LOG_DEBUG("SteamCM", "正在查询 64 位 PICS AccessToken (AppID={})...", appId);
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在查询 64 位 PICS AccessToken...\n";
    }
    creds.appTokens = RequestAppTokens({ appId });
    if (!creds.appTokens.empty()) {
        LOG_INFO("SteamCM", "提取到 PICS 访问令牌 (共 {} 个)", creds.appTokens.size());
        if (!TuiEngine::IsActive()) {
            std::cout << "     [OK] 提取到 PICS 访问令牌\n";
        }
    }

    return creds;
}

} // namespace OST::ExtractTickets
