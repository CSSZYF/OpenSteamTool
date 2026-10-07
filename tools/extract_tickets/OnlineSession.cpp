#include "OnlineSession.h"
#include "GameListManager.h"
#include "I18n.h"
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
#if defined(_WIN32)
#include <conio.h>
#endif

namespace OST::ExtractTickets {

namespace {
    std::string FormatTimestamp(int64_t t) {
        if (t <= 0) return std::string{TR(MsgKey::TimestampUnknown)};
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

        TuiEngine::DrawHeader("extract_tickets",
                              TR_FMT(MsgKey::L2HeaderTagAccounts, accounts.size()));

        const int boxW = std::clamp(w - 4, 70, 110);
        const int boxH = std::clamp(h - 4, 16, 26);
        const int top = (std::max)(1, (h - boxH) / 2);
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, TR(MsgKey::L2BoxTitle).data());

        const size_t innerW = static_cast<size_t>(boxW - 8);

        // Subtitle explanation
        TuiEngine::PrintBounded(top + 2, left + 4,
            TR(MsgKey::L2Subtitle),
            innerW, "\x1b[90m");

        // Item 0: [+] Login New Account (Default)
        std::string item0 = std::format("   {}", TR(MsgKey::L2ActionLogin));
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
            std::string lineText = TR_FMT(MsgKey::L2AccountItem,
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
            std::string wipeText = std::format("   {}", TR(MsgKey::L2ActionWipe));
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

        std::string_view tip;
        std::string tipStyle = "\x1b[32m";
        if (selected == 0) {
            tip = TR(MsgKey::L2TipLogin);
            tipStyle = "\x1b[33m";
        } else if (selected <= accounts.size()) {
            tip = TR(MsgKey::L2TipCached);
            tipStyle = "\x1b[32m";
        } else {
            tip = TR(MsgKey::L2TipWipe);
            tipStyle = "\x1b[31m";
        }
        TuiEngine::PrintBounded(top + boxH - 3, left + 4, tip, innerW, tipStyle);

        TuiEngine::DrawFooter(TR(MsgKey::L2Footer));
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

        std::string title = TR_FMT(MsgKey::L3HeaderAccount, accountName, steamId);
        std::string tag = TR_FMT(MsgKey::L3HeaderPage,
                                 gameMgr.CurrentPage() + 1,
                                 gameMgr.TotalPages(),
                                 gameMgr.TotalGames());
        TuiEngine::DrawHeader(title, tag);

        const int boxW = std::clamp(w - 4, 70, 120);
        const int boxH = std::clamp(h - 4, 16, 28);
        const int top = (std::max)(1, (h - boxH) / 2);
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, TR(MsgKey::L3BoxTitle).data());

        const int innerW = boxW - 4;
        const size_t nameColWidth = (innerW > 34) ? static_cast<size_t>(innerW - 32) : 18;

        // Table header
        TuiEngine::MoveCursor(top + 2, left + 2);
        std::string nameTitle = TuiEngine::Pad(std::string{TR(MsgKey::L3ColName)}, nameColWidth);
        std::string hdr = std::format(" {:>4} │ {:<10} │ {} │ {:^6} ",
                                      TR(MsgKey::L3ColIndex), TR(MsgKey::L3ColAppId), nameTitle, TR(MsgKey::L3ColStatus));
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
                bool isShared = g.isShared;
                std::string displayName = g.name;
                std::string truncatedName = TuiEngine::TruncateToWidth(displayName, nameColWidth);
                std::string paddedName = TuiEngine::Pad(truncatedName, nameColWidth);

                std::string statusBadge = std::string{isShared ? TR(MsgKey::L3StatusShared) : TR(MsgKey::L3StatusReady)};
                std::string rowStr = std::format(" {:>4} │ {:<10} │ {} │ {:^6} ",
                                                 globalIdx, g.appId, paddedName, statusBadge);
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
        std::cout << "\x1b[1;37m" << TR(MsgKey::L3QuickInputPrompt) << " \x1b[1;30;47m[ "
                  << std::format("{:<12}", std::string{inputAppId} + "_")
                  << " ]\x1b[0m  \x1b[90m" << TR(MsgKey::L3QuickInputHint) << "\x1b[0m";

        TuiEngine::DrawFooter(TR(MsgKey::L3Footer));
    }

    void UpdateLevel3Input(std::string_view inputAppId) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int boxW = std::clamp(w - 4, 70, 120);
        const int boxH = std::clamp(h - 4, 16, 28);
        const int top = (std::max)(1, (h - boxH) / 2);
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::MoveCursor(top + boxH - 2, left + 4);
        std::cout << "\x1b[1;37m" << TR(MsgKey::L3QuickInputPrompt) << " \x1b[1;30;47m[ "
                  << std::format("{:<12}", std::string{inputAppId} + "_")
                  << " ]\x1b[0m  \x1b[90m" << TR(MsgKey::L3QuickInputHint) << "\x1b[0m   ";
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
                        TR(MsgKey::DelAccountTitle).data(),
                        TR_FMT(MsgKey::DelAccountMsg, targetAcc.accountName),
                        TR(MsgKey::DelAccountDetail).data(),
                        false);
                    needFullClear = true;
                    if (confirmed) {
                        TokenStorage::DeleteAccount(targetAcc.accountName);
                        break; // Reload accounts & redraw menu
                    }
                }
                continue;
            }

            // Enter key
            if (ev.code == KeyCode::Enter) {
                if (selected == 0) {
                    // Log in to a new account
                    TuiEngine::ClearScreen();
                    auto accName = TuiEngine::PromptInputModal(
                        TR(MsgKey::LoginPromptTitle).data(),
                        TR(MsgKey::LoginPromptAccount).data(),
                        "");
                    if (!accName || accName->empty()) {
                        needFullClear = true;
                        continue;
                    }
                    TuiEngine::ClearScreen();
                    auto pwdStr = TuiEngine::PromptInputModal(
                        TR(MsgKey::LoginPromptTitle).data(),
                        TR(MsgKey::LoginPromptPassword).data(),
                        "",
                        true);
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
                    } else if (loginRes.cancelled) {
                        needFullClear = true;
                        continue;
                    } else {
                        TuiEngine::ClearScreen();
                        TuiEngine::ShowMessageModal(
                            TR(MsgKey::LoginFailedTitle).data(),
                            loginRes.errorMessage.empty() ? TR(MsgKey::LoginFailedDefaultMsg).data() : loginRes.errorMessage,
                            TR(MsgKey::LoginFailedDetail).data());
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
                        } else if (acc.lastLoginTime > 0 && nowSec - acc.lastLoginTime > 24 * 3600) {
                            activeToken.clear();
                        }
                    }

                    if (!activeToken.empty()) {
                        TuiEngine::ClearScreen();
                        RunInSessionExtraction(acc.accountName, acc.steamId, acc.refreshToken, activeToken);
                    } else {
                        TuiEngine::ClearScreen();
                        bool relogin = TuiEngine::ShowConfirmModal(
                            TR(MsgKey::TokenExpiredTitle).data(),
                            TR_FMT(MsgKey::TokenExpiredMsg, acc.accountName),
                            TR(MsgKey::TokenExpiredPrompt).data(),
                            true);
                        if (relogin) {
                            TuiEngine::ClearScreen();
                            auto pwdStr = TuiEngine::PromptInputModal(
                                TR(MsgKey::ReloginTitle).data(),
                                TR_FMT(MsgKey::ReloginPrompt, acc.accountName),
                                "",
                                true);
                            if (pwdStr && !pwdStr->empty()) {
                                SecureString secPwd(*pwdStr);
                                SecureZeroMemory(pwdStr->data(), pwdStr->size());
                                TuiEngine::ClearScreen();
                                auto loginRes = authService.LoginWithCredentials(acc.accountName, secPwd);
                                secPwd.Clear();
                                if (loginRes.success) {
                                    TuiEngine::ClearScreen();
                                    RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.refreshToken, loginRes.accessToken);
                                } else if (loginRes.cancelled) {
                                    needFullClear = true;
                                    continue;
                                } else {
                                    TuiEngine::ClearScreen();
                                    TuiEngine::ShowMessageModal(TR(MsgKey::LoginFailedTitle).data(), loginRes.errorMessage);
                                }
                            }
                        }
                    }
                    needFullClear = true;
                    break;
                } else if (!accounts.empty() && selected == accounts.size() + 1) {
                    TuiEngine::ClearScreen();
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        TR(MsgKey::WipeAllTitle).data(),
                        TR(MsgKey::WipeAllMsg).data(),
                        TR(MsgKey::WipeAllDetail).data(),
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
    TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ConnectingTitle).data());
    TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::ConnectingGatewayMsg), innerW, "\x1b[1;36m");
    std::cout.flush();

    std::string curAccessToken = accessToken;
    if (!cmClient.ConnectAndLogon(steamId, refreshToken, curAccessToken)) {
        // Attempt automatic refresh if token was stale
        bool logonOk = false;
        if (!refreshToken.empty()) {
            TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::ConnectingRefreshingMsg), innerW, "\x1b[1;33m");
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
            TuiEngine::ShowMessageModal(TR(MsgKey::ConnectingFailedTitle).data(),
                                        TR(MsgKey::ConnectingFailedMsg).data(),
                                        TR(MsgKey::ConnectingFailedDetail).data());
            return;
        }
    } else {
        CachedAccount updated;
        updated.accountName = accountName;
        updated.steamId = steamId;
        updated.refreshToken = refreshToken;
        updated.accessToken = curAccessToken;
        updated.lastLoginTime = std::time(nullptr);
        TokenStorage::UpsertAccount(updated);
    }

    TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::SyncingGamesMsg), innerW, "\x1b[1;32m");
    std::cout.flush();

    auto games = authService.FetchOwnedGames(steamId, curAccessToken, &cmClient);
    if (games.empty()) {
        TuiEngine::ClearScreen();
        TuiEngine::ShowMessageModal(TR(MsgKey::NoGamesFoundTitle).data(),
                                    TR(MsgKey::NoGamesFoundMsg).data(),
                                    TR(MsgKey::NoGamesFoundDetail).data());
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
                TR(MsgKey::CsvExportTitle).data(),
                TR_FMT(MsgKey::CsvExportMsg, gameMgr.TotalGames()),
                TR_FMT(MsgKey::CsvExportDetail, accountName),
                false);
            needFullClear = true;
            if (confirmed) {
                std::string outPath;
                if (gameMgr.ExportCsv(accountName, &outPath)) {
                    TuiEngine::ClearScreen();
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::CsvExportSuccessTitle).data(),
                        TR_FMT(MsgKey::CsvExportSuccessMsg, outPath),
                        TR(MsgKey::CsvExportSuccessDetail).data());
                }
            }
            continue;
        }

        // 'a' / 'A' -> Batch extract all
        if (ev.code == KeyCode::Char && (ev.ch == 'a' || ev.ch == 'A')) {
            TuiEngine::ClearScreen();
            bool confirmed = TuiEngine::ShowConfirmModal(
                TR(MsgKey::BatchConfirmTitle).data(),
                TR_FMT(MsgKey::BatchConfirmMsg, gameMgr.TotalGames()),
                TR(MsgKey::BatchConfirmDetail).data(),
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
                TuiEngine::DrawBox(bTop, bLeft, bModalW, bModalH, TR(MsgKey::BatchExtractTitle).data());

                size_t progress = 0;
                size_t succeeded = 0;
                const size_t total = gameMgr.TotalGames();
                bool aborted = false;

                for (const auto& game : gameMgr.Games()) {
#if defined(_WIN32)
                    if (_kbhit()) {
                        int ch = _getch();
                        if (ch == 27) {
                            TuiEngine::FlushInputBuffer();
                            aborted = true;
                            break;
                        }
                    }
#endif
                    ++progress;
                    TuiEngine::DrawProgressBar(bTop + 3, bLeft + 4, bModalW - 8, progress, total, game.name);

                    auto creds = cmClient.ExtractFullCredentials(game.appId);
                    if (WriteOutputs(game.appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                     creds.depotKeys, creds.dlcs, creds.appTokens)) {
                        ++succeeded;
                    }

                    for (int s = 0; s < 4; ++s) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
#if defined(_WIN32)
                        if (_kbhit()) {
                            int ch = _getch();
                            if (ch == 27) {
                                TuiEngine::FlushInputBuffer();
                                aborted = true;
                                break;
                            }
                        }
#endif
                    }
                    if (aborted) break;
                }

                TuiEngine::ClearScreen();
                if (aborted) {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::BatchAbortedTitle).data(),
                        TR_FMT(MsgKey::BatchAbortedMsg, succeeded, total),
                        TR(MsgKey::BatchAbortedDetail).data());
                } else {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::BatchSuccessTitle).data(),
                        TR_FMT(MsgKey::BatchSuccessMsg, succeeded, total),
                        TR(MsgKey::BatchSuccessDetail).data());
                }
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

        // Enter -> Extract target AppID (normal mode, auto-detect Denuvo)
        // 'e' / 'E' -> Extract target AppID with forced encrypted ticket (--force-eticket)
        const bool isEnter = (ev.code == KeyCode::Enter);
        const bool isForceE = (ev.code == KeyCode::Char && (ev.ch == 'e' || ev.ch == 'E'));
        if (isEnter || isForceE) {
            const bool forceEticket = isForceE;
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
                TuiEngine::DrawBox(eTop, eLeft, eModalW, eModalH, TR(MsgKey::ExtractingOnlineTitle).data());

                std::string line1 = std::string{TR(MsgKey::ExtractingOnlineMsg1)};
                std::string line2 = TR_FMT(MsgKey::ExtractingOnlineMsg2, gameName, targetAppId);

                TuiEngine::PrintBounded(eTop + 2, eLeft + 4, line1, eInnerW, "\x1b[1;33m");
                TuiEngine::PrintBounded(eTop + 3, eLeft + 4, line2, eInnerW, "\x1b[36m");
                std::cout.flush();

                auto creds = cmClient.ExtractFullCredentials(targetAppId, forceEticket);
                bool ok = WriteOutputs(targetAppId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                       creds.depotKeys, creds.dlcs, creds.appTokens);
                TuiEngine::ClearScreen();
                if (ok) {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::ExtractSuccessTitle).data(),
                        TR_FMT(MsgKey::ExtractSuccessMsg, targetAppId),
                        TR_FMT(MsgKey::ExtractSuccessDetail, targetAppId));
                } else {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::ExtractWarnTitle).data(),
                        TR_FMT(MsgKey::ExtractWarnMsg, targetAppId));
                }
                needFullClear = true;
            }
            continue;
        }

        // ESC -> Clear quick extract input if non-empty; otherwise return to Level 2
        if (ev.code == KeyCode::Escape) {
            if (!inputAppId.empty()) {
                inputAppId.clear();
                UpdateLevel3Input(inputAppId);
                continue;
            }
            cmClient.Disconnect();
            TuiEngine::ClearScreen();
            return;
        }

        // 'q' / 'Q' -> Return to Level 2
        if (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q')) {
            cmClient.Disconnect();
            TuiEngine::ClearScreen();
            return;
        }
    }
}

int OnlineSession::RunSilent(uint32_t appId, const std::string& accountName, bool forceEticket) {
    if (appId == 0) {
        std::cerr << TR(MsgKey::InvalidAppId) << "\n";
        return 1;
    }

    auto accounts = TokenStorage::LoadAccounts();
    if (accounts.empty()) {
        std::cerr << TR(MsgKey::ErrNoSavedAccounts) << "\n";
        std::cerr << TR(MsgKey::ErrNoSavedAccountsTip) << "\n";
        return 1;
    }

    std::vector<const CachedAccount*> candidates;
    if (!accountName.empty()) {
        for (const auto& acc : accounts) {
            if (acc.accountName == accountName) {
                candidates.push_back(&acc);
                break;
            }
        }
        if (candidates.empty()) {
            std::cerr << TR_FMT(MsgKey::ErrAccountNotFound, MaskAccount(accountName)) << "\n";
            std::cerr << TR(MsgKey::ErrAccountListTip) << "\n";
            for (const auto& acc : accounts) {
                std::cerr << "  - " << MaskAccount(acc.accountName) << "\n";
            }
            return 1;
        }
    } else {
        for (const auto& acc : accounts) candidates.push_back(&acc);
        // Sort by lastLoginTime descending
        std::sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
            return a->lastLoginTime > b->lastLoginTime;
        });
    }

    SteamAuthService authService;
    SteamCmClient cmClient;
    const CachedAccount* activeAcc = nullptr;
    ExtractedAppCredentials creds;
    std::optional<ExtractedAppCredentials> fallbackCreds;
    const CachedAccount* fallbackAcc = nullptr;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto* curAcc = candidates[i];
        if (candidates.size() > 1 && accountName.empty()) {
            std::cout << TR_FMT(MsgKey::CliExtractAttempting,
                                MaskAccount(curAcc->accountName), i + 1, candidates.size()) << "\n";
        } else {
            std::cout << TR_FMT(MsgKey::CliExtractUsing,
                                MaskAccount(curAcc->accountName), appId) << "\n";
        }

        std::string activeToken = curAcc->accessToken;
        bool logonOk = cmClient.ConnectAndLogon(curAcc->steamId, curAcc->refreshToken, activeToken);
        if (!logonOk && !curAcc->refreshToken.empty()) {
            std::cout << TR(MsgKey::CliExtractTokenExpired) << "\n";
            auto refreshed = authService.RefreshAccessToken(curAcc->steamId, curAcc->refreshToken);
            if (refreshed && !refreshed->empty()) {
                activeToken = *refreshed;
                logonOk = cmClient.ConnectAndLogon(curAcc->steamId, curAcc->refreshToken, activeToken);
            }
        }

        if (logonOk) {
            std::cout << TR(MsgKey::CliExtractLoginSuccess) << "\n";
            auto curCreds = cmClient.ExtractFullCredentials(appId, forceEticket);

            const bool hasTicket = curCreds.appOwnershipTicket.has_value() || curCreds.encryptedAppTicket.has_value();
            const bool hasKeys = std::any_of(curCreds.depotKeys.begin(), curCreds.depotKeys.end(),
                                             [](const DepotKeyInfo& k) { return !k.hexKey.empty(); });

            if (hasTicket || hasKeys || !accountName.empty() || candidates.size() == 1) {
                // 当前账号拥有正版授权，或者为用户显式指定/唯一账号
                activeAcc = curAcc;
                creds = std::move(curCreds);

                CachedAccount updated = *curAcc;
                updated.accessToken = activeToken;
                updated.lastLoginTime = std::time(nullptr);
                TokenStorage::UpsertAccount(updated);
                break;
            }

            // 当前账号未拥有目标游戏正版授权（无票据且无密钥）
            if (!fallbackCreds.has_value()) {
                fallbackCreds = std::move(curCreds);
                fallbackAcc = curAcc;
            }

            if (i + 1 < candidates.size()) {
                std::cout << TR_FMT(MsgKey::CliExtractNoLicenseSwitch,
                                    MaskAccount(curAcc->accountName), appId) << "\n";
                cmClient.Disconnect();
            }
            continue;
        }

        if (accountName.empty() && i + 1 < candidates.size()) {
            std::cout << TR_FMT(MsgKey::CliExtractLoginFailedSwitch, MaskAccount(curAcc->accountName)) << "\n";
        }
    }

    if (!activeAcc) {
        if (fallbackAcc && fallbackCreds.has_value()) {
            activeAcc = fallbackAcc;
            creds = std::move(*fallbackCreds);
            LOG_INFO("OnlineSession", "{}", TR_FMT(MsgKey::CliExtractFallbackBase, appId));
        } else {
            std::cerr << TR(MsgKey::CliExtractFailedAll) << "\n";
            std::cerr << TR(MsgKey::CliExtractFailedAllTip) << "\n";
            return 1;
        }
    }

    bool ok = WriteOutputs(appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                           creds.depotKeys, creds.dlcs, creds.appTokens);
    if (ok) {
        std::cout << TR_FMT(MsgKey::CliExtractSuccess, appId, appId) << "\n";
        return 0;
    } else {
        std::cerr << TR_FMT(MsgKey::CliExtractWarn, appId) << "\n";
        return 1;
    }
}

} // namespace OST::ExtractTickets
