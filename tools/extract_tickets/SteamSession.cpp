#include "SteamSession.h"
#include "Log.h"
#include "LuaFallbackParser.h"
#include "RaiiGuards.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace OST::ExtractTickets {

HMODULE LoadSteamClient64(const std::string& steamPath, std::string& loadedPath) {
    if (steamPath.empty()) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] 未在注册表中找到 Steam 安装路径 / Failed to find Steam install path in registry.\n";
        }
        LOG_WARN("SteamSession", "未在注册表中找到 Steam 安装路径");
        return nullptr;
    }

    const std::string steamDir{NormalizeDir(steamPath)};
    loadedPath = JoinPath(steamPath, "steamclient64.dll");

    // steamclient64.dll pulls in tier0_s64.dll / vstdlib_s64.dll from the Steam
    // directory. Add that directory to the search path and load with
    // LOAD_WITH_ALTERED_SEARCH_PATH so those dependencies resolve; otherwise the
    // load fails with ERROR_MOD_NOT_FOUND (126).
    SetDllDirectoryA(steamDir.c_str());
    HMODULE module{LoadLibraryExA(loadedPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)};
    if (!module) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] 加载 steamclient64.dll 失败 / Failed to load " << loadedPath << " (GetLastError=" << GetLastError() << ").\n";
        }
        LOG_WARN("SteamSession", "加载 steamclient64.dll 失败: {} (GetLastError={})", loadedPath, GetLastError());
        return nullptr;
    }

    return module;
}

ISteamClient* CreateSteamClient(HMODULE module) {
    if (!module) return nullptr;
    auto createInterface{reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"))};
    if (!createInterface) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] steamclient64.dll 缺少 CreateInterface 导出 / steamclient64.dll has no CreateInterface export.\n";
        }
        LOG_WARN("SteamSession", "steamclient64.dll 缺少 CreateInterface 导出");
        return nullptr;
    }

    int returnCode{0};
    auto* client{reinterpret_cast<ISteamClient*>(createInterface(kSteamClientInterfaceVersion, &returnCode))};
    if (!client) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] CreateInterface(" << kSteamClientInterfaceVersion
                      << ") 失败 / failed (returnCode=" << returnCode << ").\n";
        }
        LOG_WARN("SteamSession", "CreateInterface({}) 失败 (returnCode={})", kSteamClientInterfaceVersion, returnCode);
        return nullptr;
    }
    return client;
}

bool OpenSession(ISteamClient* client, HSteamPipe& pipe, HSteamUser& user) {
    if (!client) return false;
    pipe = client->CreateSteamPipe();
    if (!pipe) {
        return false;
    }

    user = client->ConnectToGlobalUser(pipe);
    if (!user) {
        client->BReleaseSteamPipe(pipe);
        pipe = 0;
        return false;
    }

    return true;
}

std::optional<std::vector<uint8_t>> ExtractAppOwnershipTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId) {
    auto* appTicket{reinterpret_cast<ISteamAppTicket*>(
        client->GetISteamGenericInterface(user, pipe, kSteamAppTicketInterfaceVersion))};
    if (!appTicket) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] GetISteamGenericInterface(" << kSteamAppTicketInterfaceVersion
                      << ") 返回空 / returned null.\n";
        }
        LOG_WARN("SteamSession", "GetISteamGenericInterface({}) 返回空", kSteamAppTicketInterfaceVersion);
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(2048);
    uint32_t appIdOffset{0};
    uint32_t steamIdOffset{0};
    uint32_t signatureOffset{0};
    uint32_t signatureSize{0};
    uint32_t written{appTicket->GetAppOwnershipTicketData(
        appId,
        buffer.data(),
        static_cast<uint32_t>(buffer.size()),
        &appIdOffset,
        &steamIdOffset,
        &signatureOffset,
        &signatureSize)};

    if (written > buffer.size()) {
        buffer.resize(written);
        const uint32_t written2{appTicket->GetAppOwnershipTicketData(
            appId,
            buffer.data(),
            static_cast<uint32_t>(buffer.size()),
            &appIdOffset,
            &steamIdOffset,
            &signatureOffset,
            &signatureSize)};
        if (written2 == 0 || written2 > buffer.size()) {
            if (!TuiEngine::IsActive()) {
                std::cerr << "[INFO] 未能获取 AppID " << appId << " 的所有权票据 (账号可能未拥有或本地未缓存) / "
                          << "GetAppOwnershipTicketData returned no ticket for AppID " << appId
                          << " (account may not own the app or not cached locally).\n";
            }
            LOG_INFO("SteamSession", "未能获取 AppID {} 的所有权票据", appId);
            return std::nullopt;
        }
        written = written2;
    } else if (written == 0) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[INFO] 未能获取 AppID " << appId << " 的所有权票据 (账号可能未拥有或本地未缓存) / "
                      << "GetAppOwnershipTicketData returned no ticket for AppID " << appId
                      << " (account may not own the app or not cached locally).\n";
        }
        LOG_INFO("SteamSession", "未能获取 AppID {} 的所有权票据", appId);
        return std::nullopt;
    }

    buffer.resize(written);
    if (!TuiEngine::IsActive()) {
        std::cout << "Ownership ticket " << written << " bytes"
                  << " (appIdOffset=" << appIdOffset
                  << " steamIdOffset=" << steamIdOffset
                  << " signatureOffset=" << signatureOffset
                  << " signatureSize=" << signatureSize << ")\n";
    }
    LOG_INFO("SteamSession", "Ownership ticket: {} bytes", written);
    return buffer;
}

std::optional<std::vector<uint8_t>> ExtractEncryptedAppTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId) {
    auto* utils{client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)};
    auto* steamUser{client->GetISteamUser(user, pipe, kSteamUserInterfaceVersion)};
    if (!utils || !steamUser) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] GetISteamUtils/GetISteamUser 返回空 / returned null.\n";
        }
        LOG_WARN("SteamSession", "GetISteamUtils/GetISteamUser 返回空");
        return std::nullopt;
    }

    const SteamAPICall_t hCall{steamUser->RequestEncryptedAppTicket(nullptr, 0)};
    if (!hCall) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] 请求 EncryptedAppTicket 启动失败 / RequestEncryptedAppTicket failed to start for AppID " << appId << ".\n";
        }
        LOG_WARN("SteamSession", "请求 EncryptedAppTicket 启动失败 (AppID={})", appId);
        return std::nullopt;
    }

    // Bounded poll so a wedged client can never hang the tool.
    constexpr int kMaxWaitMs{15000};
    constexpr int kStepMs{50};
    bool failed{false};
    int waited{0};
    while (!utils->IsAPICallCompleted(hCall, &failed)) {
        if (waited >= kMaxWaitMs) {
            if (!TuiEngine::IsActive()) {
                std::cerr << "[WARN] 等待 EncryptedAppTicket 超时 / Timed out waiting for EncryptedAppTicketResponse_t.\n";
            }
            LOG_WARN("SteamSession", "等待 EncryptedAppTicket 超时");
            return std::nullopt;
        }
        Sleep(kStepMs);
        waited += kStepMs;
    }

    EncryptedAppTicketResponse_t response{};
    const bool gotResult{utils->GetAPICallResult(
        hCall,
        &response,
        sizeof(response),
        EncryptedAppTicketResponse_t::k_iCallback,
        &failed)};
    if (!gotResult || failed) {
        int failureReason = utils->GetAPICallFailureReason(hCall);
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] 获取 EncryptedAppTicket 结果失败 / GetAPICallResult failed for EncryptedAppTicketResponse_t (failureReason="
                      << failureReason << ").\n";
        }
        LOG_WARN("SteamSession", "获取 EncryptedAppTicket 结果失败 (failureReason={})", failureReason);
        return std::nullopt;
    }
    if (response.m_eResult != k_EResultOK) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[INFO] 请求 EncryptedAppTicket 返回状态码 / RequestEncryptedAppTicket returned EResult "
                      << static_cast<int>(response.m_eResult);
            if (response.m_eResult == k_EResultAccessDenied) {
                std::cerr << " (AccessDenied: 当前登录账号未拥有该游戏或无权获取其凭据 / Account does not own this app or lacks permission)";
            }
            std::cerr << ".\n";
        }
        LOG_INFO("SteamSession", "请求 EncryptedAppTicket 返回状态码 EResult {} (游戏未配置加密票据密钥或未授权，通常单机游戏无需此票据)", static_cast<int>(response.m_eResult));
        return std::nullopt;
    }

    // Pass a null buffer first to learn the size, then fetch.
    uint32_t cbTicket{0};
    steamUser->GetEncryptedAppTicket(nullptr, 0, &cbTicket);
    if (cbTicket == 0) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] 加密票据为空 / Encrypted app ticket is empty.\n";
        }
        LOG_WARN("SteamSession", "加密票据为空");
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(cbTicket);
    if (!steamUser->GetEncryptedAppTicket(buffer.data(), static_cast<int>(buffer.size()), &cbTicket)) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "[WARN] 获取 EncryptedAppTicket 数据失败 / GetEncryptedAppTicket failed.\n";
        }
        LOG_WARN("SteamSession", "获取 EncryptedAppTicket 数据失败");
        return std::nullopt;
    }

    buffer.resize(cbTicket);
    if (!TuiEngine::IsActive()) {
        std::cout << "Encrypted ticket " << cbTicket << " bytes\n";
    }
    LOG_INFO("SteamSession", "Encrypted ticket: {} bytes", cbTicket);
    return buffer;
}

std::vector<DepotKeyInfo> ExtractDepotDecryptionKeys(
    const std::string& steamPath,
    uint32_t appId,
    ISteamClient* client,
    HSteamPipe pipe,
    HSteamUser user,
    std::vector<DlcInfo>& outDlcs) {

    std::unordered_map<uint32_t, std::string> knownDepotManifests;
    std::unordered_set<uint32_t> knownDlcIds;
    std::unordered_map<uint32_t, uint32_t> depotToDlc;
    std::map<uint32_t, DlcInfo> dlcMap;

    knownDepotManifests[appId] = "";
    depotToDlc[appId] = 0;

    if (client && pipe && user) {
        auto* apps = reinterpret_cast<ISteamApps*>(
            client->GetISteamGenericInterface(user, pipe, kSteamAppsInterfaceVersion));
        auto* steamUser = client->GetISteamUser(user, pipe, kSteamUserInterfaceVersion);
        uint64 steamId{0};
        if (steamUser) {
            steamUser->GetSteamID(&steamId);
        }

        if (apps) {
            DepotId_t depots[128]{};
            uint32_t count = apps->GetInstalledDepots(appId, depots, static_cast<uint32_t>(std::size(depots)));
            uint32_t safeCount = std::min(count, static_cast<uint32_t>(std::size(depots)));
            for (uint32_t i = 0; i < safeCount; ++i) {
                if (depots[i] != 0) {
                    knownDepotManifests.try_emplace(depots[i], "");
                    depotToDlc.try_emplace(depots[i], 0);
                }
            }

            int dlcCount = apps->GetDLCCount();
            for (int i = 0; i < dlcCount; ++i) {
                AppId_t dlcId{0};
                bool available{false};
                char dlcName[256]{};
                if (apps->BGetDLCDataByIndex(i, &dlcId, &available, dlcName, static_cast<int>(sizeof(dlcName))) && dlcId != 0) {
                    bool isSubscribed = apps->BIsSubscribedApp(dlcId);
                    bool isInstalled = apps->BIsDlcInstalled(dlcId);
                    bool hasLicense = (steamUser && steamId != 0) ? (steamUser->UserHasLicenseForApp(steamId, dlcId) == 0) : false;
                    bool owned = isSubscribed || hasLicense || isInstalled;

                    if (owned && dlcId != appId) {
                        DlcInfo& d = dlcMap[dlcId];
                        d.dlcId = dlcId;
                        if (d.name.empty() && dlcName[0] != '\0') {
                            d.name = dlcName;
                        }
                        knownDlcIds.insert(dlcId);
                        depotToDlc[dlcId] = dlcId;

                        // Ensure DLC itself is checked for depot manifests
                        knownDepotManifests.try_emplace(dlcId, "");

                        // Also query any installed depots for this DLC
                        DepotId_t dlcDepots[64]{};
                        uint32_t dlcDepotCount = apps->GetInstalledDepots(dlcId, dlcDepots, static_cast<uint32_t>(std::size(dlcDepots)));
                        uint32_t safeDlcCount = std::min(dlcDepotCount, static_cast<uint32_t>(std::size(dlcDepots)));
                        for (uint32_t j = 0; j < safeDlcCount; ++j) {
                            if (dlcDepots[j] != 0) {
                                knownDepotManifests.try_emplace(dlcDepots[j], "");
                                depotToDlc[dlcDepots[j]] = dlcId;
                            }
                        }
                    }
                }
            }
        }
    }

    std::vector<std::string> libraries;
    if (!steamPath.empty()) {
        libraries = FindSteamLibraryFolders(steamPath);
        for (const auto& lib : libraries) {
            std::string acf = JoinPath(lib, "steamapps\\appmanifest_" + std::to_string(appId) + ".acf");
            ParseAcfDepots(acf, knownDepotManifests, knownDlcIds, depotToDlc);
        }

        std::vector<uint32_t> dlcQueue(knownDlcIds.begin(), knownDlcIds.end());
        std::unordered_set<uint32_t> visitedDlcs;
        while (!dlcQueue.empty()) {
            uint32_t dlcId = dlcQueue.back();
            dlcQueue.pop_back();
            if (!visitedDlcs.insert(dlcId).second) continue;

            for (const auto& lib : libraries) {
                std::string acf = JoinPath(lib, "steamapps\\appmanifest_" + std::to_string(dlcId) + ".acf");
                std::unordered_set<uint32_t> newlyFoundDlcs;
                ParseAcfDepots(acf, knownDepotManifests, newlyFoundDlcs, depotToDlc);
                for (uint32_t newDlc : newlyFoundDlcs) {
                    if (knownDlcIds.insert(newDlc).second) {
                        dlcQueue.push_back(newDlc);
                    }
                }
            }
        }
    }

    // Any DLC found via ACF installed depots is confirmed owned
    for (uint32_t dlcId : knownDlcIds) {
        if (dlcId != appId) {
            DlcInfo& d = dlcMap[dlcId];
            d.dlcId = dlcId;
            knownDepotManifests.try_emplace(dlcId, "");
        }
    }

    outDlcs.clear();
    outDlcs.reserve(dlcMap.size());
    for (const auto& [id, info] : dlcMap) {
        outDlcs.push_back(info);
    }

    auto allDepotKeys = !steamPath.empty() ? ParseConfigVdfDepotKeys(steamPath) : std::unordered_map<uint32_t, std::string>{};

    const LuaFallbackData luaFallback = ParseLuaFallbackData(steamPath, appId);
    if (!luaFallback.Empty()) {
        for (uint32_t dlcId : luaFallback.dlcIds) {
            if (dlcId != appId && !knownDlcIds.contains(dlcId)) {
                knownDlcIds.insert(dlcId);
                depotToDlc.try_emplace(dlcId, dlcId);
                DlcInfo& d = dlcMap[dlcId];
                d.dlcId = dlcId;
                auto itN = luaFallback.dlcNames.find(dlcId);
                if (itN != luaFallback.dlcNames.end() && !itN->second.empty()) {
                    d.name = itN->second;
                }
                knownDepotManifests.try_emplace(dlcId, "");
            }
        }

        for (const auto& [dId, dlcId] : luaFallback.depotToDlc) {
            depotToDlc.try_emplace(dId, dlcId);
        }

        for (const auto& [dId, man] : luaFallback.depotManifests) {
            if (!knownDepotManifests.contains(dId) || knownDepotManifests[dId].empty()) {
                knownDepotManifests[dId] = man;
            }
        }

        for (const auto& [dId, key] : luaFallback.depotKeys) {
            if (!allDepotKeys.contains(dId) || allDepotKeys[dId].empty()) {
                allDepotKeys[dId] = key;
            }
        }

        outDlcs.clear();
        outDlcs.reserve(dlcMap.size());
        for (const auto& [id, info] : dlcMap) {
            outDlcs.push_back(info);
        }
    }

    auto getDlcIdForDepot = [&](uint32_t dId) -> uint32_t {
        auto it = depotToDlc.find(dId);
        if (it != depotToDlc.end()) return it->second;
        for (uint32_t dlcId : knownDlcIds) {
            if (dId == dlcId || (dlcId > 0 && dId >= dlcId && dId <= dlcId + 20)) {
                return dlcId;
            }
        }
        return 0; // base game
    };

    std::vector<DepotKeyInfo> result;
    std::unordered_set<uint32_t> addedDepots;

    for (const auto& [dId, manifest] : knownDepotManifests) {
        auto it = allDepotKeys.find(dId);
        if (it != allDepotKeys.end() && !it->second.empty()) {
            result.push_back({dId, it->second, manifest, "", getDlcIdForDepot(dId)});
            addedDepots.insert(dId);
        }
    }

    for (uint32_t dlcId : knownDlcIds) {
        if (!addedDepots.contains(dlcId)) {
            auto it = allDepotKeys.find(dlcId);
            if (it != allDepotKeys.end() && !it->second.empty()) {
                result.push_back({dlcId, it->second, "", "", dlcId});
                addedDepots.insert(dlcId);
            }
        }
    }

    for (const auto& [dId, key] : allDepotKeys) {
        if (!addedDepots.contains(dId)) {
            bool inRange = (dId >= appId && dId <= appId + 50);
            uint32_t matchedDlcId = 0;
            if (!inRange) {
                for (uint32_t dlcId : knownDlcIds) {
                    if (dlcId > 0 && dId >= dlcId && dId <= dlcId + 20) {
                        inRange = true;
                        matchedDlcId = dlcId;
                        break;
                    }
                }
            }
            if (inRange) {
                std::string manifest;
                auto it = knownDepotManifests.find(dId);
                if (it != knownDepotManifests.end()) manifest = it->second;
                result.push_back({dId, key, manifest, "", matchedDlcId ? matchedDlcId : getDlcIdForDepot(dId)});
                addedDepots.insert(dId);
            }
        }
    }

    // Also check known depots that might not have keys in config.vdf,
    // so any cached manifest files can still be discovered and extracted.
    for (const auto& [dId, manifest] : knownDepotManifests) {
        if (!addedDepots.contains(dId)) {
            result.push_back({dId, "", manifest, "", getDlcIdForDepot(dId)});
            addedDepots.insert(dId);
        }
    }

    // Search for cached .manifest files across all depotcache directories
    if (!steamPath.empty()) {
        auto depotcacheDirs = GetDepotcacheDirs(steamPath, libraries);
        for (auto& dk : result) {
            dk.manifestFilePath = FindDepotManifestFile(depotcacheDirs, dk.depotId, dk.manifestId);
        }
    }

    if (!luaFallback.Empty()) {
        for (auto& dk : result) {
            if (dk.manifestFilePath.empty()) {
                auto itMf = luaFallback.manifestFiles.find(dk.depotId);
                if (itMf != luaFallback.manifestFiles.end()) {
                    dk.manifestFilePath = itMf->second;
                }
            }
        }
    }

    // Remove entries that have no key, no manifest file, and no valid manifest ID
    std::erase_if(result, [](const DepotKeyInfo& dk) {
        return dk.hexKey.empty() && dk.manifestFilePath.empty() && !IsValidManifestId(dk.manifestId);
    });

    std::sort(result.begin(), result.end(), [](const DepotKeyInfo& a, const DepotKeyInfo& b) {
        return a.depotId < b.depotId;
    });

    return result;
}

bool ExtractTicketsFromLocalClient(
    uint32_t appId,
    std::optional<std::vector<uint8_t>>& outOwnership,
    std::optional<std::vector<uint8_t>>& outEncrypted) {
    auto steamPathOpt = FindSteamInstallPath();
    if (!steamPathOpt || steamPathOpt->empty()) return false;

    std::string appIdStr = std::to_string(appId);
    SetEnvironmentVariableA("SteamAppId", appIdStr.c_str());
    SetEnvironmentVariableA("OST_TOOL_EXTRACTION", "1");

    std::string steamClientPath;
    HMODULE hClient = LoadSteamClient64(*steamPathOpt, steamClientPath);
    if (!hClient) {
        SetEnvironmentVariableA("SteamAppId", nullptr);
        SetEnvironmentVariableA("OST_TOOL_EXTRACTION", nullptr);
        return false;
    }

    SteamSessionGuard guard{nullptr, 0, 0, hClient};

    ISteamClient* client = CreateSteamClient(hClient);
    if (!client) {
        return false;
    }
    guard.client = client;

    HSteamPipe pipe{0};
    HSteamUser user{0};
    bool ok = false;
    if (OpenSession(client, pipe, user)) {
        guard.pipe = pipe;
        guard.user = user;
        if (!outOwnership || outOwnership->empty()) {
            outOwnership = ExtractAppOwnershipTicket(client, pipe, user, appId);
            if (outOwnership && !outOwnership->empty()) ok = true;
        }
        if (!outEncrypted || outEncrypted->empty()) {
            outEncrypted = ExtractEncryptedAppTicket(client, pipe, user, appId);
            if (outEncrypted && !outEncrypted->empty()) ok = true;
        }
    }
    return ok;
}

} // namespace OST::ExtractTickets

