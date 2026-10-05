#include "SteamCmClient.h"
#include "AppInfoParser.h"
#include "JsonHelper.h"
#include "Log.h"
#include "LuaFallbackParser.h"
#include "TuiEngine.h"
#include "Utils.h"
#include "tinf.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
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

    std::vector<std::string> QueryCmWebSockets() {
        std::vector<std::string> endpoints;
        WinHttpTransport http;
        LOG_DEBUG("SteamCM", "正在从 Steam 官方 API 查询可用 CM 服务器列表 (GetCMList)...");
        HttpResponse resp = http.Get("https://api.steampowered.com/ISteamDirectory/GetCMList/v1/?cellid=0");
        if (resp.IsSuccess()) {
            auto list = JsonHelper::GetStringArray(resp.body, "serverlist_websockets");
            for (const auto& item : list) {
                // Prefer 443 endpoints for standard TLS WebSocket
                if (item.find(":443") != std::string::npos) {
                    endpoints.push_back(item);
                }
            }
            if (endpoints.empty()) {
                endpoints = std::move(list);
            }
            LOG_DEBUG("SteamCM", "从 Steam Directory 获取到 {} 个 WebSocket CM 节点", endpoints.size());
        } else {
            LOG_WARN("SteamCM", "查询 GetCMList 失败 (HTTP {}): {}", resp.statusCode, resp.errorMessage);
        }

        if (endpoints.empty()) {
            endpoints = {
                "cmp1-ord1.steamserver.net:443",
                "cmp2-ord1.steamserver.net:443",
                "cmp1-iad1.steamserver.net:443",
                "cmp2-iad1.steamserver.net:443",
                "cmp1-sea1.steamserver.net:443",
                "cmp2-sea1.steamserver.net:443",
                "cmp1-lax1.steamserver.net:443",
                "cmp1-fra1.steamserver.net:443"
            };
            LOG_INFO("SteamCM", "使用预置的高可用 CM 服务器集群 (共 {} 个)", endpoints.size());
        }
        return endpoints;
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
    m_msgQueue.clear();
    m_isLoggedOn = false;
}

bool SteamCmClient::SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId) {
    if (!m_ws.IsConnected()) return false;

    auto packet = PackSteamMsg(eMsg, m_steamId, jobId, body.Data());
    LOG_TRACE("SteamCM", "发送 eMsg: {} ({} 字节)", static_cast<uint32_t>(eMsg), packet.size());
    return m_ws.Send(packet, true);
}

void SteamCmClient::UnpackMultiMsg(std::span<const uint8_t> bodySpan) {
    ProtoReader reader(bodySpan);
    ProtoField field;
    uint32_t sizeUnzipped = 0;
    std::span<const uint8_t> messageBody;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1) { // size_unzipped
            sizeUnzipped = static_cast<uint32_t>(field.varintVal);
        } else if (field.fieldNumber == 2) { // message_body
            messageBody = field.bytesVal;
        }
    }

    if (messageBody.empty()) return;

    std::vector<uint8_t> payload;
    if (sizeUnzipped > 0) {
        payload.resize(sizeUnzipped);
        unsigned int destLen = sizeUnzipped;
        int res = TINF_DATA_ERROR;

        if (messageBody.size() >= 2 && messageBody[0] == 0x1F && messageBody[1] == 0x8B) {
            res = tinf_gzip_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
        }
        if (res != TINF_OK) {
            destLen = sizeUnzipped;
            res = tinf_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
        }

        if (res != TINF_OK) {
            LOG_ERROR("SteamCM", "解压 CMsgMulti 失败 (res={}, size_unzipped={}, compressed_len={})",
                      res, sizeUnzipped, messageBody.size());
            return;
        }
        payload.resize(destLen);
    } else {
        payload.assign(messageBody.begin(), messageBody.end());
    }

    size_t offset = 0;
    while (offset + 4 <= payload.size()) {
        uint32_t subSize = 0;
        std::memcpy(&subSize, payload.data() + offset, 4);
        offset += 4;
        if (offset + subSize > payload.size()) break;

        std::span<const uint8_t> subPacket(payload.data() + offset, subSize);
        offset += subSize;

        uint32_t subEMsg = 0;
        std::span<const uint8_t> subHdr;
        std::span<const uint8_t> subBody;
        if (UnpackSteamMsg(subPacket, subEMsg, subHdr, subBody)) {
            LOG_TRACE("SteamCM", "  -> CMsgMulti 内部消息: eMsg {} (Body: {} 字节)", subEMsg, subBody.size());

            if (subEMsg == static_cast<uint32_t>(ESteamMsg::ClientHeartBeat)) {
                ProtoWriter hb;
                (void)SendProtoMsg(ESteamMsg::ClientHeartBeat, hb);
                continue;
            }

            m_msgQueue.push_back({subEMsg, std::vector<uint8_t>(subBody.begin(), subBody.end())});
        }
    }
}

bool SteamCmClient::ReadMatchingMsg(uint32_t expectedEMsg, std::vector<uint8_t>& outBody, DWORD timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        // 1. Check if expected message already in queue
        for (auto it = m_msgQueue.begin(); it != m_msgQueue.end(); ++it) {
            if (it->eMsg == expectedEMsg) {
                outBody = std::move(it->body);
                m_msgQueue.erase(it);
                return true;
            }
        }

        // 2. Check elapsed timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
        if (elapsed >= static_cast<long long>(timeoutMs)) {
            LOG_WARN("SteamCM", "等待 eMsg {} 响应超时 ({}ms)", expectedEMsg, timeoutMs);
            return false;
        }

        // 3. Receive next WebSocket frame
        std::vector<uint8_t> frame;
        bool isBinary = false;
        DWORD remainingMs = static_cast<DWORD>(timeoutMs - elapsed);
        if (!m_ws.Receive(frame, isBinary, remainingMs)) {
            return false;
        }

        uint32_t eMsg = 0;
        std::span<const uint8_t> hdrSpan;
        std::span<const uint8_t> bodySpan;

        if (UnpackSteamMsg(frame, eMsg, hdrSpan, bodySpan)) {
            LOG_TRACE("SteamCM", "收到 eMsg: {} (Body: {} 字节)", eMsg, bodySpan.size());

            // If heartbeat request from server, reply ClientHeartBeat
            if (eMsg == static_cast<uint32_t>(ESteamMsg::ClientHeartBeat)) {
                ProtoWriter hb;
                (void)SendProtoMsg(ESteamMsg::ClientHeartBeat, hb);
                continue;
            }

            // If multi-container message, unpack all sub-messages into queue
            if (eMsg == 1) { // k_EMsgMulti
                UnpackMultiMsg(bodySpan);
                for (auto it = m_msgQueue.begin(); it != m_msgQueue.end(); ++it) {
                    if (it->eMsg == expectedEMsg) {
                        outBody = std::move(it->body);
                        m_msgQueue.erase(it);
                        return true;
                    }
                }
                continue;
            }

            if (eMsg == expectedEMsg) {
                outBody.assign(bodySpan.begin(), bodySpan.end());
                return true;
            }

            m_msgQueue.push_back({eMsg, std::vector<uint8_t>(bodySpan.begin(), bodySpan.end())});
        }
    }
}

bool SteamCmClient::ConnectAndLogon(uint64_t steamId, std::string_view refreshToken, std::string_view accessToken) {
    Disconnect();
    m_steamId = steamId;
    m_accessToken = accessToken;

    std::string_view tokenToUse = !refreshToken.empty() ? refreshToken : accessToken;
    if (tokenToUse.empty()) {
        LOG_ERROR("SteamCM", "缺少登录令牌 (refreshToken / accessToken 均为空)");
        return false;
    }

    auto cmList = QueryCmWebSockets();
    if (cmList.empty()) {
        LOG_ERROR("SteamCM", "获取 Steam CM WebSocket 节点列表失败");
        return false;
    }

    bool logonSuccess = false;
    const size_t tryLimit = std::min<size_t>(cmList.size(), 8);

    for (size_t i = 0; i < tryLimit; ++i) {
        Disconnect();

        const auto& endpoint = cmList[i];
        std::string wsUrl = std::format("wss://{}/cmsocket/", endpoint);
        LOG_DEBUG("SteamCM", "正在建立 WebSocket 通道连接 CM 节点 [{}/{}]: {}", i + 1, tryLimit, wsUrl);

        if (TuiEngine::IsActive()) {
            int w = 80, h = 25;
            TuiEngine::GetScreenSize(w, h);
            const int modalW = std::clamp(w - 12, 64, 84);
            const int modalH = 6;
            const int top = (h - modalH) / 2, left = (w - modalW) / 2;
            std::string connMsg = std::format("正在连接 CM 节点 ({}/{}) 并登录...", i + 1, tryLimit);
            TuiEngine::PrintBounded(top + 2, left + 4, connMsg, static_cast<size_t>(modalW - 8), "\x1b[1;36m");
            std::cout.flush();
        } else {
            std::cout << std::format("[NET] 正在连接 Steam CM 服务器 ({}/{}): {}...\n", i + 1, tryLimit, endpoint);
        }

        if (!m_ws.Connect(wsUrl, 5000)) {
            LOG_WARN("SteamCM", "连接 CM 节点 {} 失败/超时，尝试下一个节点", endpoint);
            continue;
        }

        LOG_DEBUG("SteamCM", "通信信道已建立 ({})，正在执行会话握手登录 (steamId={}, token={})...",
                  endpoint, MaskSteamId(m_steamId), MaskToken(tokenToUse));

        // Build CMsgClientLogon strictly adhering to Valve Protobuf specifications
        ProtoWriter logonBody;
        logonBody.WriteUInt32(1, 65580);                          // protocol_version
        logonBody.WriteUInt32(3, 0);                              // cell_id
        logonBody.WriteString(6, "schinese");                     // client_language
        logonBody.WriteUInt32(7, 16);                             // client_os_type (Windows 10/11)
        logonBody.WriteBool(8, true);                             // should_remember_password
        if (m_steamId != 0) {
            logonBody.WriteFixed64(22, m_steamId);                // client_supplied_steam_id
        }
        logonBody.WriteUInt32(33, 2);                             // chat_mode = 2 (NewSteamChat)
        logonBody.WriteBool(102, true);                           // supports_rate_limit_response
        logonBody.WriteString(108, tokenToUse);                   // access_token (Valve refresh_token)

        if (!SendProtoMsg(ESteamMsg::ClientLogon, logonBody)) {
            LOG_WARN("SteamCM", "向节点 {} 发送 CMsgClientLogon 失败", endpoint);
            Disconnect();
            continue;
        }

        std::vector<uint8_t> respBody;
        if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientLogonResponse), respBody, 5000)) {
            LOG_WARN("SteamCM", "节点 {} 未在 5000ms 内响应 CMsgClientLogonResponse，切换下一个节点", endpoint);
            Disconnect();
            continue;
        }

        ProtoReader reader(respBody);
        ProtoField field;
        int32_t eresult = 2; // k_EResultFail

        while (reader.ReadNext(field)) {
            if (field.fieldNumber == 1) { // eresult
                eresult = static_cast<int32_t>(field.varintVal);
            } else if (field.fieldNumber == 20) { // client_supplied_steamid
                if (field.fixed64Val != 0) {
                    m_steamId = field.fixed64Val;
                }
            }
        }

        if (eresult == 1) { // 1 == k_EResultOK
            m_isLoggedOn = true;
            logonSuccess = true;
            LOG_INFO("SteamCM", "CM WebSocket 登录成功！节点: {}, SteamID: {}", endpoint, MaskSteamId(m_steamId));
            if (!TuiEngine::IsActive()) {
                std::cout << "[OK] Steam CM 登录就绪！(SteamID: " << m_steamId << ")\n";
            }
            break;
        }

        if (eresult == 48 || eresult == 20) { // 48: TryAnotherCM, 20: ServiceUnavailable
            LOG_WARN("SteamCM", "节点 {} 返回 eresult={} ({})，自动切换下一个 CM 节点...",
                     endpoint, eresult, eresult == 48 ? "TryAnotherCM" : "ServiceUnavailable");
            Disconnect();
            continue;
        }

        std::string reason;
        switch (eresult) {
            case 2:  reason = "通用失败 (Fail)"; break;
            case 5:  reason = "访问被拒绝 / 权限不足 (AccessDenied)"; break;
            case 6:  reason = "账号已在其他位置登录 (LoggedInElsewhere)"; break;
            case 8:  reason = "无效或过期的授权令牌 (InvalidToken)"; break;
            case 15: reason = "无权限 (AccessDenied)"; break;
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

    if (!logonSuccess) {
        LOG_ERROR("SteamCM", "尝试了所有候选 CM 节点，登录均未成功");
        Disconnect();
        return false;
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

std::optional<ParsedAppInfoData> SteamCmClient::RequestPicsProductInfo(uint32_t appId, uint64_t accessToken) {
    if (!IsConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", "正在向 Steam CM 请求 PICS 产品元数据 (AppID={}, token={})...", appId, accessToken);

    ProtoWriter appInfoWriter;
    appInfoWriter.WriteUInt32(1, appId); // appid = 1
    if (accessToken != 0) {
        appInfoWriter.WriteUInt64(2, accessToken); // access_token = 2
    }
    appInfoWriter.WriteBool(3, false); // only_public = 3

    ProtoWriter body;
    body.WriteBytes(2, appInfoWriter.Data()); // apps = 2
    body.WriteBool(3, false); // meta_data_only = 3

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientPICSProductInfoRequest, body, jobId)) {
        return std::nullopt;
    }

    std::vector<uint8_t> respBody;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse), respBody, 8000)) {
        return std::nullopt;
    }

    ProtoReader reader(respBody);
    ProtoField field;
    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1 || field.fieldNumber == 2) {
            ProtoReader appReader(field.bytesVal);
            ProtoField appField;
            uint32_t rAppId = 0;
            std::span<const uint8_t> buffer;
            while (appReader.ReadNext(appField)) {
                if (appField.fieldNumber == 1) { // appid
                    rAppId = static_cast<uint32_t>(appField.varintVal);
                } else if (appField.fieldNumber == 5) { // buffer
                    buffer = appField.bytesVal;
                }
            }
            if (rAppId == appId && !buffer.empty()) {
                LOG_DEBUG("SteamCM", "收到 PICS AppInfo 数据 (大小: {} 字节)", buffer.size());
                return ParseBinaryVdfAppInfo(buffer, appId);
            }
        }
    }

    return std::nullopt;
}

bool SteamCmClient::SetGamePlayed(uint32_t appId) {
    if (!IsConnected()) return false;
    ProtoWriter body;
    if (appId > 0) {
        ProtoWriter gamePlayed;
        gamePlayed.WriteUInt64(1, appId); // game_id = 1
        gamePlayed.WriteUInt32(4, GetCurrentProcessId()); // process_id = 4
        body.WriteBytes(1, gamePlayed.Data()); // games_played = 1
    }
    const uint64_t jobId = ++m_nextJobId;
    LOG_DEBUG("SteamCM", "发送 ClientGamesPlayed (AppID={}, process_id={})", appId, GetCurrentProcessId());
    return SendProtoMsg(ESteamMsg::ClientGamesPlayed, body, jobId);
}

std::string SteamCmClient::FetchManifestRequestCode(
    uint32_t appId, uint32_t depotId, const std::string& manifestId) {
    if (manifestId.empty() || !IsValidManifestId(manifestId)) return "";

    std::string reqCode;

    // 1. Try requesting ManifestRequestCode via CM WebSocket (ContentServerDirectory.GetManifestRequestCode#1, eMsg 5594)
    if (m_ws.IsConnected() && m_isLoggedOn) {
        ProtoWriter innerReq;
        innerReq.WriteUInt32(1, appId); // app_id
        innerReq.WriteUInt32(2, depotId); // depot_id
        innerReq.WriteUInt64(3, std::strtoull(manifestId.c_str(), nullptr, 10)); // manifest_id
        innerReq.WriteString(4, "public"); // app_branch
        innerReq.WriteString(5, ""); // branch_password

        ProtoWriter svcMsg;
        svcMsg.WriteString(1, "ContentServerDirectory.GetManifestRequestCode#1");
        svcMsg.WriteBytes(2, innerReq.Data());

        const uint64_t jobId = ++m_nextJobId;
        LOG_DEBUG("SteamCM", "正在向 Steam CM 请求 ManifestRequestCode: Depot={}, Manifest={}", depotId, manifestId);
        if (SendProtoMsg(ESteamMsg::ClientServiceMethod, svcMsg, jobId)) {
            std::vector<uint8_t> respBody;
            if (ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientServiceMethodResponse), respBody, 4000)) {
                ProtoReader respReader(respBody);
                ProtoField field;
                std::span<const uint8_t> innerBytes;
                while (respReader.ReadNext(field)) {
                    if (field.fieldNumber == 2) { // serialized_method_response
                        innerBytes = field.bytesVal;
                    }
                }
                if (!innerBytes.empty()) {
                    ProtoReader innerReader(innerBytes);
                    ProtoField innerField;
                    uint64_t reqCodeNum = 0;
                    while (innerReader.ReadNext(innerField)) {
                        if (innerField.fieldNumber == 1) { // manifest_request_code
                            reqCodeNum = innerField.varintVal;
                        }
                    }
                    if (reqCodeNum != 0) {
                        reqCode = std::to_string(reqCodeNum);
                        LOG_DEBUG("SteamCM", "CM 成功返回 manifest_request_code: {}", reqCode);
                        return reqCode;
                    }
                }
            }
        }
    }

    // 2. Fallback to WebAPI if CM method was unavailable
    if (!m_accessToken.empty()) {
        WinHttpTransport http;
        std::string reqCodeUrl = std::format(
            "https://api.steampowered.com/IContentServerDirectoryService/GetManifestRequestCode/v1/"
            "?access_token={}&app_id={}&depot_id={}&manifest_id={}&app_branch=public",
            m_accessToken, appId, depotId, manifestId);

        HttpResponse resp = http.Get(reqCodeUrl);
        if (resp.IsSuccess()) {
            auto codeStr = JsonHelper::GetString(resp.body, "manifest_request_code");
            if (codeStr && !codeStr->empty() && *codeStr != "0") {
                reqCode = *codeStr;
                LOG_DEBUG("SteamCM", "WebAPI 成功返回 manifest_request_code: {}", reqCode);
                return reqCode;
            }
            auto codeNum = JsonHelper::GetUInt64(resp.body, "manifest_request_code");
            if (codeNum && *codeNum != 0) {
                reqCode = std::to_string(*codeNum);
                LOG_DEBUG("SteamCM", "WebAPI 成功返回 manifest_request_code: {}", reqCode);
                return reqCode;
            }
        }
    }

    return "";
}

std::vector<std::string> SteamCmClient::GetCdnServers() {
    static std::vector<std::string> s_cachedServers;
    static std::mutex s_mutex;
    std::lock_guard lock(s_mutex);

    if (!s_cachedServers.empty()) {
        return s_cachedServers;
    }

    WinHttpTransport http;
    HttpResponse resp = http.Get("https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=0");
    if (resp.IsSuccess() && !resp.body.empty()) {
        size_t pos = 0;
        while ((pos = resp.body.find("\"host\"", pos)) != std::string::npos) {
            size_t colon = resp.body.find(':', pos + 6);
            if (colon == std::string::npos) break;
            size_t q1 = resp.body.find('"', colon);
            if (q1 == std::string::npos) break;
            size_t q2 = resp.body.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string host = resp.body.substr(q1 + 1, q2 - q1 - 1);
            if (host.ends_with(".steamcontent.com") || host.ends_with(".steampowered.com")) {
                if (std::find(s_cachedServers.begin(), s_cachedServers.end(), host) == s_cachedServers.end()) {
                    s_cachedServers.push_back(std::move(host));
                    if (s_cachedServers.size() >= 10) break;
                }
            }
            pos = q2 + 1;
        }
    }

    if (s_cachedServers.empty()) {
        s_cachedServers = {
            "cache1-lax2.steamcontent.com",
            "cache2-lax2.steamcontent.com",
            "cache3-lax2.steamcontent.com",
            "valve.steamcontent.com",
            "content.steampowered.com"
        };
    }

    return s_cachedServers;
}

std::optional<std::string> SteamCmClient::DownloadManifestOnline(
    uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& destDir) {

    if (appId == 0 || depotId == 0 || !IsValidManifestId(manifestId)) {
        return std::nullopt;
    }

    std::string fileName = std::format("{}_{}.manifest", depotId, manifestId);
    std::string localPath = JoinPath(destDir, fileName);

    // Fast check if already exists in destination
    DWORD attr = GetFileAttributesA(localPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        LOG_INFO("SteamCM", "清单文件已存在于目标目录: {}", localPath);
        return localPath;
    }

    std::string reqCode = FetchManifestRequestCode(appId, depotId, manifestId);
    if (reqCode.empty() || reqCode == "0") {
        LOG_WARN("SteamCM", "未能获得 manifest_request_code (Depot={}, Manifest={})", depotId, manifestId);
        return std::nullopt;
    }

    auto cdnServers = GetCdnServers();
    WinHttpTransport http;
    HttpResponse cdnResp;

    for (const auto& server : cdnServers) {
        std::string manifestUrl = std::format(
            "https://{}/depot/{}/manifest/{}/5/{}",
            server, depotId, manifestId, reqCode);

        LOG_DEBUG("SteamCM", "正在从 Steam CDN 下载清单: {}", manifestUrl);
        cdnResp = http.Get(manifestUrl);
        if (cdnResp.IsSuccess() && !cdnResp.body.empty()) {
            LOG_INFO("SteamCM", "成功从 Steam CDN ({}) 获取清单数据 ({} 字节)", server, cdnResp.body.size());
            break;
        }
    }

    if (!cdnResp.IsSuccess() || cdnResp.body.empty()) {
        LOG_WARN("SteamCM", "从 Steam CDN 下载清单失败 (HTTP {}): {}", cdnResp.statusCode, cdnResp.errorMessage);
        return std::nullopt;
    }

    if (!CreateDirectoryA(destDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        LOG_WARN("SteamCM", "创建目录失败: {}", destDir);
    }

    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(cdnResp.body.data()),
        cdnResp.body.size());

    std::vector<uint8_t> uncompressed;
    if (payload.size() >= 2 && payload[0] == 0x1F && payload[1] == 0x8B) {
        uint32_t isize = 0;
        if (payload.size() >= 18) {
            std::memcpy(&isize, payload.data() + payload.size() - 4, sizeof(uint32_t));
        }
        unsigned int destLen = (isize > 0 && isize < 256 * 1024 * 1024)
            ? (isize + 1024)
            : static_cast<unsigned int>(payload.size() * 15);
        uncompressed.resize(destLen);

        int res = tinf_gzip_uncompress(uncompressed.data(), &destLen, payload.data(), static_cast<unsigned int>(payload.size()));
        if (res == TINF_BUF_ERROR) {
            destLen = static_cast<unsigned int>(uncompressed.size() * 2);
            uncompressed.resize(destLen);
            res = tinf_gzip_uncompress(uncompressed.data(), &destLen, payload.data(), static_cast<unsigned int>(payload.size()));
        }

        if (res == TINF_OK) {
            uncompressed.resize(destLen);
            payload = uncompressed;
            LOG_DEBUG("SteamCM", "清单 GZIP 解压成功 ({} 压缩 -> {} 原始字节)", cdnResp.body.size(), destLen);
        } else {
            LOG_WARN("SteamCM", "清单 GZIP 解压失败 (res={}), 将以原始数据保存", res);
        }
    }

    if (!WriteBinaryFile(localPath, payload)) {
        LOG_WARN("SteamCM", "保存清单文件失败: {}", localPath);
        return std::nullopt;
    }

    LOG_INFO("SteamCM", "成功直接在线下载并保存清单文件: {} ({} 字节)", fileName, payload.size());
    return localPath;
}

ExtractedAppCredentials SteamCmClient::ExtractFullCredentials(uint32_t appId) {
    ExtractedAppCredentials creds;
    creds.appId = appId;

    // 1. AppOwnershipTicket
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
    }

    // 2. EncryptedAppTicket
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
    }

    // 2.1 针对家庭共享/临时会话激活：若票据缺失，通过 ClientGamesPlayed (eMsg 742) 建立游戏会话重试提取
    if (!creds.appOwnershipTicket || !creds.encryptedAppTicket) {
        LOG_INFO("SteamCM", "检测到 AppID {} 票据未就绪，尝试通过 ClientGamesPlayed 激活会话授权...", appId);
        SetGamePlayed(appId);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (!creds.appOwnershipTicket) {
            creds.appOwnershipTicket = RequestAppOwnershipTicket(appId);
            if (creds.appOwnershipTicket) {
                LOG_INFO("SteamCM", "会话激活后成功提取到所有权票据 ({} 字节)", creds.appOwnershipTicket->size());
                if (!TuiEngine::IsActive()) {
                    std::cout << "     [OK] 激活会话提取到所有权票据 (" << creds.appOwnershipTicket->size() << " 字节)\n";
                }
            }
        }
        if (!creds.encryptedAppTicket) {
            creds.encryptedAppTicket = RequestEncryptedAppTicket(appId);
            if (creds.encryptedAppTicket) {
                LOG_INFO("SteamCM", "会话激活后成功提取到加密票据 ({} 字节)", creds.encryptedAppTicket->size());
                if (!TuiEngine::IsActive()) {
                    std::cout << "     [OK] 激活会话提取到加密票据 (" << creds.encryptedAppTicket->size() << " 字节)\n";
                }
            }
        }
        SetGamePlayed(0);
    }

    // 2.2 本地 Steam 客户端会话兜底：若所有权票据缺失，尝试通过本地 Steam 客户端运行时同步凭证
    if (!creds.appOwnershipTicket) {
        LOG_INFO("SteamCM", "尝试从本地 Steam 客户端提取 AppID {} 凭据兜底...", appId);
        bool localExtracted = ExtractTicketsFromLocalClient(appId, creds.appOwnershipTicket, creds.encryptedAppTicket);
        (void)localExtracted;
        if (creds.appOwnershipTicket && !TuiEngine::IsActive()) {
            std::cout << "     [OK] 从本地客户端同步到所有权票据 (" << creds.appOwnershipTicket->size() << " 字节)\n";
        }
        if (creds.encryptedAppTicket && !TuiEngine::IsActive()) {
            std::cout << "     [OK] 从本地客户端同步到加密票据 (" << creds.encryptedAppTicket->size() << " 字节)\n";
        }
    }

    if (!creds.appOwnershipTicket) {
        LOG_INFO("SteamCM", "未能获取所有权票据 (账号未直接拥有或属于共享借用；OpenSteamTool 将在运行时自动执行 AppID 7 伪造兜底)");
    }
    if (!creds.encryptedAppTicket) {
        LOG_INFO("SteamCM", "未能获取加密票据 (无加密运行时授权需求或未拥有)");
    }

    // 3. 收集该游戏拥有的所有 Depots、清单号与 DLC
    auto steamPathOpt = FindSteamInstallPath();
    std::string steamPath = steamPathOpt ? *steamPathOpt : "";

    std::vector<DlcInfo> localDlcs;
    std::vector<DepotKeyInfo> depotKeys = ExtractDepotDecryptionKeys(
        steamPath, appId, nullptr, 0, 0, localDlcs);

    creds.dlcs = std::move(localDlcs);

    // 从本地 appinfo.vdf 或 Steam CM PICS 获取完整结构 (Depots, GID, DLC)
    std::optional<ParsedAppInfoData> appInfoData;
    if (!steamPath.empty()) {
        appInfoData = ParseAppInfoDepots(steamPath, appId);
    }
    if (!appInfoData) {
        uint64_t appToken = 0;
        auto tokMap = RequestAppTokens({ appId });
        if (tokMap.contains(appId)) appToken = tokMap[appId];
        appInfoData = RequestPicsProductInfo(appId, appToken);
    }

    if (appInfoData) {
        LOG_INFO("SteamCM", "获得游戏 [{}] 完整 AppInfo (共 {} 个 Depot, {} 个 DLC)",
                 appId, appInfoData->depots.size(), appInfoData->dlcAppIds.size());

        std::unordered_set<uint32_t> knownDlcSet;
        for (const auto& d : creds.dlcs) knownDlcSet.insert(d.dlcId);

        for (uint32_t dlcId : appInfoData->dlcAppIds) {
            if (dlcId != appId && !knownDlcSet.contains(dlcId)) {
                knownDlcSet.insert(dlcId);
                DlcInfo dInfo;
                dInfo.dlcId = dlcId;
                creds.dlcs.push_back(std::move(dInfo));
            }
        }

        for (const auto& dInfo : appInfoData->depots) {
            auto it = std::find_if(depotKeys.begin(), depotKeys.end(),
                                   [&](const DepotKeyInfo& k) { return k.depotId == dInfo.depotId; });
            if (it != depotKeys.end()) {
                if (it->manifestId.empty() && !dInfo.manifestId.empty()) {
                    it->manifestId = dInfo.manifestId;
                }
                if (it->dlcId == 0 && dInfo.dlcId > 0) {
                    it->dlcId = dInfo.dlcId;
                }
            } else {
                DepotKeyInfo newDk;
                newDk.depotId = dInfo.depotId;
                newDk.manifestId = dInfo.manifestId;
                newDk.dlcId = dInfo.dlcId;
                depotKeys.push_back(std::move(newDk));
            }
        }
    }

    // 4. 收集所有需要向 Steam CM 补充查询密钥的 Depot ID
    std::unordered_set<uint32_t> depotsNeedingKeys;
    depotsNeedingKeys.insert(appId);
    for (const auto& dk : depotKeys) {
        if (dk.hexKey.empty()) {
            depotsNeedingKeys.insert(dk.depotId);
        }
    }
    for (const auto& dlc : creds.dlcs) {
        depotsNeedingKeys.insert(dlc.dlcId);
    }

    LOG_DEBUG("SteamCM", "正在向 Steam CM 查询 Depot 解密密钥 (AppID={}, 共 {} 个待查 Depot)...", appId, depotsNeedingKeys.size());
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在向 Steam CM 查询 Depot 解密密钥...\n";
    }

    std::vector<uint32_t> queryDepotList(depotsNeedingKeys.begin(), depotsNeedingKeys.end());
    std::sort(queryDepotList.begin(), queryDepotList.end());

    std::vector<DepotKeyInfo> cmKeys = RequestDepotKeys(appId, queryDepotList);

    std::unordered_map<uint32_t, std::string> cmKeyMap;
    for (const auto& cmKey : cmKeys) {
        if (!cmKey.hexKey.empty()) {
            cmKeyMap[cmKey.depotId] = cmKey.hexKey;
        }
    }

    for (auto& dk : depotKeys) {
        if (dk.hexKey.empty()) {
            auto it = cmKeyMap.find(dk.depotId);
            if (it != cmKeyMap.end()) {
                dk.hexKey = it->second;
            }
        }
    }

    for (const auto& cmKey : cmKeys) {
        if (std::none_of(depotKeys.begin(), depotKeys.end(),
                         [&](const DepotKeyInfo& k) { return k.depotId == cmKey.depotId; })) {
            depotKeys.push_back(cmKey);
        }
    }

    // 5. 本地 depotcache 清单查找 + 智能在线清单下载
    std::string outDir = std::to_string(appId);
    std::vector<std::string> depotcacheDirs;
    depotcacheDirs.push_back(outDir); // 优先检查当前 AppID 目标目录
    if (!steamPath.empty()) {
        auto sysDirs = GetDepotcacheDirs(steamPath);
        depotcacheDirs.insert(depotcacheDirs.end(), sysDirs.begin(), sysDirs.end());
    }

    for (auto& dk : depotKeys) {
        if (dk.manifestFilePath.empty() && !depotcacheDirs.empty()) {
            dk.manifestFilePath = FindDepotManifestFile(depotcacheDirs, dk.depotId, dk.manifestId);
        }

        // 若本地没有清单文件且拥有有效清单号，直接通过 Steam 官方 CDN 在线拉取！
        if (dk.manifestFilePath.empty() && IsValidManifestId(dk.manifestId)) {
            auto downloaded = DownloadManifestOnline(appId, dk.depotId, dk.manifestId, outDir);
            if (downloaded) {
                dk.manifestFilePath = *downloaded;
            }
        }
    }


    // 剔除无有效密钥、无清单文件且无有效清单ID的空项
    std::erase_if(depotKeys, [](const DepotKeyInfo& dk) {
        return dk.hexKey.empty() && dk.manifestFilePath.empty() && !IsValidManifestId(dk.manifestId);
    });

    std::sort(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& a, const DepotKeyInfo& b) {
        return a.depotId < b.depotId;
    });

    creds.depotKeys = std::move(depotKeys);

    if (!creds.depotKeys.empty()) {
        LOG_INFO("SteamCM", "提取到 {} 个 Depot 解密密钥/清单", creds.depotKeys.size());
        if (!TuiEngine::IsActive()) {
            std::cout << "     [OK] 提取到 " << creds.depotKeys.size() << " 个 Depot 解密密钥/清单\n";
        }
    }

    // 6. 查询 64 位 PICS AccessTokens (AppID + 全部 DLC)
    LOG_DEBUG("SteamCM", "正在查询 64 位 PICS AccessToken (AppID={})...", appId);
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在查询 64 位 PICS AccessToken...\n";
    }

    std::unordered_set<uint32_t> targetAppIds;
    targetAppIds.insert(appId);
    for (const auto& dlc : creds.dlcs) {
        targetAppIds.insert(dlc.dlcId);
    }

    if (!steamPath.empty()) {
        creds.appTokens = ParseAppInfoTokens(steamPath, &targetAppIds);
    }

    const auto luaFallback = ParseLuaFallbackData(steamPath, appId);
    for (const auto& [tId, tVal] : luaFallback.appTokens) {
        if (tVal != 0) {
            creds.appTokens.try_emplace(tId, tVal);
        }
    }

    std::vector<uint32_t> tokensToQuery;
    for (uint32_t tApp : targetAppIds) {
        if (!creds.appTokens.contains(tApp) || creds.appTokens[tApp] == 0) {
            tokensToQuery.push_back(tApp);
        }
    }

    if (!tokensToQuery.empty()) {
        auto cmTokens = RequestAppTokens(tokensToQuery);
        for (const auto& [tApp, tVal] : cmTokens) {
            if (tVal != 0) {
                creds.appTokens[tApp] = tVal;
            }
        }
    }

    if (!creds.appTokens.empty()) {
        LOG_INFO("SteamCM", "提取到 PICS 访问令牌 (共 {} 个)", creds.appTokens.size());
        if (!TuiEngine::IsActive()) {
            std::cout << "     [OK] 提取到 PICS 访问令牌\n";
        }
    }

    return creds;
}

} // namespace OST::ExtractTickets
