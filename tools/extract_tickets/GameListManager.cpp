#include "GameListManager.h"
#include "I18n.h"
#include "Log.h"

#include <algorithm>
#include <filesystem>
#include <format>
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

bool GameListManager::ExportCsv(std::string_view accountName, std::string* outPath) const {
    std::string safeName{accountName};
    for (char& c : safeName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    std::string filename = "gameslist-" + safeName + ".csv";
    if (outPath) {
        *outPath = filename;
    }

    std::filesystem::path filePath(filename);
    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        LOG_ERROR("GameList", "无法创建表格文件: {}", filePath.string());
        return false;
    }

    // Write UTF-8 BOM
    const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
    ofs.write(reinterpret_cast<const char*>(bom), sizeof(bom));

    // Write header
    ofs << std::format("{},{},{},{}\n", TR(MsgKey::CsvIndex), TR(MsgKey::CsvAppId), TR(MsgKey::CsvName), TR(MsgKey::CsvType));

    for (size_t i = 0; i < m_games.size(); ++i) {
        const auto& g = m_games[i];
        ofs << (i + 1) << ',' << g.appId << ',' << EscapeCsv(g.name) << ','
            << (g.isShared ? TR(MsgKey::CsvShared) : TR(MsgKey::CsvOwned)) << '\n';
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
