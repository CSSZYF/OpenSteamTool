#include "SteamCmClient.h"
#include "AppInfoParser.h"
#include "I18n.h"
#include "JsonHelper.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"
#include "tinf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace OST::ExtractTickets {

namespace {
    std::vector<std::string> s_cachedCdnServers;
    std::mutex s_cdnMutex;

    std::string FetchAppNameFromStore(uint32_t appId, WinHttpTransport* httpPtr = nullptr) {
        WinHttpTransport localHttp;
        WinHttpTransport& http = httpPtr ? *httpPtr : localHttp;
        HttpResponse resp = http.Get(std::format("https://store.steampowered.com/api/appdetails?appids={}&filters=basic&l={}",
                                                  appId, I18n::GetSteamLanguageCode()));
        if (!resp.IsSuccess()) return "";
        auto nameOpt = JsonHelper::GetString(resp.body, "name");
        return nameOpt.value_or("");
    }

    static std::vector<std::string> s_cachedCmList;
    static std::mutex s_cmListMutex;

    void PromoteWorkingCmServer(std::string_view endpoint) {
        if (endpoint.empty()) return;
        std::lock_guard lock(s_cmListMutex);
        auto it = std::find(s_cachedCmList.begin(), s_cachedCmList.end(), endpoint);
        if (it != s_cachedCmList.end() && it != s_cachedCmList.begin()) {
            std::string s = std::move(*it);
            s_cachedCmList.erase(it);
            s_cachedCmList.insert(s_cachedCmList.begin(), std::move(s));
        }
    }

    std::vector<std::string> QueryCmWebSockets() {
        std::lock_guard lock(s_cmListMutex);
        if (!s_cachedCmList.empty()) {
            return s_cachedCmList;
        }

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

        s_cachedCmList = endpoints;
        return s_cachedCmList;
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

bool SteamCmClient::SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId, std::string_view targetJobName) {
    if (eMsg != ESteamMsg::ClientLogon) {
        if (!EnsureConnected()) return false;
    } else {
        if (!m_ws.IsConnected()) return false;
    }

    auto packet = PackSteamMsg(eMsg, m_steamId, jobId, body.Data(), m_clientSessionId, targetJobName);
    LOG_TRACE("SteamCM", "发送 eMsg: {} ({} 字节)", static_cast<uint32_t>(eMsg), packet.size());
    if (!m_ws.Send(packet, true)) {
        if (eMsg != ESteamMsg::ClientLogon) {
            LOG_WARN("SteamCM", "发送 eMsg: {} 失败，连接可能已失效，尝试重新建立安全会话并重试...", static_cast<uint32_t>(eMsg));
            m_ws.Close();
            m_isLoggedOn = false;
            if (EnsureConnected()) {
                LOG_INFO("SteamCM", "连接已成功恢复，正在重发 eMsg: {} (jobId={})...", static_cast<uint32_t>(eMsg), jobId);
                return m_ws.Send(packet, true);
            }
        }
        return false;
    }
    return true;
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
        while (res == TINF_BUF_ERROR && payload.size() < 64 * 1024 * 1024) {
            destLen = static_cast<unsigned int>(payload.size() * 2);
            payload.resize(destLen);
            if (messageBody.size() >= 2 && messageBody[0] == 0x1F && messageBody[1] == 0x8B) {
                res = tinf_gzip_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
            } else {
                res = tinf_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
            }
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
            int32_t subEResult = ExtractHeaderEResult(subHdr);
            uint64_t subTargetJobId = ExtractHeaderTargetJobId(subHdr);
            LOG_TRACE("SteamCM", "  -> CMsgMulti 内部消息: eMsg {} (EResult: {}, targetJobId: {}, Body: {} 字节)",
                      subEMsg, subEResult, subTargetJobId, subBody.size());

            if (subEMsg == static_cast<uint32_t>(ESteamMsg::ClientHeartBeat)) {
                ProtoWriter hb;
                (void)SendProtoMsg(ESteamMsg::ClientHeartBeat, hb);
                continue;
            }

            m_msgQueue.push_back({subEMsg, subTargetJobId, subEResult, std::vector<uint8_t>(subBody.begin(), subBody.end())});
        }
    }
}

bool SteamCmClient::ReadMatchingMsg(
    uint32_t expectedEMsg,
    std::vector<uint8_t>& outBody,
    DWORD timeoutMs,
    int32_t* outEResult,
    uint64_t expectedJobId) {

    auto startTime = std::chrono::steady_clock::now();

    auto isMatch = [&](uint32_t eMsg, uint64_t targetJobId) {
        if (eMsg != expectedEMsg) return false;
        if (expectedJobId != 0 && targetJobId != 0 && targetJobId != expectedJobId) return false;
        return true;
    };

    while (true) {
        // 1. Check if expected message already in queue
        for (auto it = m_msgQueue.begin(); it != m_msgQueue.end(); ++it) {
            if (isMatch(it->eMsg, it->targetJobId)) {
                if (outEResult) *outEResult = it->eresult;
                outBody = std::move(it->body);
                m_msgQueue.erase(it);
                return true;
            }
        }

        // 2. Check elapsed timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
        if (elapsed >= static_cast<long long>(timeoutMs)) {
            LOG_WARN("SteamCM", "等待 eMsg {} (jobId={}) 响应超时 ({}ms)", expectedEMsg, expectedJobId, timeoutMs);
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
            int32_t eresult = ExtractHeaderEResult(hdrSpan);
            uint64_t targetJobId = ExtractHeaderTargetJobId(hdrSpan);
            LOG_TRACE("SteamCM", "收到 eMsg: {} (EResult: {}, targetJobId: {}, Body: {} 字节)",
                      eMsg, eresult, targetJobId, bodySpan.size());

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
                    if (isMatch(it->eMsg, it->targetJobId)) {
                        if (outEResult) *outEResult = it->eresult;
                        outBody = std::move(it->body);
                        m_msgQueue.erase(it);
                        return true;
                    }
                }
                continue;
            }

            if (isMatch(eMsg, targetJobId)) {
                if (outEResult) *outEResult = eresult;
                outBody.assign(bodySpan.begin(), bodySpan.end());
                return true;
            }

            m_msgQueue.push_back({eMsg, targetJobId, eresult, std::vector<uint8_t>(bodySpan.begin(), bodySpan.end())});
        }
    }
}

bool SteamCmClient::ConnectAndLogon(uint64_t steamId, std::string_view refreshToken, std::string_view accessToken) {
    Disconnect();
    m_steamId = steamId;
    m_refreshToken = refreshToken;
    m_accessToken = accessToken;

    std::string_view tokenToUse = !m_refreshToken.empty() ? m_refreshToken : m_accessToken;
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
        logonBody.WriteString(6, I18n::GetSteamLanguageCode());   // client_language
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
            } else if (field.fieldNumber == 14) { // client_sessionid
                m_clientSessionId = static_cast<int32_t>(field.varintVal);
            } else if (field.fieldNumber == 20) { // client_supplied_steamid
                if (field.fixed64Val != 0) {
                    m_steamId = field.fixed64Val;
                }
            } else if (field.fieldNumber == 43) { // cell_id
                m_cellId = static_cast<uint32_t>(field.varintVal);
            }
        }

        if (eresult == 1) { // 1 == k_EResultOK
            m_isLoggedOn = true;
            logonSuccess = true;
            PromoteWorkingCmServer(endpoint);
            LOG_INFO("SteamCM", "CM WebSocket 登录成功！节点: {}, SteamID: {}, SessionID: {}, CellID: {}",
                     endpoint, MaskSteamId(m_steamId), m_clientSessionId, m_cellId);
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

bool SteamCmClient::EnsureConnected() {
    if (IsConnected()) return true;
    if (m_steamId == 0 || (m_refreshToken.empty() && m_accessToken.empty())) return false;
    LOG_INFO("SteamCM", "检测到 CM 连接已断开，正在自动恢复会话...");
    return ConnectAndLogon(m_steamId, m_refreshToken, m_accessToken);
}

std::optional<std::vector<uint8_t>> SteamCmClient::RequestAppOwnershipTicket(uint32_t appId) {
    if (!EnsureConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", "正在请求 AppOwnershipTicket (AppID={})", appId);

    ProtoWriter body;
    body.WriteUInt32(1, appId); // app_id = 1

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientGetAppOwnershipTicket, body, jobId)) {
        return std::nullopt;
    }

    std::vector<uint8_t> respBody;
    // Expected response eMsg is 858 (CMsgClientGetAppOwnershipTicketResponse)
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientGetAppOwnershipTicketResponse), respBody, 8000, nullptr, jobId)) {
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
    if (!EnsureConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", "正在请求 EncryptedAppTicket (AppID={})", appId);

    ProtoWriter body;
    body.WriteUInt32(1, appId); // app_id = 1

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientRequestEncryptedAppTicket, body, jobId)) {
        return std::nullopt;
    }

    std::vector<uint8_t> respBody;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientRequestEncryptedAppTicketResponse), respBody, 8000, nullptr, jobId)) {
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
            // The Steamworks SDK API (ISteamUser::GetEncryptedAppTicket / SteamEncryptedAppTicket_BDecryptTicket)
            // expects the entire serialized EncryptedAppTicket container (ticket_version_no, crc,
            // cb_encrypteduserdata, cb_encrypted_appownershipticket, and encrypted_ticket), matching
            // the exact binary format delivered by genuine Steam Client IPC.
            ProtoReader subReader(field.bytesVal);
            ProtoField subField;
            bool hasEncryptedTicket = false;
            while (subReader.ReadNext(subField)) {
                if (subField.fieldNumber == 5 && !subField.bytesVal.empty()) {
                    hasEncryptedTicket = true;
                    break;
                }
            }
            if (hasEncryptedTicket) {
                ticket.assign(field.bytesVal.begin(), field.bytesVal.end());
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
    if (!EnsureConnected()) return keys;

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
        if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientGetDepotDecryptionKeyResponse), respBody, 6000, nullptr, jobId)) {
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

        if (eresult == 1) {
            DepotKeyInfo info;
            info.depotId = depotId;
            if (!keyBytes.empty()) {
                info.hexKey = ToHexString(keyBytes);
                LOG_INFO("SteamCM", "获取到 Depot {} 解密密钥: {}", depotId, MaskKeyHex(info.hexKey));
            } else {
                LOG_INFO("SteamCM", "Depot {} 为免密/公共 Depot (eresult=1)", depotId);
            }
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
    if (appIds.empty() || !EnsureConnected()) return tokens;

    ProtoWriter body;
    for (uint32_t appId : appIds) {
        body.WriteUInt32(2, appId); // appids = 2 (repeated)
    }

    const uint64_t jobId = ++m_nextJobId;
    if (!SendProtoMsg(ESteamMsg::ClientPICSAccessTokenRequest, body, jobId)) {
        return tokens;
    }

    std::vector<uint8_t> respBody;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSAccessTokenResponse), respBody, 8000, nullptr, jobId)) {
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
                LOG_DEBUG("SteamCM", "获得 AppID {} 的 PICS AccessToken: {}", aId, MaskPicsToken(aToken));
            }
        } else if (field.fieldNumber == 4) { // app_denied_tokens (repeated)
            uint32_t deniedAppId = static_cast<uint32_t>(field.varintVal);
            LOG_DEBUG("SteamCM", "AppID {} 属于公开产品，无需独立 PICS AccessToken (使用公开模式 token=0)", deniedAppId);
        }
    }

    return tokens;
}

std::optional<ParsedAppInfoData> SteamCmClient::RequestPicsProductInfo(uint32_t appId, uint64_t accessToken) {
    if (!EnsureConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", "正在向 Steam CM 请求 PICS 产品元数据 (AppID={}, token={})...", appId, MaskPicsToken(accessToken));

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
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse), respBody, 8000, nullptr, jobId)) {
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

std::unordered_map<uint32_t, std::string> SteamCmClient::RequestPicsAppNames(
    const std::vector<uint32_t>& appIds) {

    std::unordered_map<uint32_t, std::string> outNames;
    if (appIds.empty() || !EnsureConnected()) return outNames;

    constexpr size_t kBatchSize = 50;
    for (size_t i = 0; i < appIds.size(); i += kBatchSize) {
        const size_t chunkEnd = std::min(i + kBatchSize, appIds.size());

        ProtoWriter body;
        for (size_t j = i; j < chunkEnd; ++j) {
            ProtoWriter appReq;
            appReq.WriteUInt32(1, appIds[j]); // appid = 1
            appReq.WriteBool(3, true);        // only_public = 3 (public common section contains name)
            body.WriteSubMessage(2, appReq);  // apps = 2 (repeated)
        }
        body.WriteBool(3, false); // meta_data_only = 3

        const uint64_t jobId = ++m_nextJobId;
        if (!SendProtoMsg(ESteamMsg::ClientPICSProductInfoRequest, body, jobId)) {
            continue;
        }

        std::vector<uint8_t> respBody;
        if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse), respBody, 8000, nullptr, jobId)) {
            continue;
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
                if (rAppId > 0 && !buffer.empty()) {
                    auto appInfo = ParseBinaryVdfAppInfo(buffer, rAppId);
                    if (appInfo && !appInfo->name.empty()) {
                        outNames[rAppId] = std::move(appInfo->name);
                    }
                }
            }
        }
    }

    return outNames;
}

bool SteamCmClient::SetGamePlayed(uint32_t appId) {
    if (!EnsureConnected()) return false;
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
    uint32_t appId, uint32_t depotId, const std::string& manifestId, bool* outAccessDenied) {
    if (outAccessDenied) *outAccessDenied = false;
    if (manifestId.empty() || !IsValidManifestId(manifestId)) return "";

    // 0. Ensure active CM connection
    (void)EnsureConnected();

    std::string reqCode;

    ProtoWriter innerReq;
    innerReq.WriteUInt32(1, appId); // app_id
    innerReq.WriteUInt32(2, depotId); // depot_id
    innerReq.WriteUInt64(3, std::strtoull(manifestId.c_str(), nullptr, 10)); // manifest_id
    innerReq.WriteString(4, "public"); // app_branch
    innerReq.WriteString(5, ""); // branch_password

    int32_t rpcResult = 0;
    bool gotReply = false;

    // Track 1: Unified RPC eMsg 151 (ServiceMethodCallFromClient)
    if (m_ws.IsConnected() && m_isLoggedOn) {
        const uint64_t jobId = ++m_nextJobId;
        LOG_DEBUG("SteamCM", "向 Steam CM 发送统一 RPC 请求 ManifestRequestCode: Depot={}, Manifest={}", depotId, manifestId);
        if (SendProtoMsg(ESteamMsg::ServiceMethodCallFromClient, innerReq, jobId, "ContentServerDirectory.GetManifestRequestCode#1")) {
            std::vector<uint8_t> respBody;
            if (ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ServiceMethodResponse), respBody, 4000, &rpcResult, jobId) ||
                ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ServiceMethodSendToClient), respBody, 2000, &rpcResult, jobId)) {
                gotReply = true;
                if (rpcResult == 1) { // k_EResultOK
                    ProtoReader respReader(respBody);
                    ProtoField field;
                    uint64_t codeVal = 0;
                    while (respReader.ReadNext(field)) {
                        if (field.fieldNumber == 1) { // manifest_request_code
                            codeVal = field.varintVal ? field.varintVal : field.fixed64Val;
                        }
                    }
                    if (codeVal != 0) {
                        reqCode = std::to_string(codeVal);
                        LOG_DEBUG("SteamCM", "统一 RPC eMsg 151 成功返回 manifest_request_code: {}", reqCode);
                        return reqCode;
                    }
                } else {
                    LOG_DEBUG("SteamCM", "统一 RPC eMsg 151 返回非成功结果 (eresult={})，账号未拥有 Depot {} 授权", rpcResult, depotId);
                    if (outAccessDenied) *outAccessDenied = true;
                    return ""; // Short-circuit: definitively not owned, never do redundant Track 2 fallback
                }
            }
        }
    }

    // Track 2: Legacy wrapper eMsg 5594 (ClientServiceMethod) fallback (only if Track 1 received no response from CM)
    if (!gotReply && m_ws.IsConnected() && m_isLoggedOn) {
        ProtoWriter svcMsg;
        svcMsg.WriteString(1, "ContentServerDirectory.GetManifestRequestCode#1");
        svcMsg.WriteBytes(2, innerReq.Data());

        const uint64_t jobId = ++m_nextJobId;
        LOG_DEBUG("SteamCM", "回退尝试传统 eMsg 5594 请求 ManifestRequestCode: Depot={}, Manifest={}", depotId, manifestId);
        if (SendProtoMsg(ESteamMsg::ClientServiceMethod, svcMsg, jobId)) {
            std::vector<uint8_t> respBody;
            int32_t legacyResult = 0;
            if (ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientServiceMethodResponse), respBody, 4000, &legacyResult, jobId)) {
                if (legacyResult != 1) {
                    if (outAccessDenied) *outAccessDenied = true;
                    return "";
                }
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
                            reqCodeNum = innerField.varintVal ? innerField.varintVal : innerField.fixed64Val;
                        }
                    }
                    if (reqCodeNum != 0) {
                        reqCode = std::to_string(reqCodeNum);
                        LOG_DEBUG("SteamCM", "传统 RPC eMsg 5594 成功返回 manifest_request_code: {}", reqCode);
                        return reqCode;
                    }
                }
            }
        }
    }

    return "";
}

void SteamCmClient::PromoteWorkingCdnServer(std::string_view server) {
    if (server.empty()) return;
    std::lock_guard lock(s_cdnMutex);
    auto it = std::find(s_cachedCdnServers.begin(), s_cachedCdnServers.end(), server);
    if (it != s_cachedCdnServers.end() && it != s_cachedCdnServers.begin()) {
        std::string s = std::move(*it);
        s_cachedCdnServers.erase(it);
        s_cachedCdnServers.insert(s_cachedCdnServers.begin(), std::move(s));
    }
}

std::vector<std::string> SteamCmClient::GetCdnServers(uint32_t cellId) {
    std::lock_guard lock(s_cdnMutex);

    if (!s_cachedCdnServers.empty()) {
        return s_cachedCdnServers;
    }

    WinHttpTransport http;
    std::string directoryUrl = std::format(
        "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id={}",
        cellId);
    HttpResponse resp = http.Get(directoryUrl);
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
                if (std::find(s_cachedCdnServers.begin(), s_cachedCdnServers.end(), host) == s_cachedCdnServers.end()) {
                    s_cachedCdnServers.push_back(std::move(host));
                    if (s_cachedCdnServers.size() >= 10) break;
                }
            }
            pos = q2 + 1;
        }
    }

    if (s_cachedCdnServers.empty()) {
        s_cachedCdnServers = {
            "valve.steamcontent.com",
            "cdn.steamcontent.com",
            "content1.steampowered.com",
            "content2.steampowered.com",
            "content.steampowered.com",
            "cache1-lax2.steamcontent.com"
        };
    }

    return s_cachedCdnServers;
}

std::optional<std::string> SteamCmClient::DownloadManifestPayload(
    uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& reqCode,
    const std::string& destDir, const std::vector<std::string>& cdnServers, WinHttpTransport* httpPtr) {

    if (appId == 0 || depotId == 0 || !IsValidManifestId(manifestId) || reqCode.empty() || reqCode == "0") {
        return std::nullopt;
    }

    std::string fileName = std::format("{}_{}.manifest", depotId, manifestId);
    std::filesystem::path localPath = std::filesystem::path(destDir) / fileName;

    std::error_code checkEc;
    if (std::filesystem::is_regular_file(localPath, checkEc) && std::filesystem::file_size(localPath, checkEc) > 0) {
        LOG_INFO("SteamCM", "清单文件已存在于目标目录，直接秒级复用: {}", localPath.string());
        return localPath.string();
    }

    WinHttpTransport localHttp;
    WinHttpTransport& http = httpPtr ? *httpPtr : localHttp;
    HttpResponse cdnResp;

    for (const auto& server : cdnServers) {
        std::string manifestUrl = std::format(
            "https://{}/depot/{}/manifest/{}/5/{}",
            server, depotId, manifestId, reqCode);

        LOG_DEBUG("SteamCM", "正在从 Steam CDN 下载清单: {}", manifestUrl);
        cdnResp = http.Get(manifestUrl);
        if (cdnResp.IsSuccess() && !cdnResp.body.empty()) {
            LOG_INFO("SteamCM", "成功从 Steam CDN ({}) 获取清单数据 ({} 字节)", server, cdnResp.body.size());
            PromoteWorkingCdnServer(server);
            break;
        }

        // Fallback to HTTP on port 80 if HTTPS was rejected or throttled by ISP/CDN edge
        std::string httpManifestUrl = std::format(
            "http://{}/depot/{}/manifest/{}/5/{}",
            server, depotId, manifestId, reqCode);
        LOG_DEBUG("SteamCM", "正在尝试 HTTP 降级下载清单: {}", httpManifestUrl);
        cdnResp = http.Get(httpManifestUrl);
        if (cdnResp.IsSuccess() && !cdnResp.body.empty()) {
            LOG_INFO("SteamCM", "成功从 Steam CDN HTTP ({}) 获取清单数据 ({} 字节)", server, cdnResp.body.size());
            PromoteWorkingCdnServer(server);
            break;
        }
    }

    if (!cdnResp.IsSuccess() || cdnResp.body.empty()) {
        LOG_WARN("SteamCM", "从 Steam CDN 下载清单失败 (Depot={}, Manifest={}): HTTP {} {}",
                 depotId, manifestId, cdnResp.statusCode, cdnResp.errorMessage);
        return std::nullopt;
    }

    std::error_code dirEc;
    std::filesystem::create_directories(std::filesystem::path(destDir), dirEc);
    if (dirEc) {
        LOG_WARN("SteamCM", "创建目录失败: {} ({})", destDir, dirEc.message());
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
        size_t bufSize = (isize > 0 && isize < 256 * 1024 * 1024)
            ? (static_cast<size_t>(isize) + 4096)
            : (std::max<size_t>(payload.size() * 10, 1024 * 1024));
        bufSize = (std::max<size_t>)(bufSize, 1024 * 1024);
        uncompressed.resize(bufSize);

        int res = TINF_DATA_ERROR;
        while (bufSize <= 256 * 1024 * 1024) {
            unsigned int destLen = static_cast<unsigned int>(bufSize);
            res = tinf_gzip_uncompress(uncompressed.data(), &destLen, payload.data(), static_cast<unsigned int>(payload.size()));
            if (res == TINF_OK) {
                uncompressed.resize(destLen);
                payload = uncompressed;
                LOG_DEBUG("SteamCM", "清单 GZIP 解压成功 ({} 压缩 -> {} 原始字节)", cdnResp.body.size(), destLen);
                break;
            }
            if (res == TINF_BUF_ERROR) {
                bufSize *= 2;
                uncompressed.resize(bufSize);
                continue;
            }
            break; // Fatal format error
        }

        if (res != TINF_OK) {
            LOG_WARN("SteamCM", "清单 GZIP 解压失败 (res={}), 丢弃损坏数据", res);
            return std::nullopt;
        }
    }

    if (!WriteBinaryFile(localPath, payload)) {
        LOG_WARN("SteamCM", "保存清单文件失败: {}", localPath.string());
        return std::nullopt;
    }

    LOG_INFO("SteamCM", "成功直接在线下载并保存清单文件: {} ({} 字节)", fileName, payload.size());
    return localPath.string();
}

std::optional<std::string> SteamCmClient::DownloadManifestOnline(
    uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& destDir,
    bool* outAccessDenied, WinHttpTransport* sharedHttp) {

    if (outAccessDenied) *outAccessDenied = false;
    if (appId == 0 || depotId == 0 || !IsValidManifestId(manifestId)) {
        return std::nullopt;
    }

    std::string fileName = std::format("{}_{}.manifest", depotId, manifestId);
    std::filesystem::path localPath = std::filesystem::path(destDir) / fileName;

    std::error_code checkEc;
    if (std::filesystem::is_regular_file(localPath, checkEc) && std::filesystem::file_size(localPath, checkEc) > 0) {
        LOG_INFO("SteamCM", "清单文件已存在于目标目录，直接秒级复用: {}", localPath.string());
        return localPath.string();
    }

    std::string reqCode = FetchManifestRequestCode(appId, depotId, manifestId, outAccessDenied);
    if (reqCode.empty() || reqCode == "0") {
        if (!outAccessDenied || !*outAccessDenied) {
            LOG_WARN("SteamCM", "未能获得 manifest_request_code (Depot={}, Manifest={})", depotId, manifestId);
        }
        return std::nullopt;
    }

    auto cdnServers = GetCdnServers(m_cellId);
    return DownloadManifestPayload(appId, depotId, manifestId, reqCode, destDir, cdnServers, sharedHttp);
}

bool SteamCmClient::DetectDenuvoFromStore(uint32_t appId) {
    if (appId == 0) return false;

    static std::unordered_map<uint32_t, bool> s_denuvoCache;
    static std::mutex s_denuvoMutex;
    {
        std::lock_guard lock(s_denuvoMutex);
        if (auto it = s_denuvoCache.find(appId); it != s_denuvoCache.end()) {
            return it->second;
        }
    }

    WinHttpTransport http;
    HttpResponse resp = http.Get(std::format("https://store.steampowered.com/api/appdetails?appids={}&filters=basic", appId));
    bool isDenuvo = false;
    if (resp.IsSuccess()) {
        auto drmNotice = JsonHelper::GetString(resp.body, "drm_notice");
        if (drmNotice) {
            std::string_view notice = *drmNotice;
            constexpr std::string_view target = "denuvo";
            auto it = std::ranges::search(notice, target, [](char c1, char c2) {
                return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
            });
            if (!it.empty()) {
                isDenuvo = true;
                LOG_INFO("SteamCM", "从 Steam Store 官方元数据检测到 Denuvo 保护 (AppID={}, drm_notice='{}')", appId, *drmNotice);
            }
        }
        std::lock_guard lock(s_denuvoMutex);
        s_denuvoCache[appId] = isDenuvo;
    }

    return isDenuvo;
}

ExtractedAppCredentials SteamCmClient::ExtractFullCredentials(uint32_t appId, bool forceEticket) {
    ExtractedAppCredentials creds;
    creds.appId = appId;

    // 1. AppOwnershipTicket (纯净拉取，自主拥有与家庭共享在无会话状态下原生直接下发)
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
        LOG_INFO("SteamCM", "未能获取所有权票据 (账号未直接拥有或属于共享借用；OpenSteamTool 将在运行时自动执行 AppID 7 伪造兜底)");
    }

    // 2. 纯官方云端向 Steam CM PICS 请求完整产品元数据 (包含全部 Depots、清单号、所有 DLC 列表与 DRM 标记)
    LOG_DEBUG("SteamCM", "正在向 Steam CM 查询 AppID {} 的 64 位 PICS 访问令牌...", appId);
    uint64_t appToken = 0;
    auto tokMap = RequestAppTokens({ appId });
    if (tokMap.contains(appId)) {
        appToken = tokMap[appId];
    }

    LOG_DEBUG("SteamCM", "正在向 Steam CM PICS 请求官方产品元数据 (AppID={}, token={})...", appId, MaskPicsToken(appToken));
    auto appInfoData = RequestPicsProductInfo(appId, appToken);

    // 3. EncryptedAppTicket (精准定向直达: 仅在检测到 Denuvo 保护或显式强制时激活游戏会话)
    bool isDenuvoApp = (appInfoData && appInfoData->requiresDenuvo) || DetectDenuvoFromStore(appId);
    if (appInfoData) {
        appInfoData->requiresDenuvo = isDenuvoApp;
    }
    const bool needsETicket = isDenuvoApp || forceEticket;

    if (needsETicket) {
        LOG_INFO("SteamCM", "检测到游戏需要加密票据 (Denuvo={}, 强制={})，建立定向会话授权...", isDenuvoApp, forceEticket);
        if (!TuiEngine::IsActive()) {
            std::cout << "  -> 正在建立定向会话以提取加密票据 (Denuvo / 专用 DRM)...\n";
        }
        SetGamePlayed(appId);
        struct SessionGuard {
            SteamCmClient* client;
            ~SessionGuard() {
                if (client) client->SetGamePlayed(0);
            }
        } sessionGuard{this};

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        creds.encryptedAppTicket = RequestEncryptedAppTicket(appId);
        if (creds.encryptedAppTicket) {
            LOG_INFO("SteamCM", "定向会话提取到加密票据 ({} 字节)", creds.encryptedAppTicket->size());
            if (!TuiEngine::IsActive()) {
                std::cout << "     [OK] 激活会话提取到加密票据 (" << creds.encryptedAppTicket->size() << " 字节)\n";
            }
        } else {
            LOG_WARN("SteamCM", "定向会话仍未能获取加密票据 (可能账号无权访问该产品)");
        }

        // 极端边界兜底：若前置未拿到所有权票据，在会话中顺带重试一次
        if (!creds.appOwnershipTicket) {
            creds.appOwnershipTicket = RequestAppOwnershipTicket(appId);
            if (creds.appOwnershipTicket) {
                LOG_INFO("SteamCM", "会话激活后成功提取到所有权票据 ({} 字节)", creds.appOwnershipTicket->size());
                if (!TuiEngine::IsActive()) {
                    std::cout << "     [OK] 激活会话提取到所有权票据 (" << creds.appOwnershipTicket->size() << " 字节)\n";
                }
            }
        }
    } else {
        LOG_DEBUG("SteamCM", "普通游戏无需 Denuvo 加密票据 (eticket)，已优雅跳过会话激活");
    }

    std::vector<DepotKeyInfo> depotKeys;
    std::unordered_set<uint32_t> knownDlcSet;
    const std::string outDir = std::to_string(appId);
    size_t downloadedManifests = 0;

    struct ManifestTask {
        uint32_t depotId{0};
        std::string manifestId;
        std::string reqCode;
    };
    std::vector<ManifestTask> downloadTasks;
    std::future<std::vector<std::pair<uint32_t, std::string>>> manifestFuture;

    if (appInfoData) {
        LOG_INFO("SteamCM", "获得官方 AppInfo 数据 (共 {} 个 Depot, {} 个 DLC)",
                 appInfoData->depots.size(), appInfoData->dlcAppIds.size());

        // 4.1 识别所有 DLC（包含无独立 Depot 的逻辑 DLC，例如季票、豪华版升级包、皮肤包、原声音乐等）
        for (uint32_t dlcId : appInfoData->dlcAppIds) {
            if (dlcId != appId && !knownDlcSet.contains(dlcId)) {
                knownDlcSet.insert(dlcId);
                DlcInfo dInfo;
                dInfo.dlcId = dlcId;
                creds.dlcs.push_back(std::move(dInfo));
            }
        }

        // 4.2 提取官方 Depot 与关联清单 ID (GID)
        for (const auto& dInfo : appInfoData->depots) {
            if (dInfo.dlcId > 0 && dInfo.dlcId != appId && !knownDlcSet.contains(dInfo.dlcId)) {
                knownDlcSet.insert(dInfo.dlcId);
                DlcInfo dInfoObj;
                dInfoObj.dlcId = dInfo.dlcId;
                creds.dlcs.push_back(std::move(dInfoObj));
            }

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

        // 4.3 纯官方云端向 Steam CM 查询真实物理 Depot 解密密钥 (先验授权鉴定，杜绝后续清单盲目撞门)
        std::vector<uint32_t> queryDepotList;
        for (const auto& dk : depotKeys) {
            // 严格剔除无独立 Depot 的纯逻辑 DLC 容器 (dlcId == depotId 或属于已知 DLC AppID 且无清单)
            if (dk.manifestId.empty() && (dk.depotId == dk.dlcId || knownDlcSet.contains(dk.depotId))) {
                continue;
            }
            // 严格剔除无任何有效清单的非 base 游戏条目 (未发布/已弃用/仅存在于私有分支的 Depot)
            if (dk.manifestId.empty() && dk.depotId != appId) {
                continue;
            }
            queryDepotList.push_back(dk.depotId);
        }
        if (std::find(queryDepotList.begin(), queryDepotList.end(), appId) == queryDepotList.end()) {
            queryDepotList.push_back(appId);
        }
        std::sort(queryDepotList.begin(), queryDepotList.end());
        queryDepotList.erase(std::unique(queryDepotList.begin(), queryDepotList.end()), queryDepotList.end());

        LOG_DEBUG("SteamCM", "正在向 Steam CM 查询 Depot 解密密钥 (AppID={}, 共 {} 个候选物理 Depot)...", appId, queryDepotList.size());
        if (!TuiEngine::IsActive()) {
            std::cout << "  -> 正在向 Steam CM 查询 Depot 解密密钥与授权...\n";
        }

        std::vector<DepotKeyInfo> cmKeys = RequestDepotKeys(appId, queryDepotList);
        std::unordered_set<uint32_t> authorizedDepots;
        std::unordered_map<uint32_t, std::string> cmKeyMap;

        for (const auto& cmKey : cmKeys) {
            authorizedDepots.insert(cmKey.depotId);
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

        // 4.4 针对已授权物理 Depot 拉取官方清单文件 (免撞门两阶段流水线: CM 请求码收集 + 异步并发 CDN 下载)
        LOG_INFO("SteamCM", "正在云端拉取官方清单文件 (已授权 Depot: {}/{})...", authorizedDepots.size(), queryDepotList.size());
        if (!TuiEngine::IsActive()) {
            std::cout << "  -> 正在纯云端拉取官方清单文件 (.manifest)...\n";
        }

        for (auto& dk : depotKeys) {
            if (IsValidManifestId(dk.manifestId)) {
                // 彻底杜绝撞门：仅对确认已授权的物理 Depot 请求清单
                if (!authorizedDepots.contains(dk.depotId)) {
                    LOG_DEBUG("SteamCM", "Depot {} 未获许可或未授权，优雅跳过清单请求", dk.depotId);
                    continue;
                }

                // 本地磁盘快速检查：若已存在则直接秒级复用，完全跳过网络查询
                std::string fileName = std::format("{}_{}.manifest", dk.depotId, dk.manifestId);
                std::filesystem::path localPath = std::filesystem::path(outDir) / fileName;
                std::error_code checkEc;
                if (std::filesystem::is_regular_file(localPath, checkEc) && std::filesystem::file_size(localPath, checkEc) > 0) {
                    LOG_INFO("SteamCM", "清单文件已存在于目标目录，直接秒级复用: {}", localPath.string());
                    dk.manifestFilePath = localPath.string();
                    ++downloadedManifests;
                    continue;
                }

                bool accessDenied = false;
                std::string reqCode = FetchManifestRequestCode(appId, dk.depotId, dk.manifestId, &accessDenied);
                if (!reqCode.empty() && reqCode != "0") {
                    downloadTasks.push_back({dk.depotId, dk.manifestId, std::move(reqCode)});
                }
            }
        }

        if (!downloadTasks.empty()) {
            auto cdnServers = GetCdnServers(m_cellId);
            manifestFuture = std::async(std::launch::async, [tasks = downloadTasks, appId, outDir, cdnServers]() {
                std::vector<std::pair<uint32_t, std::string>> results(tasks.size());
                std::atomic<size_t> nextIndex{0};
                const size_t numWorkers = std::min<size_t>(tasks.size(), 4);
                std::vector<std::thread> workers;
                workers.reserve(numWorkers);

                for (size_t w = 0; w < numWorkers; ++w) {
                    workers.emplace_back([&]() {
                        WinHttpTransport http;
                        while (true) {
                            size_t idx = nextIndex.fetch_add(1, std::memory_order_relaxed);
                            if (idx >= tasks.size()) break;
                            const auto& t = tasks[idx];
                            auto downloaded = DownloadManifestPayload(
                                appId, t.depotId, t.manifestId, t.reqCode, outDir, cdnServers, &http);
                            if (downloaded) {
                                results[idx] = {t.depotId, std::move(*downloaded)};
                            }
                        }
                    });
                }

                for (auto& worker : workers) {
                    if (worker.joinable()) worker.join();
                }
                return results;
            });
        }

        // 4.5 纯官方云端批量解析所有 DLC 名称 (Steam CM PICS 公开元数据查询 + Store WebAPI 补充兜底)
        std::vector<uint32_t> missingDlcNames;
        for (const auto& d : creds.dlcs) {
            if (d.name.empty()) {
                missingDlcNames.push_back(d.dlcId);
            }
        }
        if (!missingDlcNames.empty()) {
            LOG_DEBUG("SteamCM", "正在向 Steam CM PICS 批量查询 {} 个 DLC 官方名称...", missingDlcNames.size());
            auto resolved = RequestPicsAppNames(missingDlcNames);
            for (auto& d : creds.dlcs) {
                if (d.name.empty()) {
                    auto it = resolved.find(d.dlcId);
                    if (it != resolved.end() && !it->second.empty()) {
                        d.name = it->second;
                    }
                }
            }

            // 官方 Store WebAPI 补充兜底 (针对极少数 CM PICS 未返回公共名称的 DLC)
            WinHttpTransport storeHttp;
            for (auto& d : creds.dlcs) {
                if (d.name.empty()) {
                    std::string storeName = FetchAppNameFromStore(d.dlcId, &storeHttp);
                    if (!storeName.empty()) {
                        d.name = std::move(storeName);
                    }
                }
            }
        }
    }

    // 5. 等待并发 CDN 清单拉取任务收尾并合并路径结果
    if (manifestFuture.valid()) {
        auto asyncResults = manifestFuture.get();
        for (const auto& [depotId, path] : asyncResults) {
            if (!path.empty()) {
                for (auto& dk : depotKeys) {
                    if (dk.depotId == depotId) {
                        dk.manifestFilePath = path;
                        ++downloadedManifests;
                        break;
                    }
                }
            }
        }
    }

    if (downloadedManifests > 0 && !TuiEngine::IsActive()) {
        std::cout << "     [OK] 成功拉取 " << downloadedManifests << " 个官方清单文件\n";
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

    // 6. 纯官方云端查询 64 位 PICS AccessTokens (AppID + 全部 DLC)
    LOG_DEBUG("SteamCM", "正在查询 64 位 PICS AccessToken (AppID={})...", appId);
    if (!TuiEngine::IsActive()) {
        std::cout << "  -> 正在查询 64 位 PICS AccessToken...\n";
    }

    std::unordered_set<uint32_t> targetAppIds;
    targetAppIds.insert(appId);
    for (const auto& dlc : creds.dlcs) {
        targetAppIds.insert(dlc.dlcId);
    }

    std::vector<uint32_t> tokensToQuery(targetAppIds.begin(), targetAppIds.end());
    std::sort(tokensToQuery.begin(), tokensToQuery.end());

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
