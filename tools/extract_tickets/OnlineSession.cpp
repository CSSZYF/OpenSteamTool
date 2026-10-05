#include "OnlineSession.h"
#include "GameListManager.h"
#include "Log.h"
#include "OutputWriter.h"
#include "SteamAuthService.h"
#include "SteamCmClient.h"
#include "TokenStorage.h"
#include "Utils.h"

#include <conio.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

namespace OST::ExtractTickets {

namespace {
    std::string FormatTimestamp(int64_t t) {
        if (t <= 0) return "未知时间";
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm tmVal{};
#if defined(_WIN32)
        localtime_s(&tmVal, &tt);
#else
        localtime_r(&tt, &tmVal);
#endif
        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}",
                           tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday,
                           tmVal.tm_hour, tmVal.tm_min);
    }
} // namespace

int OnlineSession::RunInteractive() {
    RunAccountSelectionMenu();
    return 0;
}

void OnlineSession::RunAccountSelectionMenu() {
    SteamAuthService authService;

    while (true) {
        auto accounts = TokenStorage::LoadAccounts();

        // Menu items:
        // Index 0: [+] 登录新账号 (Log in to a new account) [默认]
        // Index 1 .. accounts.size(): Cached accounts
        // Index accounts.size() + 1: [x] 彻底粉碎全部账号缓存 (only if accounts is not empty)
        const size_t totalItems = 1 + accounts.size() + (accounts.empty() ? 0 : 1);
        size_t selected = 0; // Default to new account

        bool menuActive = true;
        while (menuActive) {
            std::cout << "\n======================================================================\n"
                      << "  Steam 在线凭据中心 / Steam Token Manager (Windows DPAPI 加密保护)\n"
                      << "======================================================================\n"
                      << "[安全提示] 所有凭据均采用 Windows DPAPI 内核级硬件绑定加密。\n"
                      << "           本地仅存储会话 Token，绝不持久化任何账号明文密码。\n"
                      << "[操作指南] ↑ / ↓: 移动选中 | Enter: 登录提取 | [d]: 删除选中账号 | [q] / ESC: 返回上一层\n\n"
                      << "请选择 Steam 账号 (默认登录新账号，按 Enter 直接输入；或按 ↓ 键选择已有缓存):\n";

            // 0: [+]
            std::cout << (selected == 0 ? " > " : "   ")
                      << "[+] 登录新账号 (Log in to a new account) [默认 / Default]\n";

            // 1 .. accounts.size()
            for (size_t i = 0; i < accounts.size(); ++i) {
                const auto& acc = accounts[i];
                const size_t itemIdx = i + 1;
                std::cout << (selected == itemIdx ? " > " : "   ")
                          << "[" << itemIdx << "] " << acc.accountName
                          << " (上次使用: " << FormatTimestamp(acc.lastLoginTime) << ") [DPAPI 加密保护]\n";
            }

            // Optional [x]
            if (!accounts.empty()) {
                const size_t wipeIdx = accounts.size() + 1;
                std::cout << (selected == wipeIdx ? " > " : "   ")
                          << "[x] 彻底粉碎全部账号缓存 (Wipe All Caches)\n";
            }

            std::cout << "\n请选择操作: ";
            std::cout.flush();

            // Wait for key
            int ch = _getch();
            if (ch == 0 || ch == 0xE0) {
                int arrow = _getch();
                if (arrow == 0x48) { // Up
                    selected = (selected > 0) ? (selected - 1) : (totalItems - 1);
                } else if (arrow == 0x50) { // Down
                    selected = (selected + 1 < totalItems) ? (selected + 1) : 0;
                }
                continue; // Redraw menu
            }

            if (ch == 'q' || ch == 'Q' || ch == 27) { // ESC or q -> Back to Level 1
                std::cout << "[q]\n[INFO] 返回主菜单。\n";
                return;
            }

            if (ch == 'd' || ch == 'D') {
                if (selected >= 1 && selected <= accounts.size()) {
                    const auto& targetAcc = accounts[selected - 1];
                    std::cout << "\n[确认] 确定删除账号 [" << targetAcc.accountName << "] 的本地登录缓存吗? [y/N]: ";
                    std::string confirm;
                    std::getline(std::cin, confirm);
                    confirm = std::string{TrimWhitespace(confirm)};
                    if (confirm == "y" || confirm == "Y") {
                        TokenStorage::DeleteAccount(targetAcc.accountName);
                        std::cout << "[OK] 已删除账号缓存。\n";
                        break; // Reload accounts & redraw menu
                    } else {
                        std::cout << "[INFO] 已取消删除。\n";
                    }
                }
                continue;
            }

            if (ch == 'x' || ch == 'X') {
                if (!accounts.empty()) {
                    std::cout << "\n[确认] 警告：确定要彻底粉碎全部本地账号凭据缓存吗? [y/N]: ";
                    std::string confirm;
                    std::getline(std::cin, confirm);
                    confirm = std::string{TrimWhitespace(confirm)};
                    if (confirm == "y" || confirm == "Y") {
                        TokenStorage::WipeAll();
                        std::cout << "[OK] 已粉碎全部本地缓存。\n";
                        break;
                    }
                }
                continue;
            }

            if (ch == '\r' || ch == '\n') { // Enter
                if (selected == 0) {
                    // Log in to a new account
                    std::cout << "\n\n[登录新账号]\n请输入 Steam 登录账号: ";
                    std::string accountName;
                    std::getline(std::cin, accountName);
                    accountName = std::string{TrimWhitespace(accountName)};
                    if (accountName.empty()) {
                        std::cerr << "[WARN] 账号名称不能为空。\n";
                        continue;
                    }

                    auto loginRes = authService.InteractiveLogin(accountName);
                    if (loginRes.success) {
                        RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.accessToken);
                    }
                    break; // Refresh menu after session ends
                } else if (selected >= 1 && selected <= accounts.size()) {
                    // Cached account
                    const auto& acc = accounts[selected - 1];
                    std::cout << "\n\n正在尝试使用本地 DPAPI 缓存凭据免密登录: " << acc.accountName << "...\n";
                    auto accessOpt = authService.RefreshAccessToken(acc.steamId, acc.refreshToken);
                    if (accessOpt) {
                        std::cout << "[OK] 免密授权校验通过！(SteamID: " << acc.steamId << ")\n";
                        RunInSessionExtraction(acc.accountName, acc.steamId, *accessOpt);
                    } else {
                        std::cout << "[WARN] 该账号的本地授权令牌已过期或已在手机端失效。\n"
                                  << "是否立即重新登录? [Y/n]: ";
                        std::string reloginConfirm;
                        std::getline(std::cin, reloginConfirm);
                        reloginConfirm = std::string{TrimWhitespace(reloginConfirm)};
                        if (reloginConfirm.empty() || reloginConfirm == "y" || reloginConfirm == "Y") {
                            auto loginRes = authService.InteractiveLogin(acc.accountName);
                            if (loginRes.success) {
                                RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.accessToken);
                            }
                        } else {
                            TokenStorage::DeleteAccount(acc.accountName);
                        }
                    }
                    break;
                } else if (!accounts.empty() && selected == accounts.size() + 1) {
                    // Wipe all selected
                    std::cout << "\n[确认] 确定要彻底粉碎全部本地账号凭据缓存吗? [y/N]: ";
                    std::string confirm;
                    std::getline(std::cin, confirm);
                    confirm = std::string{TrimWhitespace(confirm)};
                    if (confirm == "y" || confirm == "Y") {
                        TokenStorage::WipeAll();
                        std::cout << "[OK] 全部缓存已粉碎。\n";
                        break;
                    }
                }
            }
        }
    }
}

void OnlineSession::RunInSessionExtraction(
    const std::string& accountName,
    uint64_t steamId,
    const std::string& accessToken) {

    SteamAuthService authService;
    SteamCmClient cmClient;

    // Connect to CM
    if (!cmClient.ConnectAndLogon(steamId, accessToken)) {
        std::cerr << "[ERROR] 无法建立 Steam CM 会话，已返回账号选择菜单。\n";
        return;
    }

    std::cout << "[INFO] 正在同步账号已拥有的正版游戏列表...\n";
    auto games = authService.FetchOwnedGames(steamId, accessToken);
    std::cout << "[OK] 游戏列表同步成功！共检索到 " << games.size() << " 款游戏与许可。\n";

    GameListManager gameMgr(std::move(games), 20);
    gameMgr.PrintCurrentPage();

    while (true) {
        std::cout << "\n请输入指令 (AppID 或 a/n/b/l/q): ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        line = std::string{TrimWhitespace(line)};
        if (line.empty()) continue;

        // Command: q (Quit / Back to Level 2)
        if (line == "q" || line == "Q") {
            std::cout << "[q]\n[INFO] 正在登出当前账号会话，返回账号列表...\n";
            cmClient.Disconnect();
            break;
        }

        // Command: n (Next Page)
        if (line == "n" || line == "N") {
            if (gameMgr.NextPage()) {
                gameMgr.PrintCurrentPage();
            } else {
                std::cout << "[INFO] 当前已是最后一页 (第 " << (gameMgr.CurrentPage() + 1) << " 页)。\n";
            }
            continue;
        }

        // Command: b (Back Page)
        if (line == "b" || line == "B") {
            if (gameMgr.PrevPage()) {
                gameMgr.PrintCurrentPage();
            } else {
                std::cout << "[INFO] 当前已是第一页 (第 1 页)。\n";
            }
            continue;
        }

        // Command: l (Export List to CSV with confirmation)
        if (line == "l" || line == "L") {
            std::cout << "[确认] 确定要将当前账号全部 " << gameMgr.TotalGames() << " 款游戏导出为本地表格吗? [y/N]: ";
            std::string confirm;
            std::getline(std::cin, confirm);
            confirm = std::string{TrimWhitespace(confirm)};
            if (confirm == "y" || confirm == "Y") {
                std::string outPath;
                if (gameMgr.ExportCsv(accountName, &outPath)) {
                    std::cout << "[OK] 导出成功！表格已保存为: " << outPath
                              << " (UTF-8 带 BOM，Excel 可直接双击整齐打开)\n";
                }
            } else {
                std::cout << "[INFO] 用户已取消表格导出操作。\n";
            }
            continue;
        }

        // Command: a (Batch Extract All with confirmation)
        if (line == "a" || line == "A") {
            std::cout << "[确认] 确定要批量提取当前账号全部 " << gameMgr.TotalGames() << " 款游戏的全部凭证吗? [y/N]: ";
            std::string confirm;
            std::getline(std::cin, confirm);
            confirm = std::string{TrimWhitespace(confirm)};
            if (confirm == "y" || confirm == "Y") {
                std::cout << "\n[批量提取] 流水线已启动 (共 " << gameMgr.TotalGames() << " 款游戏)...\n";
                size_t progress = 0;
                size_t succeeded = 0;

                for (const auto& game : gameMgr.Games()) {
                    ++progress;
                    std::cout << "\n[" << progress << "/" << gameMgr.TotalGames() << "] 正在提取 "
                              << game.name << " (AppID: " << game.appId << ")...\n";

                    auto creds = cmClient.ExtractFullCredentials(game.appId);
                    if (WriteOutputs(game.appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                     creds.depotKeys, creds.dlcs, creds.appTokens)) {
                        ++succeeded;
                    }

                    // Gentle rate pacing to protect against Steam CM throttling
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                std::cout << "\n======================================================================\n"
                          << "[SUCCESS] 批量提取完成！成功处理 " << succeeded << " / " << gameMgr.TotalGames() << " 款游戏。\n"
                          << "======================================================================\n";
            } else {
                std::cout << "[INFO] 用户已取消批量提取操作。\n";
            }
            continue;
        }

        // Command: numeric AppID
        if (IsDecimal(line)) {
            auto appIdOpt = ParseAppId(line);
            if (appIdOpt && *appIdOpt > 0) {
                const uint32_t appId = *appIdOpt;
                std::cout << "\n[1/1] 正在为目标游戏 AppID " << appId << " 提取正版凭据...\n";
                auto creds = cmClient.ExtractFullCredentials(appId);
                if (WriteOutputs(appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                 creds.depotKeys, creds.dlcs, creds.appTokens)) {
                    std::cout << "[SUCCESS] 凭证与 Lua 提取完成！文件已就绪在目录 ./" << appId << "/\n";
                }
                continue;
            }
        }

        std::cout << "[WARN] 未知指令 '" << line << "'。请输入纯数字 AppID 或快捷键 (a/n/b/l/q)。\n";
    }
}

} // namespace OST::ExtractTickets
