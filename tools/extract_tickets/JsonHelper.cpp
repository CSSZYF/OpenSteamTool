#include "JsonHelper.h"

#include <cctype>
#include <charconv>

namespace OST::ExtractTickets {

namespace {
    std::string_view SkipWhitespace(std::string_view sv) {
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
            sv.remove_prefix(1);
        }
        return sv;
    }

    size_t FindKeyPosition(std::string_view json, std::string_view key) {
        char stackBuf[128];
        std::string heapBuf;
        std::string_view pattern;
        if (key.size() + 2 <= sizeof(stackBuf)) {
            stackBuf[0] = '"';
            std::memcpy(stackBuf + 1, key.data(), key.size());
            stackBuf[key.size() + 1] = '"';
            pattern = std::string_view(stackBuf, key.size() + 2);
        } else {
            heapBuf = "\"" + std::string(key) + "\"";
            pattern = heapBuf;
        }

        size_t pos = 0;
        while ((pos = json.find(pattern, pos)) != std::string_view::npos) {
            size_t afterKey = pos + pattern.size();
            afterKey = json.find_first_not_of(" \t\r\n", afterKey);
            if (afterKey != std::string_view::npos && json[afterKey] == ':') {
                return afterKey + 1; // Position immediately after ':'
            }
            pos += pattern.size();
        }
        return std::string_view::npos;
    }

    std::string UnescapeJsonString(std::string_view input) {
        std::string out;
        out.reserve(input.size());

        for (size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '\\' && i + 1 < input.size()) {
                char next = input[++i];
                switch (next) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        // 4-hex digit unicode escape
                        if (i + 4 < input.size()) {
                            unsigned int codepoint = 0;
                            auto hexChunk = input.substr(i + 1, 4);
                            auto [ptr, ec] = std::from_chars(hexChunk.data(), hexChunk.data() + 4, codepoint, 16);
                            if (ec == std::errc()) {
                                i += 4;
                                if (codepoint <= 0x7F) {
                                    out.push_back(static_cast<char>(codepoint));
                                } else if (codepoint <= 0x7FF) {
                                    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
                                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                                } else {
                                    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
                                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                                }
                                break;
                            }
                        }
                        out.push_back('?');
                        break;
                    }
                    default:
                        out.push_back(next);
                        break;
                }
            } else {
                out.push_back(input[i]);
            }
        }
        return out;
    }
} // namespace

std::optional<std::string> JsonHelper::GetString(std::string_view json, std::string_view key) {
    size_t valPos = FindKeyPosition(json, key);
    if (valPos == std::string_view::npos) return std::nullopt;

    std::string_view sv = SkipWhitespace(json.substr(valPos));
    if (sv.empty() || sv.front() != '"') return std::nullopt;

    sv.remove_prefix(1); // skip opening quote
    size_t endQuote = 0;
    bool escaped = false;
    for (size_t i = 0; i < sv.size(); ++i) {
        if (escaped) {
            escaped = false;
        } else if (sv[i] == '\\') {
            escaped = true;
        } else if (sv[i] == '"') {
            endQuote = i;
            return UnescapeJsonString(sv.substr(0, endQuote));
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> JsonHelper::GetUInt64(std::string_view json, std::string_view key) {
    size_t valPos = FindKeyPosition(json, key);
    if (valPos == std::string_view::npos) return std::nullopt;

    std::string_view sv = SkipWhitespace(json.substr(valPos));
    if (sv.empty()) return std::nullopt;

    if (sv.front() == '"') {
        sv.remove_prefix(1);
        size_t endQuote = sv.find('"');
        if (endQuote == std::string_view::npos) return std::nullopt;
        sv = sv.substr(0, endQuote);
    } else {
        size_t endNum = sv.find_first_of(" \t\r\n,}]");
        if (endNum != std::string_view::npos) {
            sv = sv.substr(0, endNum);
        }
    }

    uint64_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc()) {
        return val;
    }
    return std::nullopt;
}

std::optional<uint32_t> JsonHelper::GetUInt32(std::string_view json, std::string_view key) {
    auto val64 = GetUInt64(json, key);
    if (val64.has_value()) {
        return static_cast<uint32_t>(*val64);
    }
    return std::nullopt;
}

std::vector<AllowedConfirmation> JsonHelper::GetConfirmations(std::string_view json) {
    std::vector<AllowedConfirmation> list;
    size_t targetPos = json.find("\"allowed_confirmations\"");
    if (targetPos == std::string_view::npos) return list;

    size_t arrayStart = json.find('[', targetPos);
    if (arrayStart == std::string_view::npos) return list;

    size_t arrayEnd = json.find(']', arrayStart);
    if (arrayEnd == std::string_view::npos) return list;

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos || objStart >= arrayEnd) break;

        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string_view::npos || objEnd > arrayEnd) break;

        std::string_view itemJson = json.substr(objStart, objEnd - objStart + 1);
        auto cTypeOpt = GetUInt32(itemJson, "confirmation_type");
        if (cTypeOpt) {
            AllowedConfirmation conf;
            conf.type = static_cast<int>(*cTypeOpt);
            conf.associatedMessage = GetString(itemJson, "associated_message").value_or("");
            list.push_back(std::move(conf));
        }

        pos = objEnd + 1;
    }
    return list;
}

std::vector<std::string> JsonHelper::GetStringArray(std::string_view json, std::string_view key) {
    std::vector<std::string> items;
    size_t keyPos = FindKeyPosition(json, key);
    if (keyPos == std::string_view::npos) return items;

    size_t arrayStart = json.find('[', keyPos);
    if (arrayStart == std::string_view::npos) return items;

    size_t arrayEnd = json.find(']', arrayStart);
    if (arrayEnd == std::string_view::npos) return items;

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t strStart = json.find('"', pos);
        if (strStart == std::string_view::npos || strStart >= arrayEnd) break;

        size_t strEnd = json.find('"', strStart + 1);
        if (strEnd == std::string_view::npos || strEnd > arrayEnd) break;

        items.emplace_back(json.substr(strStart + 1, strEnd - strStart - 1));
        pos = strEnd + 1;
    }
    return items;
}

std::vector<OwnedGameInfo> JsonHelper::ParseOwnedGames(std::string_view json) {
    std::vector<OwnedGameInfo> games;

    size_t gamesPos = json.find("\"games\"");
    if (gamesPos == std::string_view::npos) return games;

    size_t arrayStart = json.find('[', gamesPos);
    if (arrayStart == std::string_view::npos) return games;

    // Track bracket depth to locate true end of games array
    size_t arrayEnd = std::string_view::npos;
    int bracketDepth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = arrayStart; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\' && inStr) { escaped = true; continue; }
        if (c == '"') { inStr = !inStr; continue; }
        if (inStr) continue;

        if (c == '[') {
            bracketDepth++;
        } else if (c == ']') {
            bracketDepth--;
            if (bracketDepth == 0) {
                arrayEnd = i;
                break;
            }
        }
    }
    if (arrayEnd == std::string_view::npos) {
        arrayEnd = json.size();
    }

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos || objStart >= arrayEnd) break;

        size_t objEnd = std::string_view::npos;
        int braceDepth = 0;
        inStr = false;
        escaped = false;
        for (size_t i = objStart; i < arrayEnd; ++i) {
            char c = json[i];
            if (escaped) { escaped = false; continue; }
            if (c == '\\' && inStr) { escaped = true; continue; }
            if (c == '"') { inStr = !inStr; continue; }
            if (inStr) continue;

            if (c == '{') {
                braceDepth++;
            } else if (c == '}') {
                braceDepth--;
                if (braceDepth == 0) {
                    objEnd = i;
                    break;
                }
            }
        }

        if (objEnd == std::string_view::npos || objEnd >= arrayEnd) break;

        std::string_view itemJson = json.substr(objStart, objEnd - objStart + 1);

        auto appIdOpt = GetUInt32(itemJson, "appid");
        auto nameOpt = GetString(itemJson, "name");

        if (appIdOpt && *appIdOpt > 0) {
            OwnedGameInfo info;
            info.appId = *appIdOpt;
            info.name = nameOpt.value_or("App " + std::to_string(info.appId));
            games.push_back(std::move(info));
        }

        pos = objEnd + 1;
    }

    return games;
}

std::vector<OwnedGameInfo> JsonHelper::ParseSharedLibraryApps(std::string_view json) {
    std::vector<OwnedGameInfo> games;

    size_t appsPos = json.find("\"apps\"");
    if (appsPos == std::string_view::npos) return games;

    size_t arrayStart = json.find('[', appsPos);
    if (arrayStart == std::string_view::npos) return games;

    // Track bracket depth to locate true end of apps array (skipping inner brackets like owner_steamids: [...])
    size_t arrayEnd = std::string_view::npos;
    int bracketDepth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = arrayStart; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\' && inStr) { escaped = true; continue; }
        if (c == '"') { inStr = !inStr; continue; }
        if (inStr) continue;

        if (c == '[') {
            bracketDepth++;
        } else if (c == ']') {
            bracketDepth--;
            if (bracketDepth == 0) {
                arrayEnd = i;
                break;
            }
        }
    }
    if (arrayEnd == std::string_view::npos) {
        arrayEnd = json.size();
    }

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos || objStart >= arrayEnd) break;

        // Track brace depth to find matching '}' for current game object
        size_t objEnd = std::string_view::npos;
        int braceDepth = 0;
        inStr = false;
        escaped = false;
        for (size_t i = objStart; i < arrayEnd; ++i) {
            char c = json[i];
            if (escaped) { escaped = false; continue; }
            if (c == '\\' && inStr) { escaped = true; continue; }
            if (c == '"') { inStr = !inStr; continue; }
            if (inStr) continue;

            if (c == '{') {
                braceDepth++;
            } else if (c == '}') {
                braceDepth--;
                if (braceDepth == 0) {
                    objEnd = i;
                    break;
                }
            }
        }

        if (objEnd == std::string_view::npos || objEnd >= arrayEnd) break;

        std::string_view itemJson = json.substr(objStart, objEnd - objStart + 1);

        auto appIdOpt = GetUInt32(itemJson, "appid");
        auto nameOpt = GetString(itemJson, "name");
        auto excludeOpt = GetUInt32(itemJson, "exclude_reason");

        if (appIdOpt && *appIdOpt > 0) {
            // exclude_reason != 0 indicates developer or borrower restriction
            if (!excludeOpt.has_value() || *excludeOpt == 0) {
                OwnedGameInfo info;
                info.appId = *appIdOpt;
                info.name = nameOpt.value_or("");
                games.push_back(std::move(info));
            }
        }

        pos = objEnd + 1;
    }

    return games;
}

} // namespace OST::ExtractTickets
