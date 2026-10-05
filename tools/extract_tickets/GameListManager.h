#pragma once

#include "JsonHelper.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

class GameListManager {
public:
    explicit GameListManager(std::vector<OwnedGameInfo> games, size_t pageSize = 20);

    [[nodiscard]] size_t TotalGames() const noexcept { return m_games.size(); }
    [[nodiscard]] size_t TotalPages() const noexcept;
    [[nodiscard]] size_t CurrentPage() const noexcept { return m_currentPage; }
    [[nodiscard]] size_t PageSize() const noexcept { return m_pageSize; }
    [[nodiscard]] const std::vector<OwnedGameInfo>& Games() const noexcept { return m_games; }
    [[nodiscard]] std::vector<OwnedGameInfo> GetPageItems(size_t pageIndex) const;

    // Renders the current page to the console with border and indices
    void PrintCurrentPage() const;

    // Navigates to next page (returns false if already on last page)
    bool NextPage();

    // Navigates to previous page (returns false if already on first page)
    bool PrevPage();

    // Jumps to specific 1-based page number
    bool JumpToPage(size_t pageNum);

    // Exports full game list to gameslist-<accountName>.csv with UTF-8 BOM
    [[nodiscard]] bool ExportCsv(std::string_view accountName, std::string* outPath = nullptr) const;

private:
    std::vector<OwnedGameInfo> m_games;
    size_t m_pageSize{20};
    size_t m_currentPage{0}; // 0-indexed
};

} // namespace OST::ExtractTickets
