#include "OutputWriter.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_set>

namespace OST::ExtractTickets {

std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket) {
    if (!ticket || ticket->empty()) return std::string{name} + ":null\n";
    return std::format("{}({}bytes):{}\n", name, ticket->size(), ToHexString(*ticket));
}

bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys,
                  const std::vector<DlcInfo>& dlcs,
                  const std::unordered_map<uint32_t, uint64_t>& appTokens) {
    const std::string dir{std::to_string(appId)};
    std::filesystem::path dirPath(dir);
    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "Failed to create directory " << dir << " (" << ec.message() << ").\n";
        }
        LOG_ERROR("OutputWriter", "创建输出目录失败: {} ({})", dir, ec.message());
        return false;
    }

    bool ok{true};
    if (ownership && !ownership->empty()) ok = WriteBinaryFile(dirPath / "appticket.bin", *ownership) && ok;
    if (encrypted && !encrypted->empty()) ok = WriteBinaryFile(dirPath / "eticket.bin", *encrypted) && ok;

    // Copy manifest files (.manifest) if found in depotcache
    std::vector<std::string> copiedManifests;
    copiedManifests.reserve(depotKeys.size());
    for (const auto& dk : depotKeys) {
        if (!dk.manifestFilePath.empty()) {
            std::filesystem::path srcPath(dk.manifestFilePath);
            std::string fname = srcPath.filename().string();
            if (std::ranges::find(copiedManifests, fname) == copiedManifests.end()) {
                std::filesystem::path destPath = dirPath / fname;
                std::error_code copyEc;
                if (std::filesystem::equivalent(srcPath, destPath, copyEc)) {
                    copiedManifests.push_back(fname);
                } else if (std::filesystem::copy_file(srcPath, destPath, std::filesystem::copy_options::overwrite_existing, copyEc)) {
                    copiedManifests.push_back(fname);
                } else {
                    if (std::filesystem::exists(destPath, copyEc)) {
                        copiedManifests.push_back(fname);
                    } else {
                        if (!TuiEngine::IsActive()) {
                            std::cerr << "[WARN] Failed to copy manifest " << fname << ": " << copyEc.message() << "\n";
                        }
                        LOG_WARN("OutputWriter", "复制清单文件失败: {} ({})", fname, copyEc.message());
                    }
                }
            }
        }
    }

    uint64_t baseToken = 0;
    auto baseTokIt = appTokens.find(appId);
    if (baseTokIt != appTokens.end()) {
        baseToken = baseTokIt->second;
    }

    std::map<uint32_t, uint64_t> relevantTokens;
    if (baseToken != 0) {
        relevantTokens[appId] = baseToken;
    }
    for (const auto& dlc : dlcs) {
        auto it = appTokens.find(dlc.dlcId);
        if (it != appTokens.end() && it->second != 0) {
            relevantTokens[dlc.dlcId] = it->second;
        }
    }

    // Build tickets.txt summary
    std::string text;
    text.reserve(2048);
    text += "appid:" + std::to_string(appId) + "\n";
    for (const auto& dlc : dlcs) {
        text += "dlc(" + std::to_string(dlc.dlcId) + ")";
        if (!dlc.name.empty()) {
            text += ":" + SanitizeComment(dlc.name);
        }
        text += "\n";
    }
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) {
            text += "depotkey(" + std::to_string(dk.depotId) + "):" + dk.hexKey + "\n";
        }
    }
    for (const auto& dk : depotKeys) {
        if (IsValidManifestId(dk.manifestId)) {
            text += "manifest(" + std::to_string(dk.depotId) + "):" + dk.manifestId + "\n";
        }
    }
    if (baseToken != 0) {
        text += "token(" + std::to_string(appId) + "):" + std::to_string(baseToken) + "\n";
    } else {
        text += "token(" + std::to_string(appId) + "):null\n";
    }
    for (const auto& [tId, tVal] : relevantTokens) {
        if (tId != appId) {
            text += "token(" + std::to_string(tId) + "):" + std::to_string(tVal) + "\n";
        }
    }
    text += TicketLine("appticket", ownership);
    text += TicketLine("eticket", encrypted);

    const std::filesystem::path textPath = dirPath / "tickets.txt";
    std::ofstream summary{textPath, std::ios::trunc};
    if (!summary || !(summary << text)) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "Failed to write " << textPath.string() << ".\n";
        }
        LOG_ERROR("OutputWriter", "写入 tickets.txt 失败: {}", textPath.string());
        return false;
    }

    // Generate ready-to-use Lua script
    std::string luaText;
    luaText.reserve(8192);
    luaText += "-- Auto-generated by extract_tickets for AppID: " + std::to_string(appId) + "\n\n";

    std::unordered_set<uint32_t> ownedDlcIdSet;
    for (const auto& dlc : dlcs) {
        ownedDlcIdSet.insert(dlc.dlcId);
    }

    // 1. Base Game Unlocks (AppID and base game depots)
    luaText += "-- Base Game\n";
    const DepotKeyInfo* baseAppDk = nullptr;
    for (const auto& dk : depotKeys) {
        if (dk.depotId == appId) {
            baseAppDk = &dk;
            break;
        }
    }

    if (baseAppDk && !baseAppDk->hexKey.empty()) {
        luaText += "addappid(" + std::to_string(appId) + ", 1, \"" + baseAppDk->hexKey + "\")\n";
    } else {
        luaText += "addappid(" + std::to_string(appId) + ")\n";
    }

    if (baseToken != 0) {
        luaText += "addtoken(" + std::to_string(appId) + ", \"" + std::to_string(baseToken) + "\")\n";
    }

    for (const auto& dk : depotKeys) {
        if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
            if (!dk.hexKey.empty()) {
                luaText += "addappid(" + std::to_string(dk.depotId) + ", 1, \"" + dk.hexKey + "\")\n";
            }
        }
    }

    // 2. Base Game Manifests (placed right under Base Game)
    bool hasBaseManifests = false;
    if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
        hasBaseManifests = true;
    }
    if (!hasBaseManifests) {
        for (const auto& dk : depotKeys) {
            if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
                if (IsValidManifestId(dk.manifestId)) {
                    hasBaseManifests = true;
                    break;
                }
            }
        }
    }

    auto hasManifestOnDisk = [&](uint32_t depotId, const std::string& manifestId) -> bool {
        std::string fname = std::format("{}_{}.manifest", depotId, manifestId);
        std::filesystem::path p = dirPath / fname;
        std::error_code diskEc;
        return std::filesystem::exists(p, diskEc);
    };

    if (hasBaseManifests) {
        luaText += "\n-- Base Game Manifests\n";
        if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
            if (hasManifestOnDisk(appId, baseAppDk->manifestId)) {
                luaText += "setManifestid(" + std::to_string(appId) + ", \"" + baseAppDk->manifestId + "\")\n";
            } else {
                luaText += "-- setManifestid(" + std::to_string(appId) + ", \"" + baseAppDk->manifestId + "\") -- [未下载到实体清单文件]\n";
            }
        }
        for (const auto& dk : depotKeys) {
            if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
                if (IsValidManifestId(dk.manifestId)) {
                    if (hasManifestOnDisk(dk.depotId, dk.manifestId)) {
                        luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")\n";
                    } else {
                        luaText += "-- setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\") -- [未下载到实体清单文件]\n";
                    }
                }
            }
        }
    }

    // 3. Owned DLCs Unlocks
    if (!dlcs.empty()) {
        luaText += "\n-- Owned DLCs\n";
        for (const auto& dlc : dlcs) {
            const DepotKeyInfo* dlcDk = nullptr;
            for (const auto& dk : depotKeys) {
                if (dk.depotId == dlc.dlcId) {
                    dlcDk = &dk;
                    break;
                }
            }

            if (dlcDk && !dlcDk->hexKey.empty()) {
                luaText += "addappid(" + std::to_string(dlc.dlcId) + ", 1, \"" + dlcDk->hexKey + "\")";
            } else {
                luaText += "addappid(" + std::to_string(dlc.dlcId) + ")";
            }

            if (!dlc.name.empty()) {
                luaText += " -- " + SanitizeComment(dlc.name);
            }
            luaText += "\n";

            auto dlcTokIt = appTokens.find(dlc.dlcId);
            if (dlcTokIt != appTokens.end() && dlcTokIt->second != 0) {
                luaText += "addtoken(" + std::to_string(dlc.dlcId) + ", \"" + std::to_string(dlcTokIt->second) + "\")\n";
            }

            // Any subdepots of this DLC with keys
            for (const auto& dk : depotKeys) {
                if (dk.dlcId == dlc.dlcId && dk.depotId != dlc.dlcId) {
                    if (!dk.hexKey.empty()) {
                        luaText += "addappid(" + std::to_string(dk.depotId) + ", 1, \"" + dk.hexKey + "\")\n";
                    }
                }
            }
        }

        // 4. DLC Manifests (placed right under DLC unlocks)
        bool hasDlcManifests = false;
        for (const auto& dlc : dlcs) {
            for (const auto& dk : depotKeys) {
                if ((dk.depotId == dlc.dlcId || dk.dlcId == dlc.dlcId) && IsValidManifestId(dk.manifestId)) {
                    hasDlcManifests = true;
                    break;
                }
            }
            if (hasDlcManifests) break;
        }

        if (hasDlcManifests) {
            luaText += "\n-- DLC Manifests\n";
            for (const auto& dlc : dlcs) {
                // DLC itself manifest
                for (const auto& dk : depotKeys) {
                    if (dk.depotId == dlc.dlcId && IsValidManifestId(dk.manifestId)) {
                        if (hasManifestOnDisk(dk.depotId, dk.manifestId)) {
                            luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")";
                        } else {
                            luaText += "-- setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\") -- [未下载到实体清单文件]";
                        }
                        if (!dlc.name.empty()) {
                            luaText += " -- " + SanitizeComment(dlc.name);
                        }
                        luaText += "\n";
                        break;
                    }
                }
                // Any subdepots of this DLC with manifests
                for (const auto& dk : depotKeys) {
                    if (dk.dlcId == dlc.dlcId && dk.depotId != dlc.dlcId && IsValidManifestId(dk.manifestId)) {
                        if (hasManifestOnDisk(dk.depotId, dk.manifestId)) {
                            luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")\n";
                        } else {
                            luaText += "-- setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\") -- [未下载到实体清单文件]\n";
                        }
                    }
                }
            }
        }
    }

    // 5. App Tickets (at the very end of the file)
    luaText += "\n";
    const bool hasOwnership = (ownership && !ownership->empty());
    const bool hasEncrypted = (encrypted && !encrypted->empty());

    luaText += "-- App Ownership Ticket (AppTicket)\n";
    if (hasOwnership) {
        luaText += std::format("setAppTicket({}, \"{}\")\n\n",
                               appId,
                               ToHexString(*ownership));
    } else {
        luaText += std::format("-- setAppTicket({}, \"null\") -- [未检测到有效所有权票据; OpenSteamTool 将自动启用 AppID 7 伪造兜底]\n\n", appId);
    }

    luaText += "-- Encrypted App Ticket (ETicket)\n";
    if (hasEncrypted) {
        luaText += std::format("setETicket({}, \"{}\")\n", appId, ToHexString(*encrypted));
    } else {
        luaText += std::format("-- setETicket({}, \"null\") -- [仅 Denuvo 强加密游戏需要，普通游戏无需此项]\n", appId);
    }

    const std::filesystem::path luaPath = dirPath / (std::to_string(appId) + ".lua");
    std::ofstream luaFile{luaPath, std::ios::trunc};
    if (!luaFile || !(luaFile << luaText)) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "Failed to write " << luaPath.string() << ".\n";
        }
        LOG_ERROR("OutputWriter", "写入 Lua 脚本失败: {}", luaPath.string());
        ok = false;
    }

    if (!TuiEngine::IsActive()) {
        std::cout << "Wrote " << dir << "\\ (" << std::to_string(appId) << ".lua, tickets.txt";
        if (ownership && !ownership->empty()) std::cout << ", appticket.bin";
        if (encrypted && !encrypted->empty()) std::cout << ", eticket.bin";
        for (const auto& mName : copiedManifests) {
            std::cout << ", " << mName;
        }
        std::cout << ")\n";

        if (!dlcs.empty()) {
            std::cout << "[INFO] 已提取 " << dlcs.size() << " 个拥有的 DLC / Extracted " << dlcs.size() << " owned DLC(s):\n";
            for (const auto& dlc : dlcs) {
                std::cout << "       DLC " << dlc.dlcId;
                if (!dlc.name.empty()) {
                    std::cout << ": " << dlc.name;
                }
                std::cout << "\n";
            }
        } else {
            std::cout << "[INFO] 未检测到该游戏拥有的 DLC / No owned DLCs found for AppID " << appId << ".\n";
        }

        const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
            return !dk.hexKey.empty();
        });
        if (keyCount > 0) {
            std::cout << "[INFO] 已提取 " << keyCount << " 个 Depot 解密密钥 / Extracted " << keyCount << " depot decryption key(s):\n";
            for (const auto& dk : depotKeys) {
                if (!dk.hexKey.empty()) {
                    std::cout << "       Depot " << dk.depotId << ": " << dk.hexKey << "\n";
                }
            }
        } else {
            std::cout << "[INFO] 未在 config.vdf 中找到缓存的 Depot 解密密钥 / No cached depot decryption keys found in config.vdf for AppID " << appId << ".\n";
            std::cout << "[TIP] 若该游戏需要 Depot 密钥，请在 Steam 中启动一次安装/更新以生成缓存，然后重新运行提取工具。\n"
                      << "      If this game requires depot keys, start installing/updating it once in Steam to cache them, then run extract_tickets again.\n";
        }

        if (!copiedManifests.empty()) {
            std::cout << "[INFO] 已提取 " << copiedManifests.size() << " 个清单文件 (.manifest) / Extracted " << copiedManifests.size() << " depot manifest file(s) (.manifest):\n";
            for (const auto& mName : copiedManifests) {
                std::cout << "       " << mName << "\n";
            }
        } else {
            std::cout << "[INFO] 未在 depotcache 中找到缓存的清单文件 (.manifest) / No cached .manifest files found in depotcache for AppID " << appId << ".\n";
        }

        if (!relevantTokens.empty()) {
            std::cout << "[INFO] 已提取 " << relevantTokens.size() << " 个访问令牌 (AccessToken) / Extracted "
                      << relevantTokens.size() << " access token(s):\n";
            for (const auto& [tId, tVal] : relevantTokens) {
                std::cout << "       AppID " << tId << ": " << tVal << "\n";
            }
        } else {
            std::cout << "[INFO] 未在 appinfo.vdf 中找到非零访问令牌 (该游戏可能无需 Access Token) / "
                      << "No non-zero access token found in appinfo.vdf for AppID " << appId
                      << " (this game may not require an access token).\n";
        }

        std::cout << "[INFO] 配置文件已生成 / Ready-to-use Lua script saved to: " << luaPath.string() << "\n";
    }

    const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
        return !dk.hexKey.empty();
    });
    LOG_INFO("OutputWriter", "输出文件已写入: {}/ (DLCs={}, DepotKeys={}, Manifests={}, Tokens={})",
             dir, dlcs.size(), keyCount, copiedManifests.size(), relevantTokens.size());
    LOG_INFO("OutputWriter", "Lua 配置文件已保存至: {}", luaPath.string());
    return ok;
}

} // namespace OST::ExtractTickets
