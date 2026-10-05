#include "GameListManager.h"
#include "Log.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace OST::ExtractTickets {

namespace {
    std::string EscapeCsv(std::string_view str) {
        bool needsQuotes = str.find_first_of(",\"\r\n") != std::string_view::npos;
        if (!needsQuotes) {
            return std::string{str};
        }

        std::string escaped = "\"";
        for (char c : str) {
            if (c == '"') {
                escaped += "\"\"";
            } else {
                escaped.push_back(c);
            }
        }
        escaped += "\"";
        return escaped;
    }
} // namespace

GameListManager::GameListManager(std::vector<OwnedGameInfo> games, size_t pageSize)
    : m_games(std::move(games)), m_pageSize(pageSize ? pageSize : 20), m_currentPage(0) {
}

size_t GameListManager::TotalPages() const noexcept {
    if (m_games.empty()) return 1;
    return (m_games.size() + m_pageSize - 1) / m_pageSize;
}

void GameListManager::PrintCurrentPage() const {
    const size_t total = m_games.size();
    const size_t totalP = TotalPages();
    const size_t startIdx = m_currentPage * m_pageSize;
    const size_t endIdx = (std::min)(startIdx + m_pageSize, total);

    std::cout << "\n======================================================================\n"
              << "  当前账号已拥有游戏列表 (共 " << total << " 款) [第 "
              << (m_currentPage + 1) << " / " << totalP << " 页]\n"
              << "======================================================================\n"
              << "序号    AppID       游戏名称 (Game Name)\n"
              << "----------------------------------------------------------------------\n";

    if (m_games.empty()) {
        std::cout << "  (当前账号游戏库为空或尚未同步到许可)\n";
    } else {
        for (size_t i = startIdx; i < endIdx; ++i) {
            const auto& game = m_games[i];
            std::cout << std::left
                      << "[" << std::setw(4) << (i + 1) << "] "
                      << std::setw(11) << game.appId << " "
                      << game.name << "\n";
        }
    }

    std::cout << "----------------------------------------------------------------------\n"
              << "[操作快捷指令] (单键直达 / 纯数字提取)\n"
              << "  - <纯数字 AppID> : 直接提取指定游戏凭证与 Lua (例如输入 1091500 回车)\n"
              << "  - a             : 批量提取全部游戏凭证 (需确认 [y/N]，默认回车为 N)\n"
              << "  - n / b         : 翻页 -> [n] 下一页 (Next) | [b] 上一页 (Back)\n"
              << "  - l             : 导出完整游戏列表表格 (需确认 [y/N]，默认回车为 N)\n"
              << "  - q             : 登出当前账号，返回上一层账号选择列表 (Level 2)\n"
              << "======================================================================\n";
}

bool GameListManager::NextPage() {
    if (m_currentPage + 1 < TotalPages()) {
        ++m_currentPage;
        return true;
    }
    return false;
}

bool GameListManager::PrevPage() {
    if (m_currentPage > 0) {
        --m_currentPage;
        return true;
    }
    return false;
}

bool GameListManager::JumpToPage(size_t pageNum) {
    if (pageNum >= 1 && pageNum <= TotalPages()) {
        m_currentPage = pageNum - 1;
        return true;
    }
    return false;
}

bool GameListManager::ExportCsv(std::string_view accountName, std::string* outPath) const {
    std::string filename = "gameslist-" + std::string{accountName} + ".csv";
    if (outPath) {
        *outPath = filename;
    }

    std::ofstream ofs(filename, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        LOG_ERROR("GameList", "无法创建表格文件: gameslist-{}.csv", MaskAccount(accountName));
        return false;
    }

    // Write UTF-8 BOM
    const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
    ofs.write(reinterpret_cast<const char*>(bom), sizeof(bom));

    // Write header
    ofs << "序号,AppID,游戏名称\n";

    for (size_t i = 0; i < m_games.size(); ++i) {
        const auto& g = m_games[i];
        ofs << (i + 1) << ',' << g.appId << ',' << EscapeCsv(g.name) << '\n';
    }

    ofs.flush();
    ofs.close();

    LOG_INFO("GameList", "成功导出表格至: gameslist-{}.csv (共 {} 款游戏)", MaskAccount(accountName), m_games.size());
    return true;
}

std::vector<OwnedGameInfo> GameListManager::GetPageItems(size_t pageIndex) const {
    const size_t total = m_games.size();
    const size_t startIdx = pageIndex * m_pageSize;
    if (startIdx >= total) return {};
    const size_t endIdx = (std::min)(startIdx + m_pageSize, total);
    return std::vector<OwnedGameInfo>(m_games.begin() + startIdx, m_games.begin() + endIdx);
}

} // namespace OST::ExtractTickets
