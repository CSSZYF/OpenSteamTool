#include "OutputWriter.h"
#include "I18n.h"
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

namespace {
std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket) {
    if (!ticket || ticket->empty()) return std::format("{}:null\n", name);
    return std::format("{}({}bytes):{}\n", name, ticket->size(), ToHexString(*ticket));
}
} // namespace

bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys,
                  const std::vector<DlcInfo>& dlcs,
                  const std::unordered_map<uint32_t, uint64_t>& appTokens) {
    const std::string dir{std::to_string(appId)};
    std::filesystem::path dirPath = Utf8Path(dir);
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
            std::filesystem::path srcPath = Utf8Path(dk.manifestFilePath);
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
    std::format_to(std::back_inserter(text), "appid:{}\n", appId);
    for (const auto& dlc : dlcs) {
        if (!dlc.name.empty()) {
            std::format_to(std::back_inserter(text), "dlc({}):{}\n", dlc.dlcId, SanitizeComment(dlc.name));
        } else {
            std::format_to(std::back_inserter(text), "dlc({})\n", dlc.dlcId);
        }
    }
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) {
            std::format_to(std::back_inserter(text), "depotkey({}):{}\n", dk.depotId, dk.hexKey);
        }
    }
    for (const auto& dk : depotKeys) {
        if (IsValidManifestId(dk.manifestId)) {
            std::format_to(std::back_inserter(text), "manifest({}):{}\n", dk.depotId, dk.manifestId);
        }
    }
    if (baseToken != 0) {
        std::format_to(std::back_inserter(text), "token({}):{}\n", appId, baseToken);
    } else {
        std::format_to(std::back_inserter(text), "token({}):null\n", appId);
    }
    for (const auto& [tId, tVal] : relevantTokens) {
        if (tId != appId) {
            std::format_to(std::back_inserter(text), "token({}):{}\n", tId, tVal);
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
    std::format_to(std::back_inserter(luaText), "{}{}\n\n", TR(MsgKey::LuaHeader), appId);

    std::unordered_set<uint32_t> ownedDlcIdSet;
    for (const auto& dlc : dlcs) {
        ownedDlcIdSet.insert(dlc.dlcId);
    }

    const std::unordered_set<std::string> copiedManifestSet(copiedManifests.begin(), copiedManifests.end());
    auto hasManifestOnDisk = [&](uint32_t depotId, const std::string& manifestId) -> bool {
        std::string fname = std::format("{}_{}.manifest", depotId, manifestId);
        if (copiedManifestSet.contains(fname)) return true;
        std::filesystem::path p = dirPath / fname;
        std::error_code diskEc;
        return std::filesystem::exists(p, diskEc);
    };

    // 1. Base Game Unlocks (AppID and base game depots)
    std::format_to(std::back_inserter(luaText), "{}\n", TR(MsgKey::LuaBaseGame));
    const DepotKeyInfo* baseAppDk = nullptr;
    for (const auto& dk : depotKeys) {
        if (dk.depotId == appId) {
            baseAppDk = &dk;
            break;
        }
    }

    if (baseAppDk && !baseAppDk->hexKey.empty()) {
        std::format_to(std::back_inserter(luaText), "addappid({}, 1, \"{}\")\n", appId, baseAppDk->hexKey);
    } else {
        std::format_to(std::back_inserter(luaText), "addappid({})\n", appId);
    }

    if (baseToken != 0) {
        std::format_to(std::back_inserter(luaText), "addtoken({}, \"{}\")\n", appId, baseToken);
    }

    for (const auto& dk : depotKeys) {
        if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
            if (!dk.hexKey.empty()) {
                std::format_to(std::back_inserter(luaText), "addappid({}, 1, \"{}\")\n", dk.depotId, dk.hexKey);
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

    if (hasBaseManifests) {
        std::format_to(std::back_inserter(luaText), "\n{}\n", TR(MsgKey::LuaBaseManifests));
        if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
            if (hasManifestOnDisk(appId, baseAppDk->manifestId)) {
                std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", appId, baseAppDk->manifestId);
            } else {
                std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", appId, baseAppDk->manifestId, TR(MsgKey::LuaManifestMissing));
            }
        }
        for (const auto& dk : depotKeys) {
            if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
                if (IsValidManifestId(dk.manifestId)) {
                    if (hasManifestOnDisk(dk.depotId, dk.manifestId)) {
                        std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", dk.depotId, dk.manifestId);
                    } else {
                        std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", dk.depotId, dk.manifestId, TR(MsgKey::LuaManifestMissing));
                    }
                }
            }
        }
    }

    // 3. Owned DLCs Unlocks
    if (!dlcs.empty()) {
        struct DlcDepotData {
            const DepotKeyInfo* dlcDk = nullptr;
            std::vector<const DepotKeyInfo*> subDepots;
            std::vector<const DepotKeyInfo*> associatedDepots;
        };
        std::vector<DlcDepotData> dlcDepotList(dlcs.size());
        bool hasDlcManifests = false;

        std::unordered_map<uint32_t, const DepotKeyInfo*> primaryDepots;
        std::unordered_map<uint32_t, std::vector<const DepotKeyInfo*>> subDepotsMap;
        primaryDepots.reserve(depotKeys.size());
        for (const auto& dk : depotKeys) {
            primaryDepots[dk.depotId] = &dk;
            if (dk.dlcId != 0 && dk.dlcId != dk.depotId) {
                subDepotsMap[dk.dlcId].push_back(&dk);
            }
        }

        // Single pass over dlcs to build indexed association table in strictly O(N + M) time
        for (size_t i = 0; i < dlcs.size(); ++i) {
            auto& data = dlcDepotList[i];
            const uint32_t dlcId = dlcs[i].dlcId;
            if (auto it = primaryDepots.find(dlcId); it != primaryDepots.end()) {
                data.dlcDk = it->second;
                data.associatedDepots.push_back(it->second);
            }
            if (auto it = subDepotsMap.find(dlcId); it != subDepotsMap.end()) {
                data.subDepots = it->second;
                data.associatedDepots.insert(data.associatedDepots.end(), it->second.begin(), it->second.end());
            }
            if (!hasDlcManifests) {
                for (const auto* dk : data.associatedDepots) {
                    if (IsValidManifestId(dk->manifestId)) {
                        hasDlcManifests = true;
                        break;
                    }
                }
            }
        }

        std::format_to(std::back_inserter(luaText), "\n{}\n", TR(MsgKey::LuaOwnedDlcs));
        for (size_t i = 0; i < dlcs.size(); ++i) {
            const auto& dlc = dlcs[i];
            const auto& data = dlcDepotList[i];

            bool isDepotIncomplete = false;
            bool hasPhysicalDepot = false;
            for (const auto* dk : data.associatedDepots) {
                if (IsValidManifestId(dk->manifestId)) {
                    hasPhysicalDepot = true;
                    const bool manifestPresent = hasManifestOnDisk(dk->depotId, dk->manifestId);
                    if (!manifestPresent) {
                        isDepotIncomplete = true;
                        break;
                    }
                }
            }
            if (!hasPhysicalDepot) {
                isDepotIncomplete = false; // Pure logical DLC: zero physical files on CDN, always active
            }

            const std::string_view prefix = isDepotIncomplete ? "-- " : "";
            const std::string reasonSuffix = isDepotIncomplete ? (" -- " + std::string(TR(MsgKey::LuaDlcDepotMissing))) : "";

            if (data.dlcDk && !data.dlcDk->hexKey.empty()) {
                std::format_to(std::back_inserter(luaText), "{}addappid({}, 1, \"{}\")", prefix, dlc.dlcId, data.dlcDk->hexKey);
            } else {
                std::format_to(std::back_inserter(luaText), "{}addappid({}, 1)", prefix, dlc.dlcId);
            }

            if (!dlc.name.empty()) {
                luaText += " -- " + SanitizeComment(dlc.name);
            }
            luaText += reasonSuffix + "\n";

            auto dlcTokIt = appTokens.find(dlc.dlcId);
            if (dlcTokIt != appTokens.end() && dlcTokIt->second != 0) {
                std::format_to(std::back_inserter(luaText), "{}addtoken({}, \"{}\")\n", prefix, dlc.dlcId, dlcTokIt->second);
            }

            // Any subdepots of this DLC with keys
            for (const auto* subDk : data.subDepots) {
                if (!subDk->hexKey.empty()) {
                    std::format_to(std::back_inserter(luaText), "{}addappid({}, 1, \"{}\"){}\n", prefix, subDk->depotId, subDk->hexKey, reasonSuffix);
                }
            }
        }

        // 4. DLC Manifests (placed right under DLC unlocks)
        if (hasDlcManifests) {
            std::format_to(std::back_inserter(luaText), "\n{}\n", TR(MsgKey::LuaDlcManifests));
            for (size_t i = 0; i < dlcs.size(); ++i) {
                const auto& dlc = dlcs[i];
                const auto& data = dlcDepotList[i];

                // DLC itself manifest
                if (data.dlcDk && IsValidManifestId(data.dlcDk->manifestId)) {
                    const bool onDisk = hasManifestOnDisk(data.dlcDk->depotId, data.dlcDk->manifestId);
                    if (onDisk) {
                        std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")", data.dlcDk->depotId, data.dlcDk->manifestId);
                    } else {
                        std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}", data.dlcDk->depotId, data.dlcDk->manifestId, TR(MsgKey::LuaManifestMissing));
                    }
                    if (!dlc.name.empty()) {
                        luaText += " -- " + SanitizeComment(dlc.name);
                    }
                    luaText += "\n";
                }

                // Any subdepots of this DLC with manifests
                for (const auto* subDk : data.subDepots) {
                    if (IsValidManifestId(subDk->manifestId)) {
                        const bool onDisk = hasManifestOnDisk(subDk->depotId, subDk->manifestId);
                        if (onDisk) {
                            std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", subDk->depotId, subDk->manifestId);
                        } else {
                            std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", subDk->depotId, subDk->manifestId, TR(MsgKey::LuaManifestMissing));
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

    std::format_to(std::back_inserter(luaText), "{}\n", TR(MsgKey::LuaAppTicket));
    if (hasOwnership) {
        std::format_to(std::back_inserter(luaText), "setAppTicket({}, \"{}\")\n\n",
                       appId,
                       ToHexString(*ownership));
    } else {
        std::format_to(std::back_inserter(luaText), "-- setAppTicket({}, \"null\") -- {}\n\n", appId, TR(MsgKey::LuaAppTicketMissing));
    }

    std::format_to(std::back_inserter(luaText), "{}\n", TR(MsgKey::LuaETicket));
    if (hasEncrypted) {
        std::format_to(std::back_inserter(luaText), "setETicket({}, \"{}\")\n", appId, ToHexString(*encrypted));
    } else {
        std::format_to(std::back_inserter(luaText), "-- setETicket({}, \"null\") -- {}\n", appId, TR(MsgKey::LuaETicketNote));
    }

    const std::filesystem::path luaPath = dirPath / (std::to_string(appId) + ".lua");
    std::ofstream luaFile{luaPath, std::ios::binary | std::ios::trunc};
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
