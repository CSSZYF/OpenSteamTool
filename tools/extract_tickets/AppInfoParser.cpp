#include "AppInfoParser.h"
#include "Log.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>

namespace OST::ExtractTickets {

std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds) {
    std::unordered_map<uint32_t, uint64_t> tokens;
    if (steamPath.empty() || (targetAppIds && targetAppIds->empty())) {
        return tokens;
    }

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

    ScopedHandle hMapping{CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
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

[[nodiscard]] inline bool EqualIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char c1, char c2) {
        return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
    });
}

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
            if (dlcId == 0) return;
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
            }
        } else if (type == 0x02) { // Int32
            int32_t val = 0;
            if (!reader.ReadInt32(val)) return;

            if (EqualIgnoreCase(key, "dlcappid") && currentDepotId > 0 && val > 0) {
                recordDlcId(static_cast<uint32_t>(val));
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

enum class TextVdfTokenType {
    String,
    OpenBrace,
    CloseBrace,
    EndOfFile
};

struct TextVdfToken {
    TextVdfTokenType type{TextVdfTokenType::EndOfFile};
    std::string value;
};

class TextVdfLexer {
public:
    explicit TextVdfLexer(std::string_view text) : m_text(text), m_pos(0), m_len(text.size()) {}

    TextVdfToken Next() {
        if (m_peeked.has_value()) {
            TextVdfToken t = std::move(*m_peeked);
            m_peeked.reset();
            return t;
        }
        return ReadNext();
    }

    const TextVdfToken& Peek() {
        if (!m_peeked.has_value()) {
            m_peeked = ReadNext();
        }
        return *m_peeked;
    }

private:
    TextVdfToken ReadNext() {
        while (m_pos < m_len) {
            char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
                continue;
            }
            if (c == '/' && m_pos + 1 < m_len && m_text[m_pos + 1] == '/') {
                m_pos += 2;
                while (m_pos < m_len && m_text[m_pos] != '\n') {
                    ++m_pos;
                }
                continue;
            }
            if (c == '{') {
                ++m_pos;
                return {TextVdfTokenType::OpenBrace, "{"};
            }
            if (c == '}') {
                ++m_pos;
                return {TextVdfTokenType::CloseBrace, "}"};
            }
            if (c == '"') {
                ++m_pos;
                std::string s;
                while (m_pos < m_len) {
                    char sc = m_text[m_pos];
                    if (sc == '\\' && m_pos + 1 < m_len) {
                        char esc = m_text[m_pos + 1];
                        if (esc == 'n') s += '\n';
                        else if (esc == 't') s += '\t';
                        else s += esc;
                        m_pos += 2;
                    } else if (sc == '"') {
                        ++m_pos;
                        break;
                    } else {
                        s += sc;
                        ++m_pos;
                    }
                }
                return {TextVdfTokenType::String, std::move(s)};
            }

            size_t start = m_pos;
            while (m_pos < m_len) {
                char bc = m_text[m_pos];
                if (bc == ' ' || bc == '\t' || bc == '\r' || bc == '\n' ||
                    bc == '{' || bc == '}' || bc == '"' ||
                    (bc == '/' && m_pos + 1 < m_len && m_text[m_pos + 1] == '/')) {
                    break;
                }
                ++m_pos;
            }
            return {TextVdfTokenType::String, std::string(m_text.substr(start, m_pos - start))};
        }
        return {TextVdfTokenType::EndOfFile, ""};
    }

    std::string_view m_text;
    size_t m_pos{0};
    size_t m_len{0};
    std::optional<TextVdfToken> m_peeked;
};

void ParseTextVdfRecurse(
    TextVdfLexer& lexer,
    std::vector<std::string>& pathStack,
    uint32_t currentDepotId,
    ParsedAppInfoData& outData)
{
    if (pathStack.size() > 64) return;

    auto recordManifest = [&](uint32_t depotId, std::string_view gidStr, bool isPublic) {
        if (depotId == 0 || gidStr.empty() || !IsValidManifestId(gidStr)) return;
        for (auto& d : outData.depots) {
            if (d.depotId == depotId) {
                if (isPublic || d.manifestId.empty()) {
                    d.manifestId = std::string(gidStr);
                    LOG_DEBUG("AppInfoParser", "Depot {} 记录清单 GID: {} (分支: {})",
                              depotId, gidStr, isPublic ? "public" : "other");
                }
                break;
            }
        }
    };

    auto recordDlcId = [&](uint32_t depotId, uint32_t dlcId) {
        if (dlcId == 0) return;
        if (depotId > 0) {
            for (auto& d : outData.depots) {
                if (d.depotId == depotId && d.dlcId == 0) {
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

    while (true) {
        const auto& peek = lexer.Peek();
        if (peek.type == TextVdfTokenType::CloseBrace || peek.type == TextVdfTokenType::EndOfFile) {
            if (peek.type == TextVdfTokenType::CloseBrace) {
                (void)lexer.Next();
            }
            return;
        }

        if (peek.type == TextVdfTokenType::OpenBrace) {
            (void)lexer.Next();
            ParseTextVdfRecurse(lexer, pathStack, currentDepotId, outData);
            continue;
        }

        TextVdfToken keyToken = lexer.Next();
        std::string key = std::move(keyToken.value);

        const auto& afterKey = lexer.Peek();
        if (afterKey.type == TextVdfTokenType::OpenBrace) {
            (void)lexer.Next();

            uint32_t nextDepotId = currentDepotId;
            if (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "depots")) {
                uint32_t parsedId = 0;
                auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), parsedId);
                if (ec == std::errc() && parsedId > 0) {
                    nextDepotId = parsedId;
                    if (std::none_of(outData.depots.begin(), outData.depots.end(),
                                     [&](const AppDepotManifest& d) { return d.depotId == parsedId; })) {
                        outData.depots.push_back({parsedId, "", 0});
                        LOG_DEBUG("AppInfoParser", "发现 Depot ID: {}", parsedId);
                    }
                }
            }

            pathStack.push_back(key);
            ParseTextVdfRecurse(lexer, pathStack, nextDepotId, outData);
            pathStack.pop_back();
        } else if (afterKey.type == TextVdfTokenType::String) {
            TextVdfToken valToken = lexer.Next();
            const std::string& val = valToken.value;

            const bool isPublicBranch = (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "public"));
            const bool isManifestsParent = (!pathStack.empty() && EqualIgnoreCase(pathStack.back(), "manifests"));
            const bool isPublicKey = EqualIgnoreCase(key, "public");
            const bool isGidKey = EqualIgnoreCase(key, "gid");

            if (isGidKey || (isManifestsParent && isPublicKey)) {
                recordManifest(currentDepotId, val, isPublicBranch || isPublicKey);
            } else if ((EqualIgnoreCase(key, "dlc") || EqualIgnoreCase(key, "listofdlc")) &&
                       !pathStack.empty() && EqualIgnoreCase(pathStack.back(), "extended")) {
                size_t start = 0;
                while (start < val.size()) {
                    size_t comma = val.find(',', start);
                    std::string_view token = (comma == std::string::npos) ?
                        std::string_view(val).substr(start) :
                        std::string_view(val).substr(start, comma - start);
                    token = TrimWhitespace(token);
                    uint32_t dlcId = 0;
                    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), dlcId);
                    if (ec == std::errc() && dlcId > 0) {
                        recordDlcId(currentDepotId, dlcId);
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            } else if (EqualIgnoreCase(key, "dlcappid") && currentDepotId > 0) {
                uint32_t dlcId = 0;
                auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), dlcId);
                if (ec == std::errc() && dlcId > 0) {
                    recordDlcId(currentDepotId, dlcId);
                }
            } else if (EqualIgnoreCase(key, "name") && !pathStack.empty() && EqualIgnoreCase(pathStack.back(), "common")) {
                if (outData.name.empty()) {
                    outData.name = val;
                    LOG_DEBUG("AppInfoParser", "解析到游戏官方名称: '{}'", outData.name);
                }
            }
        }
    }
}

void ParseTextVdfAppInfo(std::string_view text, ParsedAppInfoData& outData) {
    TextVdfLexer lexer(text);
    std::vector<std::string> pathStack;
    ParseTextVdfRecurse(lexer, pathStack, 0, outData);
}

} // namespace

std::optional<ParsedAppInfoData> ParseAppInfoDepots(
    const std::string& steamPath, uint32_t appId) {

    if (steamPath.empty() || appId == 0) return std::nullopt;

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
                ParseStringTableV41(data, totalBytes, stringTableOffset, stringTable);
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

    std::string hexDump;
    for (size_t i = 0; i < (std::min<size_t>)(32, buffer.size()); ++i) {
        hexDump += std::format("{:02X} ", buffer[i]);
    }
    LOG_DEBUG("AppInfoParser", "开始解析 PICS VDF 数据 (大小: {} 字节, AppID: {}, 前32字节Hex: {})",
              buffer.size(), appId, hexDump);

    ParsedAppInfoData out;
    out.appId = appId;

    // Detect format: Text KeyValues vs Binary VDF
    bool isText = false;
    for (uint8_t b : buffer) {
        if (b == ' ' || b == '\t' || b == '\r' || b == '\n') continue;
        if (b == '"' || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '{') {
            isText = true;
        }
        break;
    }

    if (isText) {
        LOG_DEBUG("AppInfoParser", "检测到 PICS 响应为文本 VDF (KeyValues) 格式，启动文本解析引擎...");
        std::string_view text(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        ParseTextVdfAppInfo(text, out);
    } else {
        LOG_DEBUG("AppInfoParser", "检测到 PICS 响应为二进制 VDF 格式，启动二进制解析引擎...");
        VdfReader reader{buffer.data(), buffer.data() + buffer.size(), nullptr};
        std::vector<std::string_view> pathStack;
        uint32_t curDepot = 0;
        ParseVdfRecurse(reader, pathStack, out, curDepot);
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

    if (!hFile.IsValid()) return names;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 16 || fileSize.QuadPart > 1024ULL * 1024ULL * 1024ULL) {
        return names;
    }

    ScopedHandle hMapping{CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
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
                ParseStringTableV41(data, totalBytes, stringTableOffset, stringTable);
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
