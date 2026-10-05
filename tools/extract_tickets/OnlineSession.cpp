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

    void RenderLevel2Tui(const std::vector<CachedAccount>& accounts, size_t selected, bool fullClear = false) {
        if (!TuiEngine::EnsureMinTerminalSize(76, 18)) {
            return;
        }
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        if (fullClear) {
            TuiEngine::ClearScreen();
        } else {
            TuiEngine::RepositionCursor();
        }

        TuiEngine::DrawHeader("Steam 在线凭据中心 (Windows DPAPI 内核级硬件保护)",
                              std::format("已保存 {} 个账号", accounts.size()));

        const int boxW = std::clamp(w - 4, 70, 110);
        const int boxH = std::clamp(h - 4, 16, 26);
        const int top = (std::max)(1, (h - boxH) / 2);
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, "请选择 Steam 账号进行在线提取");

        const size_t innerW = static_cast<size_t>(boxW - 8);

        // Subtitle explanation
        TuiEngine::PrintBounded(top + 2, left + 4,
            "所有凭据均采用 Windows DPAPI 加密保护，仅保存 Token，不持久化明文密码。",
            innerW, "\x1b[90m");

        // Item 0: [+] 登录新账号 (Default)
        std::string item0 = "   [+] 登录新账号 (Log in to a new account) [默认 / Enter]";
        if (selected == 0) {
            item0[1] = '>';
            TuiEngine::PrintBounded(top + 4, left + 4, item0, innerW, "\x1b[30;107m");
        } else {
            TuiEngine::PrintBounded(top + 4, left + 4, item0, innerW, "\x1b[1;37m");
        }

        // Cached accounts
        for (size_t i = 0; i < accounts.size(); ++i) {
            const auto& acc = accounts[i];
            const size_t itemIdx = i + 1;
            std::string lineText = std::format("   [{}] {}  (上次登录: {}) [DPAPI有效]",
                                               itemIdx, acc.accountName, FormatTimestamp(acc.lastLoginTime));
            if (selected == itemIdx) {
                lineText[1] = '>';
                TuiEngine::PrintBounded(top + 5 + static_cast<int>(i), left + 4, lineText, innerW, "\x1b[30;107m");
            } else {
                TuiEngine::PrintBounded(top + 5 + static_cast<int>(i), left + 4, lineText, innerW, "\x1b[37m");
            }
        }

        // Optional wipe all
        if (!accounts.empty()) {
            const size_t wipeIdx = accounts.size() + 1;
            std::string wipeText = "   [x] 彻底粉碎全部账号缓存 (Wipe All Caches)";
            if (selected == wipeIdx) {
                wipeText[1] = '>';
                TuiEngine::PrintBounded(top + 6 + static_cast<int>(accounts.size()), left + 4, wipeText, innerW, "\x1b[1;37;41m");
            } else {
                TuiEngine::PrintBounded(top + 6 + static_cast<int>(accounts.size()), left + 4, wipeText, innerW, "\x1b[91m");
            }
        }

        // Bottom help card
        TuiEngine::MoveCursor(top + boxH - 4, left + 4);
        std::cout << "\x1b[90m" << std::string(innerW, '-') << "\x1b[0m";

        std::string tip;
        std::string tipStyle = "\x1b[32m";
        if (selected == 0) {
            tip = "提示: 按 Enter 即可输入新账号登录；登录后自动保存加密凭证。";
            tipStyle = "\x1b[33m";
        } else if (selected <= accounts.size()) {
            tip = "提示: 按 Enter 免密授权登录；按 [D] 删除缓存；按 [Q/ESC] 返回首页。";
            tipStyle = "\x1b[32m";
        } else {
            tip = "提示: 危险操作！按 Enter 将执行零填充覆写粉碎本地所有已保存凭据。";
            tipStyle = "\x1b[31m";
        }
        TuiEngine::PrintBounded(top + boxH - 3, left + 4, tip, innerW, tipStyle);

        TuiEngine::DrawFooter("[↑/↓] 移动光标   [Enter] 确认选择   [D] 删除选中账号   [Q/ESC] 返回首页");
    }

    void RenderLevel3Tui(const GameListManager& gameMgr,
                         size_t selectedRow,
                         std::string_view inputAppId,
                         std::string_view accountName,
                         uint64_t steamId,
                         bool fullClear = false) {
        if (!TuiEngine::EnsureMinTerminalSize(76, 18)) {
            return;
        }
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        if (fullClear) {
            TuiEngine::ClearScreen();
        } else {
            TuiEngine::RepositionCursor();
        }

        std::string title = std::format("Steam 账号: {} (SteamID: {})", accountName, steamId);
        std::string tag = std::format("第 {} / {} 页 (共 {} 款游戏)",
                                      gameMgr.CurrentPage() + 1,
                                      gameMgr.TotalPages(),
                                      gameMgr.TotalGames());
        TuiEngine::DrawHeader(title, tag);

        const int boxW = std::clamp(w - 4, 70, 120);
        const int boxH = std::clamp(h - 4, 16, 28);
        const int top = (std::max)(1, (h - boxH) / 2);
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, "已拥有正版游戏列表 (支持纯数字 AppID 提取与翻页)");

        const int innerW = boxW - 4;
        const size_t nameColWidth = (innerW > 34) ? static_cast<size_t>(innerW - 32) : 18;

        // Table header
        TuiEngine::MoveCursor(top + 2, left + 2);
        std::string nameTitle = TuiEngine::Pad("游戏名称 (Game Title)", nameColWidth);
        std::string hdr = std::format(" {:>4} │ {:<10} │ {} │ {:^6} ",
                                      "序号", "AppID", nameTitle, "状态");
        hdr = TuiEngine::Pad(hdr, static_cast<size_t>(innerW));
        std::cout << "\x1b[1;37;44m" << hdr << "\x1b[0m";

        TuiEngine::MoveCursor(top + 3, left + 2);
        std::cout << "\x1b[90m" << std::string(innerW, '-') << "\x1b[0m";

        // Rows
        const auto pageGames = gameMgr.GetPageItems(gameMgr.CurrentPage());
        const size_t startIndex = gameMgr.CurrentPage() * gameMgr.PageSize();
        const size_t maxVisibleRows = static_cast<size_t>((std::max)(5, boxH - 8));

        for (size_t r = 0; r < maxVisibleRows; ++r) {
            TuiEngine::MoveCursor(top + 4 + static_cast<int>(r), left + 2);
            if (r < pageGames.size()) {
                const auto& g = pageGames[r];
                const size_t globalIdx = startIndex + r + 1;
                bool isShared = g.name.starts_with("[共享] ");
                std::string displayName = isShared ? g.name.substr(9) : g.name;
                std::string truncatedName = TuiEngine::TruncateToWidth(displayName, nameColWidth);
                std::string paddedName = TuiEngine::Pad(truncatedName, nameColWidth);

                std::string rowStr = std::format(" {:>4} │ {:<10} │ {} │ {:^6} ",
                                                 globalIdx, g.appId, paddedName, isShared ? "共享" : "就绪");
                rowStr = TuiEngine::Pad(rowStr, static_cast<size_t>(innerW));

                if (r == selectedRow) {
                    rowStr[0] = '>';
                    std::cout << "\x1b[30;107m" << rowStr << "\x1b[0m";
                } else if (isShared) {
                    std::cout << "\x1b[96m" << rowStr << "\x1b[0m";
                } else {
                    std::cout << "\x1b[37m" << rowStr << "\x1b[0m";
                }
            } else {
                std::cout << std::string(innerW, ' ');
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

    void UpdateLevel3Input(std::string_view inputAppId) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int boxW = std::clamp(w - 4, 70, 120);
        const int boxH = std::clamp(h - 4, 16, 28);
        const int top = (std::max)(1, (h - boxH) / 2);
        const int left = (std::max)(1, (w - boxW) / 2);

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
        bool needFullClear = true;
        while (menuActive) {
            RenderLevel2Tui(accounts, selected, needFullClear);
            needFullClear = false;

            KeyEvent ev = TuiEngine::ReadKey();

            if (ev.code == KeyCode::Resize) {
                needFullClear = true;
                continue;
            }

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
                TuiEngine::ClearScreen();
                return;
            }

            // 'd' or Delete -> Delete selected account
            if (ev.code == KeyCode::Delete ||
                (ev.code == KeyCode::Char && (ev.ch == 'd' || ev.ch == 'D'))) {
                if (selected >= 1 && selected <= accounts.size()) {
                    const auto& targetAcc = accounts[selected - 1];
                    TuiEngine::ClearScreen();
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        "删除账号凭据",
                        std::format("确定删除账号 [{}] 的本地登录缓存吗?", targetAcc.accountName),
                        "删除后将无法免密登录该账号",
                        false);
                    needFullClear = true;
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
                    TuiEngine::ClearScreen();
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        "粉碎全部凭据",
                        "警告：确定要彻底粉碎全部本地账号凭据缓存吗?",
                        "本地所有账号的 DPAPI 加密凭据都将被安全抹除",
                        false);
                    needFullClear = true;
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
                    TuiEngine::ClearScreen();
                    auto accName = TuiEngine::PromptInputModal("Steam 账号登录", "请输入 Steam 登录账号:", "");
                    if (!accName || accName->empty()) {
                        needFullClear = true;
                        continue;
                    }
                    TuiEngine::ClearScreen();
                    auto pwdStr = TuiEngine::PromptInputModal("Steam 账号登录", "请输入 Steam 密码 (掩码保护):", "", true);
                    if (!pwdStr || pwdStr->empty()) {
                        needFullClear = true;
                        continue;
                    }

                    SecureString secPwd(*pwdStr);
                    SecureZeroMemory(pwdStr->data(), pwdStr->size());

                    TuiEngine::ClearScreen();
                    auto loginRes = authService.LoginWithCredentials(*accName, secPwd);
                    secPwd.Clear();

                    if (loginRes.success) {
                        TuiEngine::ClearScreen();
                        RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.refreshToken, loginRes.accessToken);
                    } else {
                        TuiEngine::ClearScreen();
                        TuiEngine::ShowMessageModal("登录失败",
                                                    loginRes.errorMessage.empty() ? "账号或密码错误" : loginRes.errorMessage,
                                                    "请检查网络连接及动态验证码输入");
                    }
                    needFullClear = true;
                    break;
                } else if (selected >= 1 && selected <= accounts.size()) {
                    // Cached account
                    const auto& acc = accounts[selected - 1];
                    std::string activeToken = acc.accessToken;
                    const auto nowSec = std::time(nullptr);
                    // Proactively refresh if token is empty or older than 12 hours
                    if (activeToken.empty() || (acc.lastLoginTime > 0 && nowSec - acc.lastLoginTime > 12 * 3600)) {
                        auto accessOpt = authService.RefreshAccessToken(acc.steamId, acc.refreshToken);
                        if (accessOpt && !accessOpt->empty()) {
                            activeToken = *accessOpt;
                            CachedAccount updated = acc;
                            updated.accessToken = activeToken;
                            updated.lastLoginTime = nowSec;
                            TokenStorage::UpsertAccount(updated);
                        }
                    }

                    if (!activeToken.empty()) {
                        TuiEngine::ClearScreen();
                        RunInSessionExtraction(acc.accountName, acc.steamId, acc.refreshToken, activeToken);
                    } else {
                        TuiEngine::ClearScreen();
                        bool relogin = TuiEngine::ShowConfirmModal(
                            "授权令牌失效",
                            std::format("账号 [{}] 的本地授权令牌已过期或失效", acc.accountName),
                            "是否立即重新输入密码登录该账号?",
                            true);
                        if (relogin) {
                            TuiEngine::ClearScreen();
                            auto pwdStr = TuiEngine::PromptInputModal("重新登录", std::format("请输入账号 [{}] 的密码:", acc.accountName), "", true);
                            if (pwdStr && !pwdStr->empty()) {
                                SecureString secPwd(*pwdStr);
                                SecureZeroMemory(pwdStr->data(), pwdStr->size());
                                TuiEngine::ClearScreen();
                                auto loginRes = authService.LoginWithCredentials(acc.accountName, secPwd);
                                secPwd.Clear();
                                if (loginRes.success) {
                                    TuiEngine::ClearScreen();
                                    RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.refreshToken, loginRes.accessToken);
                                } else {
                                    TuiEngine::ClearScreen();
                                    TuiEngine::ShowMessageModal("登录失败", loginRes.errorMessage);
                                }
                            }
                        } else {
                            TokenStorage::DeleteAccount(acc.accountName);
                        }
                    }
                    needFullClear = true;
                    break;
                } else if (!accounts.empty() && selected == accounts.size() + 1) {
                    TuiEngine::ClearScreen();
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        "粉碎全部凭据",
                        "警告：确定要彻底粉碎全部本地账号凭据缓存吗?",
                        "本地所有账号的 DPAPI 加密凭据都将被安全抹除",
                        false);
                    needFullClear = true;
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
    const std::string& refreshToken,
    const std::string& accessToken) {

    SteamAuthService authService;
    SteamCmClient cmClient;

    int w = 80, h = 25;
    TuiEngine::GetScreenSize(w, h);
    const int modalW = std::clamp(w - 12, 64, 84);
    const int modalH = 6;
    const int top = (h - modalH) / 2, left = (w - modalW) / 2;
    const size_t innerW = static_cast<size_t>(modalW - 8);

    TuiEngine::ClearScreen();
    TuiEngine::DrawBox(top, left, modalW, modalH, "Steam 正在接入网关");
    TuiEngine::PrintBounded(top + 2, left + 4, "正在建立安全 WebSocket CM 会话并登录...", innerW, "\x1b[1;36m");
    std::cout.flush();

    std::string curAccessToken = accessToken;
    if (!cmClient.ConnectAndLogon(steamId, refreshToken, curAccessToken)) {
        // Attempt automatic refresh if token was stale
        bool logonOk = false;
        if (!refreshToken.empty()) {
            TuiEngine::PrintBounded(top + 2, left + 4, "访问令牌失效，正在使用刷新令牌自动续期...", innerW, "\x1b[1;33m");
            std::cout.flush();
            auto accessOpt = authService.RefreshAccessToken(steamId, refreshToken);
            if (accessOpt && !accessOpt->empty()) {
                curAccessToken = *accessOpt;
                CachedAccount updated;
                updated.accountName = accountName;
                updated.steamId = steamId;
                updated.refreshToken = refreshToken;
                updated.accessToken = curAccessToken;
                updated.lastLoginTime = std::time(nullptr);
                TokenStorage::UpsertAccount(updated);

                if (cmClient.ConnectAndLogon(steamId, refreshToken, curAccessToken)) {
                    logonOk = true;
                }
            }
        }

        if (!logonOk) {
            TuiEngine::ClearScreen();
            TuiEngine::ShowMessageModal("连接失败", "无法建立 Steam CM WebSocket 会话", "请检查网络连接或系统代理设置");
            return;
        }
    }

    TuiEngine::PrintBounded(top + 2, left + 4, "登录成功！正在同步当前账号正版游戏列表...", innerW, "\x1b[1;32m");
    std::cout.flush();

    auto games = authService.FetchOwnedGames(steamId, curAccessToken);
    if (games.empty()) {
        TuiEngine::ClearScreen();
        TuiEngine::ShowMessageModal("游戏库同步",
                                    "未拉取到公开游戏列表 (可能个人资料设为了私密或暂无游戏)",
                                    "您仍可在接下来的界面中直接输入 AppID 快速提取正版凭据");
    }

    GameListManager gameMgr(std::move(games), 20);
    size_t selectedRow = 0;
    std::string inputAppId;
    bool needFullClear = true;

    while (true) {
        RenderLevel3Tui(gameMgr, selectedRow, inputAppId, accountName, steamId, needFullClear);
        needFullClear = false;

        KeyEvent ev = TuiEngine::ReadKey();

        if (ev.code == KeyCode::Resize) {
            needFullClear = true;
            continue;
        }

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
                needFullClear = true;
            }
            continue;
        }

        // Right / 'n' / 'N' -> Next Page
        if (ev.code == KeyCode::Right ||
            (ev.code == KeyCode::Char && (ev.ch == 'n' || ev.ch == 'N'))) {
            if (gameMgr.NextPage()) {
                selectedRow = 0;
                needFullClear = true;
            }
            continue;
        }

        // 'l' / 'L' -> Export CSV
        if (ev.code == KeyCode::Char && (ev.ch == 'l' || ev.ch == 'L')) {
            TuiEngine::ClearScreen();
            bool confirmed = TuiEngine::ShowConfirmModal(
                "导出游戏列表表格",
                std::format("确定要将当前账号全部 {} 款游戏导出为本地表格吗?", gameMgr.TotalGames()),
                std::format("导出文件: gameslist-{}.csv", accountName),
                false);
            needFullClear = true;
            if (confirmed) {
                std::string outPath;
                if (gameMgr.ExportCsv(accountName, &outPath)) {
                    TuiEngine::ClearScreen();
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
            TuiEngine::ClearScreen();
            bool confirmed = TuiEngine::ShowConfirmModal(
                "批量提取凭证",
                std::format("确定要批量提取当前账号全部 {} 款游戏的全部凭证吗?", gameMgr.TotalGames()),
                "流水线将自动提取票据、密钥与Lua配置 (温和限速防止风控)",
                false);
            needFullClear = true;
            if (confirmed) {
                int scrW = 80, scrH = 25;
                TuiEngine::GetScreenSize(scrW, scrH);
                const int bModalW = std::clamp(scrW - 12, 60, 90);
                const int bModalH = 8;
                const int bTop = (scrH - bModalH) / 2;
                const int bLeft = (scrW - bModalW) / 2;

                TuiEngine::ClearScreen();
                TuiEngine::DrawBox(bTop, bLeft, bModalW, bModalH, "流水线批量提取中");

                size_t progress = 0;
                size_t succeeded = 0;
                const size_t total = gameMgr.TotalGames();

                for (const auto& game : gameMgr.Games()) {
                    ++progress;
                    TuiEngine::DrawProgressBar(bTop + 3, bLeft + 4, bModalW - 8, progress, total, game.name);

                    auto creds = cmClient.ExtractFullCredentials(game.appId);
                    if (WriteOutputs(game.appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                     creds.depotKeys, creds.dlcs, creds.appTokens)) {
                        ++succeeded;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                TuiEngine::ClearScreen();
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
                UpdateLevel3Input(inputAppId);
            }
            continue;
        }

        // Backspace
        if (ev.code == KeyCode::Backspace) {
            if (!inputAppId.empty()) {
                inputAppId.pop_back();
                UpdateLevel3Input(inputAppId);
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
                int scrW = 80, scrH = 25;
                TuiEngine::GetScreenSize(scrW, scrH);
                const int eModalW = std::clamp(scrW - 12, 64, 88);
                const int eModalH = 7;
                const int eTop = (scrH - eModalH) / 2, eLeft = (scrW - eModalW) / 2;
                const size_t eInnerW = static_cast<size_t>(eModalW - 8);

                TuiEngine::ClearScreen();
                TuiEngine::DrawBox(eTop, eLeft, eModalW, eModalH, "提取凭据中");

                std::string line1 = "正在向 Steam CM 提取正版凭据与解密密钥...";
                std::string line2 = std::format("目标游戏: {} ({})", gameName, targetAppId);

                TuiEngine::PrintBounded(eTop + 2, eLeft + 4, line1, eInnerW, "\x1b[1;33m");
                TuiEngine::PrintBounded(eTop + 3, eLeft + 4, line2, eInnerW, "\x1b[36m");
                std::cout.flush();

                auto creds = cmClient.ExtractFullCredentials(targetAppId);
                bool ok = WriteOutputs(targetAppId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                       creds.depotKeys, creds.dlcs, creds.appTokens);
                TuiEngine::ClearScreen();
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
                needFullClear = true;
            }
            continue;
        }

        // 'q' / 'Q' or ESC -> Return to Level 2
        if (ev.code == KeyCode::Escape ||
            (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q'))) {
            cmClient.Disconnect();
            TuiEngine::ClearScreen();
            return;
        }
    }
}

} // namespace OST::ExtractTickets
