#include "AppInfoParser.h"
#include "I18n.h"
#include "Log.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
#include <ranges>

namespace OST::ExtractTickets {

namespace {

[[nodiscard]] inline bool EqualIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char c1, char c2) {
        return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
    });
}

void ParseStringTableV41(const uint8_t* data, size_t totalBytes, uint64_t stringTableOffset, std::vector<std::string_view>& outTable) {
    if (stringTableOffset < 16 || stringTableOffset + 4 > totalBytes) return;

    uint32_t numStrings = 0;
    std::memcpy(&numStrings, data + stringTableOffset, sizeof(uint32_t));
    outTable.reserve((std::min)(numStrings, 65536U));

    size_t sOff = static_cast<size_t>(stringTableOffset) + 4;
    for (uint32_t i = 0; i < numStrings && sOff < totalBytes; ++i) {
        const char* sStart = reinterpret_cast<const char*>(data + sOff);
        size_t sLen = 0;
        while (sOff + sLen < totalBytes && data[sOff + sLen] != '\0') {
            ++sLen;
        }
        outTable.emplace_back(sStart, sLen);
        sOff += sLen + 1;
    }
}

struct AppInfoEntryView {
    uint32_t appId{0};
    uint64_t accessToken{0};
    std::span<const uint8_t> vdfBody;
};

struct AppInfoFileContext {
    ScopedHandle hFile;
    ScopedHandle hMapping;
    ScopedFileMappingView mappedView;
    const uint8_t* data{nullptr};
    size_t totalBytes{0};
    uint8_t version{0};
    size_t appsStartOffset{8};
    size_t appsEndOffset{0};
    std::vector<std::string_view> stringTable;

    static std::optional<AppInfoFileContext> Open(const std::string& steamPath) {
        if (steamPath.empty()) return std::nullopt;
        const auto appinfoPath = std::filesystem::path(steamPath) / "appcache" / "appinfo.vdf";
        ScopedHandle hFile{CreateFileW(
            appinfoPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        )};
        if (!hFile.IsValid()) return std::nullopt;

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 16 || fileSize.QuadPart > 1024ULL * 1024ULL * 1024ULL) {
            return std::nullopt;
        }

        ScopedHandle hMapping{CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
        if (!hMapping.IsValid()) return std::nullopt;

        ScopedFileMappingView mappedView{MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)};
        if (!mappedView) return std::nullopt;

        AppInfoFileContext ctx;
        ctx.hFile = std::move(hFile);
        ctx.hMapping = std::move(hMapping);
        ctx.mappedView = std::move(mappedView);
        ctx.data = ctx.mappedView.As<uint8_t>();
        ctx.totalBytes = static_cast<size_t>(fileSize.QuadPart);

        uint32_t magic = 0;
        std::memcpy(&magic, ctx.data, sizeof(uint32_t));
        if ((magic & 0xFFFFFF00) != 0x07564400) return std::nullopt;

        ctx.version = static_cast<uint8_t>(magic & 0xFF);
        if (ctx.version < 38) return std::nullopt;

        ctx.appsStartOffset = 8;
        ctx.appsEndOffset = ctx.totalBytes;

        if (ctx.version >= 41) {
            if (ctx.totalBytes >= 16) {
                uint64_t stringTableOffset = 0;
                std::memcpy(&stringTableOffset, ctx.data + 8, sizeof(uint64_t));
                if (stringTableOffset >= 16 && stringTableOffset + 4 <= ctx.totalBytes) {
                    ctx.appsEndOffset = static_cast<size_t>(stringTableOffset);
                    ParseStringTableV41(ctx.data, ctx.totalBytes, stringTableOffset, ctx.stringTable);
                }
                ctx.appsStartOffset = 16;
            }
        }
        return ctx;
    }

    template <typename Callback>
    void ForEachApp(Callback&& cb) const {
        size_t offset = appsStartOffset;
        while (offset + 8 <= appsEndOffset) {
            uint32_t entryAppId = 0;
            uint32_t entrySize = 0;
            std::memcpy(&entryAppId, data + offset, sizeof(uint32_t));
            std::memcpy(&entrySize, data + offset + 4, sizeof(uint32_t));

            if (entryAppId == 0) break;
            if (entrySize < 60 || entrySize > appsEndOffset - (offset + 8)) break;

            uint64_t accessToken = 0;
            std::memcpy(&accessToken, data + offset + 16, sizeof(uint64_t));

            const uint8_t* body = data + offset + 8 + 60;
            const size_t bodySize = entrySize - 60;

            AppInfoEntryView entry{entryAppId, accessToken, std::span<const uint8_t>(body, bodySize)};
            if (!cb(entry)) {
                break;
            }

            offset += 8 + entrySize;
        }
    }
};

struct VdfReader {
    const uint8_t* p{nullptr};
    const uint8_t* end{nullptr};
    const std::vector<std::string_view>* stringTable{nullptr};

    [[nodiscard]] bool HasMore() const noexcept { return p < end; }

    [[nodiscard]] uint8_t ReadByte() noexcept {
        return (p < end) ? *p++ : 0xFF;
    }

    bool ReadKey(std::string_view& outKey) noexcept {
        if (stringTable && !stringTable->empty()) {
            if (p + 4 > end) return false;
            uint32_t idx = 0;
            std::memcpy(&idx, p, 4);
            p += 4;
            if (idx < stringTable->size()) {
                outKey = (*stringTable)[idx];
            } else {
                outKey = "";
            }
            return true;
        } else {
            const char* start = reinterpret_cast<const char*>(p);
            while (p < end && *p != '\0') {
                ++p;
            }
            if (p >= end) return false;
            outKey = std::string_view(start, reinterpret_cast<const char*>(p) - start);
            ++p; // skip null terminator
            return true;
        }
    }

    bool ReadString(std::string_view& outVal) noexcept {
        const char* start = reinterpret_cast<const char*>(p);
        while (p < end && *p != '\0') {
            ++p;
        }
        if (p >= end) return false;
        outVal = std::string_view(start, reinterpret_cast<const char*>(p) - start);
        ++p;
        return true;
    }

    bool ReadInt32(int32_t& outVal) noexcept {
        if (p + 4 > end) return false;
        std::memcpy(&outVal, p, 4);
        p += 4;
        return true;
    }

    bool ReadUInt64(uint64_t& outVal) noexcept {
        if (p + 8 > end) return false;
        std::memcpy(&outVal, p, 8);
        p += 8;
        return true;
    }

    bool ReadInt64(int64_t& outVal) noexcept {
        if (p + 8 > end) return false;
        std::memcpy(&outVal, p, 8);
        p += 8;
        return true;
    }

    bool ReadFloat(float& outVal) noexcept {
        if (p + 4 > end) return false;
        std::memcpy(&outVal, p, 4);
        p += 4;
        return true;
    }

    bool SkipBytes(size_t n) noexcept {
        if (p + n > end) return false;
        p += n;
        return true;
    }

    bool SkipWideString() noexcept {
        while (p + 2 <= end) {
            uint16_t ch = 0;
            std::memcpy(&ch, p, 2);
            p += 2;
            if (ch == 0) return true;
        }
        return false;
    }
};

void ParseVdfRecurse(VdfReader& reader,
                     std::vector<std::string_view>& pathStack,
                     ParsedAppInfoData& outData,
                     uint32_t& currentDepotId) {
    if (pathStack.size() > 64) {
        return; // Guard against stack overflow on corrupt or excessively deep VDF
    }
    while (reader.HasMore()) {
        uint8_t type = reader.ReadByte();
        if (type == 0x08 || type == 0x0B || type == 0xFF) {
            return; // end of current object (0x08 = End, 0x0B = AlternateEnd)
        }

        std::string_view key;
        if (!reader.ReadKey(key)) return;

        // Handle Valve PICS Root node wrapper with leading empty key (00 00 <root_name> 00)
        if (type == 0x00 && key.empty() && pathStack.empty()) {
            if (reader.ReadKey(key)) {
                LOG_DEBUG("AppInfoParser", "检测到 PICS 二进制流包含 Root 包裹名: '{}'", key);
            }
        }

        const bool isPublicBranch = (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "public"));
        const bool isDirectManifestKey = (EqualIgnoreCase(key, "gid") ||
                                          (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "manifests") && EqualIgnoreCase(key, "public")));

        auto recordManifest = [&](std::string_view gidStr, bool isPublic) {
            if (currentDepotId == 0 || gidStr.empty() || !IsValidManifestId(gidStr)) return;
            for (auto& d : outData.depots) {
                if (d.depotId == currentDepotId) {
                    if (isPublic || d.manifestId.empty()) {
                        d.manifestId = std::string(gidStr);
                        LOG_DEBUG("AppInfoParser", "Depot {} 记录清单 GID: {} (分支: {})",
                                  currentDepotId, gidStr, isPublic ? "public" : "other");
                    }
                    break;
                }
            }
        };

        auto recordDlcId = [&](uint32_t dlcId) {
            if (dlcId == 0 || dlcId == outData.appId) return;
            if (currentDepotId > 0) {
                for (auto& d : outData.depots) {
                    if (d.depotId == currentDepotId && d.dlcId == 0) {
                        d.dlcId = dlcId;
                        break;
                    }
                }
            }
            if (std::ranges::find(outData.dlcAppIds, dlcId) == outData.dlcAppIds.end()) {
                outData.dlcAppIds.push_back(dlcId);
                LOG_DEBUG("AppInfoParser", "发现关联 DLC AppID: {}", dlcId);
            }
        };

        if (type == 0x00) { // Section
            pathStack.push_back(key);

            uint32_t prevDepot = currentDepotId;
            if (pathStack.size() >= 2 && EqualIgnoreCase(pathStack[pathStack.size() - 2], "depots")) {
                uint32_t parsedId = 0;
                auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), parsedId);
                if (ec == std::errc() && parsedId > 0) {
                    currentDepotId = parsedId;
                    if (std::none_of(outData.depots.begin(), outData.depots.end(),
                                     [&](const AppDepotManifest& d) { return d.depotId == parsedId; })) {
                        outData.depots.push_back({parsedId, "", 0});
                        LOG_DEBUG("AppInfoParser", "发现 Depot ID: {}", parsedId);
                    }
                }
            }

            ParseVdfRecurse(reader, pathStack, outData, currentDepotId);

            currentDepotId = prevDepot;
            pathStack.pop_back();
        } else if (type == 0x01) { // String
            std::string_view val;
            if (!reader.ReadString(val)) return;

            if (isDirectManifestKey) {
                recordManifest(val, isPublicBranch || EqualIgnoreCase(key, "public"));
            } else if ((EqualIgnoreCase(key, "dlc") || EqualIgnoreCase(key, "listofdlc")) &&
                       !pathStack.empty() && EqualIgnoreCase(pathStack.back(), "extended")) {
                size_t start = 0;
                while (start < val.size()) {
                    size_t comma = val.find(',', start);
                    std::string_view token = (comma == std::string_view::npos) ? val.substr(start) : val.substr(start, comma - start);
                    token = TrimWhitespace(token);
                    uint32_t dlcId = 0;
                    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), dlcId);
                    if (ec == std::errc() && dlcId > 0) {
                        recordDlcId(dlcId);
                    }
                    if (comma == std::string_view::npos) break;
                    start = comma + 1;
                }
            } else if (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "extended")) {
                if (EqualIgnoreCase(key, "gamerequiresdenuvo") && (val == "1" || EqualIgnoreCase(val, "true"))) {
                    outData.requiresDenuvo = true;
                    LOG_DEBUG("AppInfoParser", "检测到 Denuvo 反篡改保护标记 (gamerequiresdenuvo={})", val);
                } else if (EqualIgnoreCase(key, "thirdpartydrm") &&
                           (val.find("denuvo") != std::string_view::npos || val.find("Denuvo") != std::string_view::npos)) {
                    outData.requiresDenuvo = true;
                    LOG_DEBUG("AppInfoParser", "检测到第三方 DRM Denuvo 标记: {}", val);
                }
            } else if (EqualIgnoreCase(key, "dlcappid") && currentDepotId > 0) {
                uint32_t dlcId = 0;
                auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), dlcId);
                if (ec == std::errc() && dlcId > 0) {
                    recordDlcId(dlcId);
                }
            } else if (EqualIgnoreCase(key, "name") && !pathStack.empty() && EqualIgnoreCase(pathStack.back(), "common")) {
                if (outData.name.empty()) {
                    outData.name = std::string(val);
                    LOG_DEBUG("AppInfoParser", "解析到游戏官方名称: '{}'", outData.name);
                }
            } else if (pathStack.size() >= 2 && EqualIgnoreCase(pathStack.back(), "name_localized") &&
                       EqualIgnoreCase(pathStack[pathStack.size() - 2], "common")) {
                if (EqualIgnoreCase(key, I18n::GetSteamLanguageCode())) {
                    outData.localizedName = std::string(val);
                    LOG_DEBUG("AppInfoParser", "解析到匹配语言的官方本地化名称: '{}' ({})", outData.localizedName, key);
                } else if (I18n::GetCurrentLanguage() == Language::Chinese && EqualIgnoreCase(key, "tchinese") && outData.localizedName.empty()) {
                    outData.localizedName = std::string(val);
                } else if (I18n::GetCurrentLanguage() == Language::Spanish && EqualIgnoreCase(key, "latam") && outData.localizedName.empty()) {
                    outData.localizedName = std::string(val);
                }
            }
        } else if (type == 0x02) { // Int32
            int32_t val = 0;
            if (!reader.ReadInt32(val)) return;

            if (EqualIgnoreCase(key, "dlcappid") && currentDepotId > 0 && val > 0) {
                recordDlcId(static_cast<uint32_t>(val));
            } else if (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "extended") &&
                       EqualIgnoreCase(key, "gamerequiresdenuvo") && val != 0) {
                outData.requiresDenuvo = true;
                LOG_DEBUG("AppInfoParser", "检测到 Denuvo 反篡改保护标记 (gamerequiresdenuvo={})", val);
            }
        } else if (type == 0x03) { // Float
            float f = 0.0f;
            if (!reader.ReadFloat(f)) return;
        } else if (type == 0x04) { // Pointer
            if (!reader.SkipBytes(4)) return;
        } else if (type == 0x05) { // WideString
            if (!reader.SkipWideString()) return;
        } else if (type == 0x06) { // Color
            if (!reader.SkipBytes(4)) return;
        } else if (type == 0x07) { // UInt64
            uint64_t val = 0;
            if (!reader.ReadUInt64(val)) return;

            if (isDirectManifestKey && val > 0) {
                recordManifest(std::to_string(val), isPublicBranch || EqualIgnoreCase(key, "public"));
            } else if (EqualIgnoreCase(key, "dlcappid") && currentDepotId > 0 && val > 0) {
                recordDlcId(static_cast<uint32_t>(val));
            }
        } else if (type == 0x09) { // CompiledInt
            if (!reader.SkipBytes(4)) return;
        } else if (type == 0x0A) { // Int64
            int64_t val = 0;
            if (!reader.ReadInt64(val)) return;

            if (isDirectManifestKey && val > 0) {
                recordManifest(std::to_string(val), isPublicBranch || EqualIgnoreCase(key, "public"));
            } else if (EqualIgnoreCase(key, "dlcappid") && currentDepotId > 0 && val > 0) {
                recordDlcId(static_cast<uint32_t>(val));
            }
        } else {
            // Unknown or unsupported type encountered: skip this attribute rather than aborting traversal
            continue;
        }
    }
}

} // namespace

std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds) {

    std::unordered_map<uint32_t, uint64_t> tokens;
    if (steamPath.empty() || (targetAppIds && targetAppIds->empty())) {
        return tokens;
    }

    auto ctx = AppInfoFileContext::Open(steamPath);
    if (!ctx) return tokens;

    std::unordered_set<uint32_t> matchedTargetAppIds;
    ctx->ForEachApp([&](const AppInfoEntryView& entry) {
        if (!targetAppIds || targetAppIds->contains(entry.appId)) {
            if (targetAppIds) {
                matchedTargetAppIds.insert(entry.appId);
            }
            if (entry.accessToken != 0) {
                tokens[entry.appId] = entry.accessToken;
            }
            if (targetAppIds && matchedTargetAppIds.size() >= targetAppIds->size()) {
                return false; // All found, stop early
            }
        }
        return true;
    });

    return tokens;
}

std::optional<ParsedAppInfoData> ParseAppInfoDepots(
    const std::string& steamPath, uint32_t appId) {

    if (steamPath.empty() || appId == 0) return std::nullopt;

    auto ctx = AppInfoFileContext::Open(steamPath);
    if (!ctx) return std::nullopt;

    std::optional<ParsedAppInfoData> result;
    ctx->ForEachApp([&](const AppInfoEntryView& entry) {
        if (entry.appId == appId) {
            ParsedAppInfoData out;
            out.appId = appId;

            VdfReader reader{entry.vdfBody.data(), entry.vdfBody.data() + entry.vdfBody.size(),
                             ctx->version >= 41 ? &ctx->stringTable : nullptr};
            std::vector<std::string_view> pathStack;
            uint32_t curDepot = 0;
            ParseVdfRecurse(reader, pathStack, out, curDepot);
            if (!out.localizedName.empty()) {
                out.name = std::move(out.localizedName);
            }
            result = std::move(out);
            return false; // Found, stop early
        }
        return true;
    });

    return result;
}

std::optional<ParsedAppInfoData> ParseBinaryVdfAppInfo(
    std::span<const uint8_t> buffer, uint32_t appId) {

    if (buffer.empty()) return std::nullopt;

    std::string hexDump;
    for (size_t i = 0; i < (std::min<size_t>)(32, buffer.size()); ++i) {
        hexDump += std::format("{:02X} ", buffer[i]);
    }
    LOG_DEBUG("AppInfoParser", "开始解析 PICS VDF 数据 (大小: {} 字节, AppID: {}, 前32字节Hex: {})",
              buffer.size(), appId, hexDump);

    ParsedAppInfoData out;
    out.appId = appId;

    VdfReader reader{buffer.data(), buffer.data() + buffer.size(), nullptr};
    std::vector<std::string_view> pathStack;
    uint32_t curDepot = 0;
    ParseVdfRecurse(reader, pathStack, out, curDepot);

    if (!out.localizedName.empty()) {
        out.name = std::move(out.localizedName);
    }

    LOG_INFO("AppInfoParser", "PICS VDF 解析完成 (AppID: {}, 提取到 {} 个 Depot, {} 个 DLC)",
             appId, out.depots.size(), out.dlcAppIds.size());
    return out;
}

std::unordered_map<uint32_t, std::string> ParseAppNames(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>& targetAppIds) {

    std::unordered_map<uint32_t, std::string> names;
    if (steamPath.empty() || targetAppIds.empty()) return names;

    auto ctx = AppInfoFileContext::Open(steamPath);
    if (!ctx) return names;

    ctx->ForEachApp([&](const AppInfoEntryView& entry) {
        if (targetAppIds.contains(entry.appId)) {
            ParsedAppInfoData out;
            out.appId = entry.appId;

            VdfReader reader{entry.vdfBody.data(), entry.vdfBody.data() + entry.vdfBody.size(),
                             ctx->version >= 41 ? &ctx->stringTable : nullptr};
            std::vector<std::string_view> pathStack;
            uint32_t curDepot = 0;
            ParseVdfRecurse(reader, pathStack, out, curDepot);
            if (!out.localizedName.empty()) {
                out.name = std::move(out.localizedName);
            }
            if (!out.name.empty()) {
                names[entry.appId] = std::move(out.name);
            }
            if (names.size() >= targetAppIds.size()) {
                return false; // Found all, stop early
            }
        }
        return true;
    });

    return names;
}

} // namespace OST::ExtractTickets
