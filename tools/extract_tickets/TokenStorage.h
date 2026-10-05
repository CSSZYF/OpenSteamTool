#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

struct CachedAccount {
    std::string accountName;
    uint64_t steamId{0};
    std::string refreshToken;
    int64_t lastLoginTime{0};
};

class TokenStorage {
public:
    // Loads all cached accounts from encrypted local storage
    [[nodiscard]] static std::vector<CachedAccount> LoadAccounts();

    // Saves list of accounts to encrypted local storage
    static bool SaveAccounts(const std::vector<CachedAccount>& accounts);

    // Inserts or updates an account entry
    static bool UpsertAccount(const CachedAccount& account);

    // Deletes single account by name (shortcut [d])
    static bool DeleteAccount(std::string_view accountName);

    // Thoroughly wipes and removes all credential cache files (shortcut [x])
    static bool WipeAll();

    // Returns the canonical path of the encrypted cache file
    [[nodiscard]] static std::string GetStorageFilePath();
};

} // namespace OST::ExtractTickets
