#include "AppInfoParser.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace OST::ExtractTickets {

std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds) {
    std::unordered_map<uint32_t, uint64_t> tokens;
    if (steamPath.empty() || (targetAppIds && targetAppIds->empty())) {
        return tokens;
    }

    const std::string appinfoPath = JoinPath(steamPath, "appcache\\appinfo.vdf");
    ScopedHandle hFile{CreateFileA(
        appinfoPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    )};

    if (!hFile.IsValid()) {
        return tokens;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 16) {
        return tokens;
    }

    if (fileSize.QuadPart > 1024ULL * 1024ULL * 1024ULL) {
        return tokens;
    }

    ScopedHandle hMapping{CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!hMapping.IsValid()) {
        return tokens;
    }

    ScopedFileMappingView mappedView{MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)};
    if (!mappedView) {
        return tokens;
    }

    const auto* data = mappedView.As<uint8_t>();
    const size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);

    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(uint32_t));

    // Valid appinfo.vdf magic format: 0x075644xx (version >= 38 has AccessToken at offset +16)
    if ((magic & 0xFFFFFF00) != 0x07564400) {
        return tokens;
    }

    const uint8_t version = static_cast<uint8_t>(magic & 0xFF);
    if (version < 38) {
        return tokens;
    }

    size_t offset = 8;
    size_t appsEnd = totalBytes;

    // Version 41+ (0x29+) includes a 64-bit string table offset at file offset 8.
    // Apps section ends at stringTableOffset.
    if (version >= 41) {
        if (totalBytes >= 16) {
            uint64_t stringTableOffset = 0;
            std::memcpy(&stringTableOffset, data + 8, sizeof(uint64_t));
            if (stringTableOffset >= 16 && stringTableOffset <= totalBytes) {
                appsEnd = static_cast<size_t>(stringTableOffset);
            }
            offset = 16;
        }
    }

    std::unordered_set<uint32_t> matchedTargetAppIds;
    while (offset + 8 <= appsEnd) {
        uint32_t entryAppId = 0;
        uint32_t entrySize = 0;
        std::memcpy(&entryAppId, data + offset, sizeof(uint32_t));
        std::memcpy(&entrySize, data + offset + 4, sizeof(uint32_t));

        if (entryAppId == 0) {
            break; // 0 marks end of apps list
        }

        // Each app entry header after size contains at least 60 bytes:
        // InfoState(4) + LastUpdated(4) + AccessToken(8) + SHA1_text(20) + ChangeNumber(4) + SHA1_bin(20) = 60.
        // Prevent overflow and ensure entry stays strictly within appsEnd.
        if (entrySize < 60 || entrySize > appsEnd - (offset + 8)) {
            break;
        }

        // If filtering by specific AppIDs, only extract when matched
        if (!targetAppIds || targetAppIds->contains(entryAppId)) {
            if (targetAppIds) {
                matchedTargetAppIds.insert(entryAppId);
            }
            // AccessToken is at entry offset +16
            uint64_t accessToken = 0;
            std::memcpy(&accessToken, data + offset + 16, sizeof(uint64_t));
            if (accessToken != 0) {
                tokens[entryAppId] = accessToken;
            }
            if (targetAppIds && matchedTargetAppIds.size() >= targetAppIds->size()) {
                break; // All unique target apps found; early exit to avoid scanning remaining thousands of apps
            }
        }

        offset += 8 + entrySize;
    }

    return tokens;
}

namespace {

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

    bool ReadFloat(float& outVal) noexcept {
        if (p + 4 > end) return false;
        std::memcpy(&outVal, p, 4);
        p += 4;
        return true;
    }
};

void ParseVdfRecurse(VdfReader& reader,
                     std::vector<std::string_view>& pathStack,
                     ParsedAppInfoData& outData,
                     uint32_t& currentDepotId) {
    while (reader.HasMore()) {
        uint8_t type = reader.ReadByte();
        if (type == 0x08 || type == 0xFF) {
            return; // end of current object
        }

        std::string_view key;
        if (!reader.ReadKey(key)) return;

        if (type == 0x00) { // Section
            pathStack.push_back(key);

            uint32_t prevDepot = currentDepotId;
            if (pathStack.size() >= 2 && pathStack[pathStack.size() - 2] == "depots") {
                uint32_t parsedId = 0;
                auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), parsedId);
                if (ec == std::errc() && parsedId > 0) {
                    currentDepotId = parsedId;
                    if (std::none_of(outData.depots.begin(), outData.depots.end(),
                                     [&](const AppDepotManifest& d) { return d.depotId == parsedId; })) {
                        outData.depots.push_back({parsedId, "", 0});
                    }
                }
            }

            ParseVdfRecurse(reader, pathStack, outData, currentDepotId);

            currentDepotId = prevDepot;
            pathStack.pop_back();
        } else if (type == 0x01) { // String
            std::string_view val;
            if (!reader.ReadString(val)) return;

            if (currentDepotId > 0 && key == "gid") {
                for (auto& d : outData.depots) {
                    if (d.depotId == currentDepotId && d.manifestId.empty()) {
                        d.manifestId = std::string(val);
                        break;
                    }
                }
            } else if (key == "dlc" && !pathStack.empty() && pathStack.back() == "extended") {
                size_t start = 0;
                while (start < val.size()) {
                    size_t comma = val.find(',', start);
                    std::string_view token = (comma == std::string_view::npos) ? val.substr(start) : val.substr(start, comma - start);
                    token = TrimWhitespace(token);
                    uint32_t dlcId = 0;
                    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), dlcId);
                    if (ec == std::errc() && dlcId > 0) {
                        if (std::ranges::find(outData.dlcAppIds, dlcId) == outData.dlcAppIds.end()) {
                            outData.dlcAppIds.push_back(dlcId);
                        }
                    }
                    if (comma == std::string_view::npos) break;
                    start = comma + 1;
                }
            } else if (key == "name" && !pathStack.empty() && pathStack.back() == "common") {
                if (outData.name.empty()) {
                    outData.name = std::string(val);
                }
            }
        } else if (type == 0x02) { // Int32
            int32_t val = 0;
            if (!reader.ReadInt32(val)) return;

            if (currentDepotId > 0 && key == "dlcappid" && val > 0) {
                for (auto& d : outData.depots) {
                    if (d.depotId == currentDepotId) {
                        d.dlcId = static_cast<uint32_t>(val);
                        break;
                    }
                }
                uint32_t dlcId = static_cast<uint32_t>(val);
                if (std::ranges::find(outData.dlcAppIds, dlcId) == outData.dlcAppIds.end()) {
                    outData.dlcAppIds.push_back(dlcId);
                }
            }
        } else if (type == 0x07) { // UInt64
            uint64_t val = 0;
            if (!reader.ReadUInt64(val)) return;

            if (currentDepotId > 0 && key == "gid" && val > 0) {
                for (auto& d : outData.depots) {
                    if (d.depotId == currentDepotId && d.manifestId.empty()) {
                        d.manifestId = std::to_string(val);
                        break;
                    }
                }
            }
        } else if (type == 0x03) { // Float
            float f = 0.0f;
            if (!reader.ReadFloat(f)) return;
        } else {
            return;
        }
    }
}

} // namespace

std::optional<ParsedAppInfoData> ParseAppInfoDepots(
    const std::string& steamPath, uint32_t appId) {

    if (steamPath.empty() || appId == 0) return std::nullopt;

    const std::string appinfoPath = JoinPath(steamPath, "appcache\\appinfo.vdf");
    ScopedHandle hFile{CreateFileA(
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

    ScopedHandle hMapping{CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!hMapping.IsValid()) return std::nullopt;

    ScopedFileMappingView mappedView{MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)};
    if (!mappedView) return std::nullopt;

    const auto* data = mappedView.As<uint8_t>();
    const size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);

    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(uint32_t));
    if ((magic & 0xFFFFFF00) != 0x07564400) return std::nullopt;

    const uint8_t version = static_cast<uint8_t>(magic & 0xFF);
    if (version < 38) return std::nullopt;

    std::vector<std::string_view> stringTable;
    size_t offset = 8;
    size_t appsEnd = totalBytes;

    if (version >= 41) {
        if (totalBytes >= 16) {
            uint64_t stringTableOffset = 0;
            std::memcpy(&stringTableOffset, data + 8, sizeof(uint64_t));
            if (stringTableOffset >= 16 && stringTableOffset + 4 <= totalBytes) {
                appsEnd = static_cast<size_t>(stringTableOffset);

                uint32_t numStrings = 0;
                std::memcpy(&numStrings, data + stringTableOffset, sizeof(uint32_t));
                stringTable.reserve(std::min(numStrings, 65536U));

                size_t sOff = static_cast<size_t>(stringTableOffset) + 4;
                for (uint32_t i = 0; i < numStrings && sOff < totalBytes; ++i) {
                    const char* sStart = reinterpret_cast<const char*>(data + sOff);
                    size_t sLen = 0;
                    while (sOff + sLen < totalBytes && data[sOff + sLen] != '\0') {
                        ++sLen;
                    }
                    stringTable.emplace_back(sStart, sLen);
                    sOff += sLen + 1;
                }
            }
            offset = 16;
        }
    }

    while (offset + 8 <= appsEnd) {
        uint32_t entryAppId = 0;
        uint32_t entrySize = 0;
        std::memcpy(&entryAppId, data + offset, sizeof(uint32_t));
        std::memcpy(&entrySize, data + offset + 4, sizeof(uint32_t));

        if (entryAppId == 0) break;
        if (entrySize < 60 || entrySize > appsEnd - (offset + 8)) break;

        if (entryAppId == appId) {
            const uint8_t* body = data + offset + 8 + 60;
            size_t bodySize = entrySize - 60;

            ParsedAppInfoData out;
            out.appId = appId;

            VdfReader reader{body, body + bodySize, version >= 41 ? &stringTable : nullptr};
            std::vector<std::string_view> pathStack;
            uint32_t curDepot = 0;
            ParseVdfRecurse(reader, pathStack, out, curDepot);
            return out;
        }

        offset += 8 + entrySize;
    }

    return std::nullopt;
}

std::optional<ParsedAppInfoData> ParseBinaryVdfAppInfo(
    std::span<const uint8_t> buffer, uint32_t appId) {

    if (buffer.empty()) return std::nullopt;

    ParsedAppInfoData out;
    out.appId = appId;

    VdfReader reader{buffer.data(), buffer.data() + buffer.size(), nullptr};
    std::vector<std::string_view> pathStack;
    uint32_t curDepot = 0;
    ParseVdfRecurse(reader, pathStack, out, curDepot);
    return out;
}

std::unordered_map<uint32_t, std::string> ParseAppNames(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>& targetAppIds) {

    std::unordered_map<uint32_t, std::string> names;
    if (steamPath.empty() || targetAppIds.empty()) return names;

    const std::string appinfoPath = JoinPath(steamPath, "appcache\\appinfo.vdf");
    ScopedHandle hFile{CreateFileA(
        appinfoPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    )};

    if (!hFile.IsValid()) return names;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 16 || fileSize.QuadPart > 1024ULL * 1024ULL * 1024ULL) {
        return names;
    }

    ScopedHandle hMapping{CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!hMapping.IsValid()) return names;

    ScopedFileMappingView mappedView{MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)};
    if (!mappedView) return names;

    const auto* data = mappedView.As<uint8_t>();
    const size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);

    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(uint32_t));
    if ((magic & 0xFFFFFF00) != 0x07564400) return names;

    const uint8_t version = static_cast<uint8_t>(magic & 0xFF);
    if (version < 38) return names;

    std::vector<std::string_view> stringTable;
    size_t offset = 8;
    size_t appsEnd = totalBytes;

    if (version >= 41) {
        if (totalBytes >= 16) {
            uint64_t stringTableOffset = 0;
            std::memcpy(&stringTableOffset, data + 8, sizeof(uint64_t));
            if (stringTableOffset >= 16 && stringTableOffset + 4 <= totalBytes) {
                appsEnd = static_cast<size_t>(stringTableOffset);

                uint32_t numStrings = 0;
                std::memcpy(&numStrings, data + stringTableOffset, sizeof(uint32_t));
                stringTable.reserve((std::min)(numStrings, 65536U));

                size_t sOff = static_cast<size_t>(stringTableOffset) + 4;
                for (uint32_t i = 0; i < numStrings && sOff < totalBytes; ++i) {
                    const char* sStart = reinterpret_cast<const char*>(data + sOff);
                    size_t sLen = 0;
                    while (sOff + sLen < totalBytes && data[sOff + sLen] != '\0') {
                        ++sLen;
                    }
                    stringTable.emplace_back(sStart, sLen);
                    sOff += sLen + 1;
                }
            }
            offset = 16;
        }
    }

    while (offset + 8 <= appsEnd) {
        uint32_t entryAppId = 0;
        uint32_t entrySize = 0;
        std::memcpy(&entryAppId, data + offset, sizeof(uint32_t));
        std::memcpy(&entrySize, data + offset + 4, sizeof(uint32_t));

        if (entryAppId == 0) break;
        if (entrySize < 60 || entrySize > appsEnd - (offset + 8)) break;

        if (targetAppIds.contains(entryAppId)) {
            const uint8_t* body = data + offset + 8 + 60;
            size_t bodySize = entrySize - 60;

            ParsedAppInfoData out;
            out.appId = entryAppId;

            VdfReader reader{body, body + bodySize, version >= 41 ? &stringTable : nullptr};
            std::vector<std::string_view> pathStack;
            uint32_t curDepot = 0;
            ParseVdfRecurse(reader, pathStack, out, curDepot);
            if (!out.name.empty()) {
                names[entryAppId] = std::move(out.name);
            }
            if (names.size() >= targetAppIds.size()) {
                break;
            }
        }

        offset += 8 + entrySize;
    }

    return names;
}

} // namespace OST::ExtractTickets
