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
        std::string pattern = "\"" + std::string(key) + "\"";
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

std::optional<bool> JsonHelper::GetBool(std::string_view json, std::string_view key) {
    size_t valPos = FindKeyPosition(json, key);
    if (valPos == std::string_view::npos) return std::nullopt;

    std::string_view sv = SkipWhitespace(json.substr(valPos));
    if (sv.starts_with("true")) return true;
    if (sv.starts_with("false")) return false;
    return std::nullopt;
}

std::vector<int> JsonHelper::GetConfirmationTypes(std::string_view json) {
    std::vector<int> types;
    size_t pos = 0;
    std::string_view target = "\"confirmation_type\"";

    while ((pos = json.find(target, pos)) != std::string_view::npos) {
        size_t afterKey = pos + target.size();
        afterKey = json.find_first_not_of(" \t\r\n", afterKey);
        if (afterKey != std::string_view::npos && json[afterKey] == ':') {
            std::string_view sv = SkipWhitespace(json.substr(afterKey + 1));
            size_t endNum = sv.find_first_of(" \t\r\n,}]");
            if (endNum != std::string_view::npos) {
                int cType = 0;
                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + endNum, cType);
                if (ec == std::errc()) {
                    types.push_back(cType);
                }
            }
        }
        pos += target.size();
    }
    return types;
}

std::vector<OwnedGameInfo> JsonHelper::ParseOwnedGames(std::string_view json) {
    std::vector<OwnedGameInfo> games;

    size_t gamesPos = json.find("\"games\"");
    if (gamesPos == std::string_view::npos) return games;

    size_t arrayStart = json.find('[', gamesPos);
    if (arrayStart == std::string_view::npos) return games;

    size_t pos = arrayStart + 1;
    while (pos < json.size()) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos) break;

        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string_view::npos) break;

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

} // namespace OST::ExtractTickets
