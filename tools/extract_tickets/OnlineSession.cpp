#include "OnlineSession.h"
#include "GameListManager.h"
#include "Log.h"
#include "OutputWriter.h"
#include "SteamAuthService.h"
#include "SteamCmClient.h"
#include "TokenStorage.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <chrono>
#include <format>
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

    void RenderLevel2Tui(const std::vector<CachedAccount>& accounts, size_t selected) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        TuiEngine::ClearScreen();

        TuiEngine::DrawHeader("Steam 在线凭据中心 (Windows DPAPI 内核级硬件保护)",
                              std::format("已保存 {} 个账号", accounts.size()));

        const int boxW = std::clamp(w - 4, 76, 110);
        const int boxH = std::clamp(h - 4, 18, 26);
        const int top = 2;
        const int left = (w - boxW) / 2;

        TuiEngine::DrawBox(top, left, boxW, boxH, "请选择 Steam 账号进行在线提取");

        // Subtitle explanation
        TuiEngine::MoveCursor(top + 2, left + 4);
        std::cout << "\x1b[90m所有凭据均采用 Windows DPAPI 硬件绑定加密，仅保存会话 Token，绝不持久化任何明文密码。\x1b[0m";

        // Item 0: [+] 登录新账号 (Default)
        TuiEngine::MoveCursor(top + 4, left + 4);
        if (selected == 0) {
            std::cout << "\x1b[1;30;46m > [+] 登录新账号 (Log in to a new account) [默认 / Enter 直接输入] \x1b[0m";
        } else {
            std::cout << "\x1b[1;37m   [+] 登录新账号 (Log in to a new account) [默认 / Enter 直接输入]\x1b[0m";
        }

        // Cached accounts
        for (size_t i = 0; i < accounts.size(); ++i) {
            const auto& acc = accounts[i];
            const size_t itemIdx = i + 1;
            TuiEngine::MoveCursor(top + 5 + static_cast<int>(i), left + 4);

            std::string lineText = std::format("   [{}] {}  (上次登录: {}) [DPAPI 加密有效]",
                                               itemIdx, acc.accountName, FormatTimestamp(acc.lastLoginTime));
            if (selected == itemIdx) {
                lineText[1] = '>';
                std::cout << "\x1b[1;30;46m" << TuiEngine::Pad(lineText, static_cast<size_t>(boxW - 8)) << "\x1b[0m";
            } else {
                std::cout << "\x1b[37m" << lineText << "\x1b[0m";
            }
        }

        // Optional wipe all
        if (!accounts.empty()) {
            const size_t wipeIdx = accounts.size() + 1;
            TuiEngine::MoveCursor(top + 6 + static_cast<int>(accounts.size()), left + 4);
            std::string wipeText = "   [x] 彻底粉碎全部账号缓存 (Wipe All Caches)";
            if (selected == wipeIdx) {
                wipeText[1] = '>';
                std::cout << "\x1b[1;37;41m" << TuiEngine::Pad(wipeText, static_cast<size_t>(boxW - 8)) << "\x1b[0m";
            } else {
                std::cout << "\x1b[91m" << wipeText << "\x1b[0m";
            }
        }

        // Bottom help card
        TuiEngine::MoveCursor(top + boxH - 4, left + 4);
        std::cout << "\x1b[90m" << std::string(boxW - 8, '-') << "\x1b[0m";
        TuiEngine::MoveCursor(top + boxH - 3, left + 4);
        if (selected == 0) {
            std::cout << "\x1b[33m提示: 按 Enter 即可输入新账号、密码及 2FA 动态码登录；登录后自动保存加密缓存。\x1b[0m";
        } else if (selected <= accounts.size()) {
            std::cout << "\x1b[32m提示: 按 Enter 立即免密授权登录；按 [D] 删除选中缓存；按 [Q/ESC] 返回首页。\x1b[0m";
        } else {
            std::cout << "\x1b[31m提示: 危险操作！按 Enter 将执行零填充覆写粉碎本地所有已保存的账号凭据。\x1b[0m";
        }

        TuiEngine::DrawFooter("[↑/↓] 移动光标   [Enter] 确认选择   [D] 删除选中账号   [Q/ESC] 返回首页");
    }

    void RenderLevel3Tui(const GameListManager& gameMgr,
                         size_t selectedRow,
                         std::string_view inputAppId,
                         std::string_view accountName,
                         uint64_t steamId) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        TuiEngine::ClearScreen();

        std::string title = std::format("Steam 账号: {} (SteamID: {})", accountName, steamId);
        std::string tag = std::format("第 {} / {} 页 (共 {} 款游戏)",
                                      gameMgr.CurrentPage() + 1,
                                      gameMgr.TotalPages(),
                                      gameMgr.TotalGames());
        TuiEngine::DrawHeader(title, tag);

        const int boxW = std::clamp(w - 4, 76, 120);
        const int boxH = std::clamp(h - 4, 22, 28);
        const int top = 2;
        const int left = (w - boxW) / 2;

        TuiEngine::DrawBox(top, left, boxW, boxH, "已拥有正版游戏列表 (支持纯数字 AppID 提取与翻页)");

        // Table header
        const size_t nameColWidth = (boxW > 36) ? static_cast<size_t>(boxW - 32) : 20;
        TuiEngine::MoveCursor(top + 2, left + 2);
        std::string nameTitle = TuiEngine::Pad("游戏名称 (Game Title)", nameColWidth);
        std::string hdr = std::format(" {:<4} │ {:<10} │ {} │ {:<6} ",
                                      "序号", "AppID", nameTitle, "状态");
        std::cout << "\x1b[1;37;44m" << hdr << "\x1b[0m";

        TuiEngine::MoveCursor(top + 3, left + 2);
        std::cout << "\x1b[90m" << std::string(boxW - 4, '-') << "\x1b[0m";

        // Rows
        const auto pageGames = gameMgr.GetPageItems(gameMgr.CurrentPage());
        const size_t startIndex = gameMgr.CurrentPage() * gameMgr.PageSize();

        for (size_t r = 0; r < 20; ++r) {
            TuiEngine::MoveCursor(top + 4 + static_cast<int>(r), left + 2);
            if (r < pageGames.size()) {
                const auto& g = pageGames[r];
                const size_t globalIdx = startIndex + r + 1;
                std::string truncatedName = TuiEngine::TruncateToWidth(g.name, nameColWidth);
                std::string paddedName = TuiEngine::Pad(truncatedName, nameColWidth);

                std::string rowStr = std::format(" {:>3}  │ {:<10} │ {} │ 就绪   ",
                                                 globalIdx, g.appId, paddedName);
                if (r == selectedRow) {
                    rowStr[0] = '>';
                    std::cout << "\x1b[1;30;46m" << rowStr << "\x1b[0m";
                } else {
                    std::cout << "\x1b[37m" << rowStr << "\x1b[0m";
                }
            } else {
                std::cout << std::string(boxW - 4, ' ');
            }
        }

        // Direct AppID input box line
        TuiEngine::MoveCursor(top + boxH - 3, left + 2);
        std::cout << "\x1b[90m" << std::string(boxW - 4, '-') << "\x1b[0m";

        TuiEngine::MoveCursor(top + boxH - 2, left + 4);
        std::cout << "\x1b[1;37m[目标 AppID 快速提取]: \x1b[1;30;47m[ "
                  << std::format("{:<12}", std::string{inputAppId} + "_")
                  << " ]\x1b[0m  \x1b[90m(直接输入纯数字回车，或回车提取高亮选中项)\x1b[0m";

        TuiEngine::DrawFooter("[Enter] 提取选中/输入   [A] 批量提取全部   [L] 导出CSV表格   [N/B/←/→] 翻页   [Q] 登出");
    }

    void UpdateLevel3Input(int top, int left, int boxH, std::string_view inputAppId) {
        TuiEngine::MoveCursor(top + boxH - 2, left + 4);
        std::cout << "\x1b[1;37m[目标 AppID 快速提取]: \x1b[1;30;47m[ "
                  << std::format("{:<12}", std::string{inputAppId} + "_")
                  << " ]\x1b[0m  \x1b[90m(直接输入纯数字回车，或回车提取高亮选中项)\x1b[0m   ";
        std::cout.flush();
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
        const size_t totalItems = 1 + accounts.size() + (accounts.empty() ? 0 : 1);
        size_t selected = 0; // Default to new account

        bool menuActive = true;
        while (menuActive) {
            RenderLevel2Tui(accounts, selected);

            KeyEvent ev = TuiEngine::ReadKey();

            // Arrow keys
            if (ev.code == KeyCode::Up) {
                selected = (selected > 0) ? (selected - 1) : (totalItems - 1);
                continue;
            }
            if (ev.code == KeyCode::Down) {
                selected = (selected + 1 < totalItems) ? (selected + 1) : 0;
                continue;
            }

            // ESC or q -> Back to Level 1
            if (ev.code == KeyCode::Escape ||
                (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q'))) {
                return;
            }

            // 'd' or Delete -> Delete selected account
            if (ev.code == KeyCode::Delete ||
                (ev.code == KeyCode::Char && (ev.ch == 'd' || ev.ch == 'D'))) {
                if (selected >= 1 && selected <= accounts.size()) {
                    const auto& targetAcc = accounts[selected - 1];
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        "删除账号凭据",
                        std::format("确定删除账号 [{}] 的本地登录缓存吗?", targetAcc.accountName),
                        "删除后将无法免密登录该账号",
                        false);
                    if (confirmed) {
                        TokenStorage::DeleteAccount(targetAcc.accountName);
                        break; // Reload accounts & redraw menu
                    }
                }
                continue;
            }

            // 'x' -> Wipe all
            if (ev.code == KeyCode::Char && (ev.ch == 'x' || ev.ch == 'X')) {
                if (!accounts.empty()) {
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        "粉碎全部凭据",
                        "警告：确定要彻底粉碎全部本地账号凭据缓存吗?",
                        "本地所有账号的 DPAPI 加密凭据都将被安全抹除",
                        false);
                    if (confirmed) {
                        TokenStorage::WipeAll();
                        break;
                    }
                }
                continue;
            }

            // Enter key
            if (ev.code == KeyCode::Enter) {
                if (selected == 0) {
                    // Log in to a new account
                    auto accName = TuiEngine::PromptInputModal("Steam 账号登录", "请输入 Steam 登录账号:", "");
                    if (!accName || accName->empty()) {
                        continue;
                    }
                    auto pwdStr = TuiEngine::PromptInputModal("Steam 账号登录", "请输入 Steam 密码 (掩码保护):", "", true);
                    if (!pwdStr || pwdStr->empty()) {
                        continue;
                    }

                    SecureString secPwd(*pwdStr);
                    SecureZeroMemory(pwdStr->data(), pwdStr->size());

                    auto loginRes = authService.LoginWithCredentials(*accName, secPwd);
                    secPwd.Clear();

                    if (loginRes.success) {
                        RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.accessToken);
                    } else {
                        TuiEngine::ShowMessageModal("登录失败",
                                                    loginRes.errorMessage.empty() ? "账号或密码错误" : loginRes.errorMessage,
                                                    "请检查网络连接及动态验证码输入");
                    }
                    break;
                } else if (selected >= 1 && selected <= accounts.size()) {
                    // Cached account
                    const auto& acc = accounts[selected - 1];
                    std::string activeToken = acc.accessToken;
                    if (activeToken.empty()) {
                        auto accessOpt = authService.RefreshAccessToken(acc.steamId, acc.refreshToken);
                        if (accessOpt) {
                            activeToken = *accessOpt;
                            CachedAccount updated = acc;
                            updated.accessToken = activeToken;
                            TokenStorage::UpsertAccount(updated);
                        }
                    }

                    if (!activeToken.empty()) {
                        RunInSessionExtraction(acc.accountName, acc.steamId, activeToken);
                    } else {
                        bool relogin = TuiEngine::ShowConfirmModal(
                            "授权令牌失效",
                            std::format("账号 [{}] 的本地授权令牌已过期或失效", acc.accountName),
                            "是否立即重新输入密码登录该账号?",
                            true);
                        if (relogin) {
                            auto pwdStr = TuiEngine::PromptInputModal("重新登录", std::format("请输入账号 [{}] 的密码:", acc.accountName), "", true);
                            if (pwdStr && !pwdStr->empty()) {
                                SecureString secPwd(*pwdStr);
                                SecureZeroMemory(pwdStr->data(), pwdStr->size());
                                auto loginRes = authService.LoginWithCredentials(acc.accountName, secPwd);
                                secPwd.Clear();
                                if (loginRes.success) {
                                    RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.accessToken);
                                } else {
                                    TuiEngine::ShowMessageModal("登录失败", loginRes.errorMessage);
                                }
                            }
                        } else {
                            TokenStorage::DeleteAccount(acc.accountName);
                        }
                    }
                    break;
                } else if (!accounts.empty() && selected == accounts.size() + 1) {
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        "粉碎全部凭据",
                        "警告：确定要彻底粉碎全部本地账号凭据缓存吗?",
                        "本地所有账号的 DPAPI 加密凭据都将被安全抹除",
                        false);
                    if (confirmed) {
                        TokenStorage::WipeAll();
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

    int w = 80, h = 25;
    TuiEngine::GetScreenSize(w, h);
    const int modalW = 64, modalH = 6;
    const int top = (h - modalH) / 2, left = (w - modalW) / 2;
    TuiEngine::DrawBox(top, left, modalW, modalH, "Steam 正在接入网关");
    TuiEngine::MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;36m正在建立安全 WebSocket CM 会话 (wss://cm.steampowered.com/cmsocket/)...\x1b[0m";
    std::cout.flush();

    if (!cmClient.ConnectAndLogon(steamId, accessToken)) {
        TuiEngine::ShowMessageModal("连接失败", "无法建立 Steam CM WebSocket 会话", "请检查网络或代理连接");
        return;
    }

    TuiEngine::MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;32m登录成功！正在同步当前账号拥有的正版游戏列表...\x1b[0m          ";
    std::cout.flush();

    auto games = authService.FetchOwnedGames(steamId, accessToken);
    if (games.empty()) {
        TuiEngine::ShowMessageModal("游戏库同步", "未找到拥有的游戏列表或网络请求失败");
        return;
    }

    GameListManager gameMgr(std::move(games), 20);
    size_t selectedRow = 0;
    std::string inputAppId;

    while (true) {
        RenderLevel3Tui(gameMgr, selectedRow, inputAppId, accountName, steamId);

        KeyEvent ev = TuiEngine::ReadKey();

        // Up / Down
        if (ev.code == KeyCode::Up) {
            const auto pageItems = gameMgr.GetPageItems(gameMgr.CurrentPage());
            if (!pageItems.empty()) {
                selectedRow = (selectedRow > 0) ? (selectedRow - 1) : (pageItems.size() - 1);
            }
            continue;
        }
        if (ev.code == KeyCode::Down) {
            const auto pageItems = gameMgr.GetPageItems(gameMgr.CurrentPage());
            if (!pageItems.empty()) {
                selectedRow = (selectedRow + 1 < pageItems.size()) ? (selectedRow + 1) : 0;
            }
            continue;
        }

        // Left / 'b' / 'B' -> Previous Page
        if (ev.code == KeyCode::Left ||
            (ev.code == KeyCode::Char && (ev.ch == 'b' || ev.ch == 'B'))) {
            if (gameMgr.PrevPage()) {
                selectedRow = 0;
            }
            continue;
        }

        // Right / 'n' / 'N' -> Next Page
        if (ev.code == KeyCode::Right ||
            (ev.code == KeyCode::Char && (ev.ch == 'n' || ev.ch == 'N'))) {
            if (gameMgr.NextPage()) {
                selectedRow = 0;
            }
            continue;
        }

        // 'l' / 'L' -> Export CSV
        if (ev.code == KeyCode::Char && (ev.ch == 'l' || ev.ch == 'L')) {
            bool confirmed = TuiEngine::ShowConfirmModal(
                "导出游戏列表表格",
                std::format("确定要将当前账号全部 {} 款游戏导出为本地表格吗?", gameMgr.TotalGames()),
                std::format("导出文件: gameslist-{}.csv", accountName),
                false);
            if (confirmed) {
                std::string outPath;
                if (gameMgr.ExportCsv(accountName, &outPath)) {
                    TuiEngine::ShowMessageModal(
                        "导出成功",
                        std::format("表格已保存为: {}", outPath),
                        "采用 UTF-8 带 BOM 编码，微软 Excel / WPS 可直接双击整齐浏览");
                }
            }
            continue;
        }

        // 'a' / 'A' -> Batch extract all
        if (ev.code == KeyCode::Char && (ev.ch == 'a' || ev.ch == 'A')) {
            bool confirmed = TuiEngine::ShowConfirmModal(
                "批量提取凭证",
                std::format("确定要批量提取当前账号全部 {} 款游戏的全部凭证吗?", gameMgr.TotalGames()),
                "流水线将自动提取票据、密钥与Lua配置 (温和限速防止风控)",
                false);
            if (confirmed) {
                int w = 80, h = 25;
                TuiEngine::GetScreenSize(w, h);
                const int modalW = std::clamp(w - 12, 60, 90);
                const int modalH = 8;
                const int top = (h - modalH) / 2;
                const int left = (w - modalW) / 2;

                TuiEngine::DrawBox(top, left, modalW, modalH, "流水线批量提取中");

                size_t progress = 0;
                size_t succeeded = 0;
                const size_t total = gameMgr.TotalGames();

                for (const auto& game : gameMgr.Games()) {
                    ++progress;
                    TuiEngine::DrawProgressBar(top + 3, left + 4, modalW - 8, progress, total, game.name);

                    auto creds = cmClient.ExtractFullCredentials(game.appId);
                    if (WriteOutputs(game.appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                     creds.depotKeys, creds.dlcs, creds.appTokens)) {
                        ++succeeded;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                TuiEngine::ShowMessageModal(
                    "批量提取完成",
                    std::format("成功处理 {} / {} 款游戏凭证！", succeeded, total),
                    "所有配置与 Lua 已生成在各自 AppID 目录下");
            }
            continue;
        }

        // Digits 0-9 -> Add to AppID input box
        if (ev.code == KeyCode::Char && ev.ch >= '0' && ev.ch <= '9') {
            if (inputAppId.size() < 10) {
                inputAppId.push_back(ev.ch);
                int w = 80, h = 25;
                TuiEngine::GetScreenSize(w, h);
                const int boxH = std::clamp(h - 4, 22, 28);
                const int top = 2;
                const int left = (w - std::clamp(w - 4, 76, 120)) / 2;
                UpdateLevel3Input(top, left, boxH, inputAppId);
            }
            continue;
        }

        // Backspace
        if (ev.code == KeyCode::Backspace) {
            if (!inputAppId.empty()) {
                inputAppId.pop_back();
                int w = 80, h = 25;
                TuiEngine::GetScreenSize(w, h);
                const int boxH = std::clamp(h - 4, 22, 28);
                const int top = 2;
                const int left = (w - std::clamp(w - 4, 76, 120)) / 2;
                UpdateLevel3Input(top, left, boxH, inputAppId);
            }
            continue;
        }

        // Enter -> Extract target AppID
        if (ev.code == KeyCode::Enter) {
            uint32_t targetAppId = 0;
            std::string gameName;

            if (!inputAppId.empty()) {
                auto parsed = ParseAppId(inputAppId);
                if (parsed) {
                    targetAppId = *parsed;
                    gameName = std::format("AppID {}", targetAppId);
                }
                inputAppId.clear();
            } else {
                const auto pageItems = gameMgr.GetPageItems(gameMgr.CurrentPage());
                if (selectedRow < pageItems.size()) {
                    targetAppId = pageItems[selectedRow].appId;
                    gameName = pageItems[selectedRow].name;
                }
            }

            if (targetAppId > 0) {
                // Show extracting modal
                int w = 80, h = 25;
                TuiEngine::GetScreenSize(w, h);
                const int modalW = 60, modalH = 6;
                const int top = (h - modalH) / 2, left = (w - modalW) / 2;
                TuiEngine::DrawBox(top, left, modalW, modalH, "提取凭据中");
                TuiEngine::MoveCursor(top + 2, left + 4);
                std::cout << "\x1b[1;33m正在向 Steam CM 请求 " << TuiEngine::TruncateToWidth(gameName, 40)
                          << " 的正版票据与密钥...\x1b[0m";
                std::cout.flush();

                auto creds = cmClient.ExtractFullCredentials(targetAppId);
                bool ok = WriteOutputs(targetAppId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                       creds.depotKeys, creds.dlcs, creds.appTokens);
                if (ok) {
                    TuiEngine::ShowMessageModal(
                        "提取完成",
                        std::format("游戏 [{}] 凭证与 Lua 提取成功！", targetAppId),
                        std::format("文件已保存至 ./{}/ 目录中", targetAppId));
                } else {
                    TuiEngine::ShowMessageModal(
                        "提取警告",
                        std::format("AppID {} 提取完成，部分输出可能受限", targetAppId));
                }
            }
            continue;
        }

        // 'q' / 'Q' or ESC -> Return to Level 2
        if (ev.code == KeyCode::Escape ||
            (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q'))) {
            cmClient.Disconnect();
            return;
        }
    }
}

} // namespace OST::ExtractTickets
