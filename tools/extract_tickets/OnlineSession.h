#pragma once

#include <cstdint>
#include <string>

namespace OST::ExtractTickets {

class OnlineSession {
public:
    // Starts the interactive online mode (Level 2 -> Level 3 flow)
    // Returns 0 on normal exit, non-zero on critical error
    static int RunInteractive();

private:
    // Level 2: Account Selection and Management Menu
    static void RunAccountSelectionMenu();

    // Level 3: Inside a logged-in account (games list, pagination, extraction)
    static void RunInSessionExtraction(
        const std::string& accountName,
        uint64_t steamId,
        const std::string& refreshToken,
        const std::string& accessToken);
};

} // namespace OST::ExtractTickets
