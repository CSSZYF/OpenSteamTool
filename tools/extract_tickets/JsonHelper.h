#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

struct OwnedGameInfo {
    uint32_t appId{0};
    std::string name;
};

class JsonHelper {
public:
    // Extracts string value for a given key: "key": "value"
    [[nodiscard]] static std::optional<std::string> GetString(std::string_view json, std::string_view key);

    // Extracts 64-bit unsigned integer value for a given key: "key": 123456 or "key": "123456"
    [[nodiscard]] static std::optional<uint64_t> GetUInt64(std::string_view json, std::string_view key);

    // Extracts 32-bit unsigned integer value
    [[nodiscard]] static std::optional<uint32_t> GetUInt32(std::string_view json, std::string_view key);

    // Extracts boolean value: "key": true / false
    [[nodiscard]] static std::optional<bool> GetBool(std::string_view json, std::string_view key);

    // Extracts confirmation_types from "allowed_confirmations": [ { "confirmation_type": 2 }, ... ]
    [[nodiscard]] static std::vector<int> GetConfirmationTypes(std::string_view json);

    // Parses games array from GetOwnedGames response:
    // "games": [ { "appid": 730, "name": "Counter-Strike 2" }, ... ]
    [[nodiscard]] static std::vector<OwnedGameInfo> ParseOwnedGames(std::string_view json);
};

} // namespace OST::ExtractTickets
