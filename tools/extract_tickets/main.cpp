#include "AppInfoParser.h"
#include "I18n.h"
#include "Log.h"
#include "LuaFallbackParser.h"
#include "OnlineSession.h"
#include "OutputWriter.h"
#include "RaiiGuards.h"
#include "SteamSession.h"
#include "TuiEngine.h"
#include "Utils.h"
#include "VdfParser.h"
#include "steam.h"

#include <chrono>
#include <format>
#include <io.h>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OST::ExtractTickets {

#if defined(_WIN64)
bool ExtractLocalApp(uint32_t appId, bool forceEticket, bool inTui = false) {
    if (inTui) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int modalW = std::clamp(w - 12, 60, 80);
        const int modalH = 6;
        const int top = (h - modalH) / 2, left = (w - modalW) / 2;
        TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ExtractingTitle).data());
        std::string msg = TR_FMT(MsgKey::ExtractingLocalMsg, appId);
        TuiEngine::PrintBounded(top + 2, left + 4, msg, static_cast<size_t>(modalW - 8), "\x1b[1;33m");
        std::cout.flush();
    }

    // Mark process as an OST extraction tool so Steam plugin avoids spurious game-launch Lua sync
    SetEnvironmentVariableA("OST_TOOL_EXTRACTION", "1");

    auto steamPathOpt = FindSteamInstallPath();
    std::string steamPath = steamPathOpt ? *steamPathOpt : "";

    const bool isInstalled = IsAppInstalledLocally(steamPath, appId);
    const bool injectAppId = isInstalled || forceEticket;

    if (injectAppId) {
        const std::string appIdStr{std::to_string(appId)};
        SetEnvironmentVariableA("SteamAppId", appIdStr.c_str());
        LOG_INFO("LocalExtract", "已为游戏 AppID {} 注入运行环境变量 (SteamAppId={})", appId, appIdStr);
        if (!inTui) {
            std::cout << TR_FMT(MsgKey::CliExtractLocalInjected, appId) << "\n";
        }
    } else if (!inTui) {
        std::cout << TR_FMT(MsgKey::CliExtractLocalSafe, appId) << "\n";
    }

    std::string steamClientPath;
    HMODULE steamClient = LoadSteamClient64(steamPath, steamClientPath);
    SteamSessionGuard sessionGuard{nullptr, 0, 0, steamClient};

    ISteamClient* client = steamClient ? CreateSteamClient(steamClient) : nullptr;
    sessionGuard.client = client;

    HSteamPipe pipe{0};
    HSteamUser user{0};
    const bool sessionOpened = (client != nullptr) && OpenSession(client, pipe, user);
    if (sessionOpened) {
        sessionGuard.pipe = pipe;
        sessionGuard.user = user;
    }

    std::optional<std::vector<uint8_t>> ownership;
    std::optional<std::vector<uint8_t>> encrypted;
    if (sessionOpened) {
        ownership = ExtractAppOwnershipTicket(client, pipe, user, appId);
        if (ownership) {
            LOG_INFO("LocalExtract", "成功提取 OwnershipTicket ({} 字节)", ownership->size());
        }
        if (injectAppId) {
            encrypted = ExtractEncryptedAppTicket(client, pipe, user, appId);
            if (encrypted) {
                LOG_INFO("LocalExtract", "成功提取 EncryptedAppTicket ({} 字节)", encrypted->size());
            }
        }
    }

    std::vector<DlcInfo> dlcs;
    std::vector<DepotKeyInfo> depotKeys = ExtractDepotDecryptionKeys(
        steamPath, appId, sessionOpened ? client : nullptr, pipe, user, dlcs);

    sessionGuard.Reset();

    std::unordered_map<uint32_t, uint64_t> appTokens;
    if (!steamPath.empty()) {
        std::unordered_set<uint32_t> targetAppIds;
        targetAppIds.insert(appId);
        for (const auto& dlc : dlcs) {
            targetAppIds.insert(dlc.dlcId);
        }
        appTokens = ParseAppInfoTokens(steamPath, &targetAppIds);
    }

    const auto luaFallback = ParseLuaFallbackData(steamPath, appId);
    for (const auto& [tId, tVal] : luaFallback.appTokens) {
        if (tVal != 0) {
            appTokens.try_emplace(tId, tVal);
        }
    }

    const bool ok = WriteOutputs(appId, ownership, encrypted, depotKeys, dlcs, appTokens);
    return ok;
}

bool RunLocalExtractionWorker(uint32_t appId, bool forceEticket) {
    wchar_t exePath[32768]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (len == 0 || len >= std::size(exePath)) {
        return ExtractLocalApp(appId, forceEticket, true);
    }

    std::wstring cmd = std::format(L"\"{}\" --worker {}", exePath, appId);
    if (forceEticket) {
        cmd += L" --force-eticket";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Launch worker child process completely hidden (CREATE_NO_WINDOW)
    // Steam will bind its AppManager to this short-lived worker PID.
    // Upon worker exit (~200ms), Steam immediately releases the "Running" state!
    BOOL success = CreateProcessW(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        LOG_ERROR("Main", "创建 Worker 子进程失败 (GetLastError={})", GetLastError());
        return ExtractLocalApp(appId, forceEticket, true);
    }

    ScopedHandle hProcess{pi.hProcess};
    ScopedHandle hThread{pi.hThread};

    DWORD waitRes = WaitForSingleObject(hProcess, 15000);
    DWORD exitCode = 1;
    if (waitRes == WAIT_OBJECT_0) {
        GetExitCodeProcess(hProcess, &exitCode);
    } else {
        LOG_WARN("Main", "Worker 子进程执行超时，强制终止");
        TerminateProcess(hProcess, 1);
    }

    return exitCode == 0;
}

namespace {
    struct Level1Layout {
        int top{0};
        int left{0};
        int boxW{80};
        int boxH{17};
    };

    Level1Layout DrawLevel1Frame() {
        if (!TuiEngine::EnsureMinTerminalSize(76, 18)) {
            return {0, 0, 0, 0};
        }
        TuiEngine::BeginFrame();
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        TuiEngine::ClearScreen();

        const bool hasSteam = FindSteamInstallPath().has_value();
        TuiEngine::DrawHeader("extract_tickets",
                              hasSteam ? TR(MsgKey::L1HeaderTagLocal) : TR(MsgKey::L1HeaderTagNoSteam));

        const int boxW = std::clamp(w - 4, 70, 100);
        const int maxBoxH = (std::min)(17, h - 2);
        const int boxH = std::clamp(16, 15, maxBoxH);
        const int top = 2 + (h - 2 - boxH) / 2;
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, TR(MsgKey::L1BoxTitle).data(), /*clearInterior=*/false);

        const size_t innerW = static_cast<size_t>(boxW - 8);

        TuiEngine::PrintBounded(top + 2, left + 4,
            TR(MsgKey::L1Welcome),
            innerW, "\x1b[1;37m");

        if (!hasSteam) {
            TuiEngine::PrintBounded(top + 4, left + 4,
                TR(MsgKey::L1NoSteamNotice),
                innerW, "\x1b[1;33m");
        } else {
            TuiEngine::PrintBounded(top + 4, left + 4,
                TR(MsgKey::L1LocalDesc),
                innerW, "\x1b[90m");
        }

        TuiEngine::PrintBounded(top + 6, left + 4,
            TR(MsgKey::L1InputPrompt),
            innerW, "\x1b[1;36m");

        TuiEngine::MoveCursor(top + 10, left + 4);
        TuiEngine::PrintRaw(std::format("\x1b[90m{}\x1b[0m", std::string(innerW, '-')));

        TuiEngine::PrintBounded(top + 11, left + 4, TR(MsgKey::L1NavTitle), innerW, "\x1b[1;33m");

        TuiEngine::PrintBounded(top + 12, left + 6,
            TR(MsgKey::L1NavOnline),
            (innerW > 2) ? (innerW - 2) : innerW, "\x1b[1;36m");

        TuiEngine::PrintBounded(top + 13, left + 6,
            TR(MsgKey::L1NavExit),
            (innerW > 2) ? (innerW - 2) : innerW, "\x1b[37m");

        TuiEngine::DrawFooter(TR(MsgKey::L1Footer));
        TuiEngine::EndFrame();
        return {top, left, boxW, boxH};
    }

    void UpdateLevel1Input(const Level1Layout& layout, std::string_view inputAppId) {
        if (layout.boxW == 0 || layout.boxH == 0) return;
        TuiEngine::MoveCursor(layout.top + 7, layout.left + 4);
        std::string boxContent = std::format("[ {:<16} ]", std::string{inputAppId} + "_");
        TuiEngine::PrintRaw(std::format("\x1b[1;30;47m{}\x1b[0m  \x1b[90m{}\x1b[0m   ", boxContent, TR(MsgKey::L1InputTip)));
    }
} // namespace

int Run(int argc, char** argv) {
    I18n::InitLanguage();

    struct LoggingScopeGuard {
        LoggingScopeGuard() {
            InitLogging("extract_tickets_debug.log");
            LOG_INFO("Main", "=== extract_tickets 会话启动 (PID: {}) ===", GetCurrentProcessId());
        }
        ~LoggingScopeGuard() {
            LOG_INFO("Main", "=== extract_tickets 会话正常退出 ===");
            CloseLogging();
        }
    } logGuard;

    std::optional<uint32_t> cliAppId;
    std::string cliAccount;
    bool forceEticket{false};
    bool onlineMode{false};
    bool workerMode{false};

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--online" || arg == "-o" || arg == "-O") {
            onlineMode = true;
        } else if (arg == "--account" || arg == "-a") {
            if (i + 1 < argc) {
                cliAccount = argv[++i];
            }
        } else if (arg.starts_with("--account=")) {
            cliAccount = arg.substr(10);
        } else if (arg == "--force-eticket" || arg == "-f") {
            forceEticket = true;
        } else if (arg == "--worker") {
            workerMode = true;
        } else if (arg == "--appid") {
            if (i + 1 < argc) {
                cliAppId = ParseAppId(argv[++i]);
                if (!cliAppId) {
                    std::cerr << TR_FMT(MsgKey::ErrInvalidAppIdCli, argv[i]) << "\n";
                    return 1;
                }
            }
        } else if (arg.starts_with("--appid=")) {
            cliAppId = ParseAppId(arg.substr(8));
            if (!cliAppId) {
                std::cerr << TR_FMT(MsgKey::ErrInvalidAppIdCli, arg.substr(8)) << "\n";
                return 1;
            }
        } else if (!cliAppId) {
            cliAppId = ParseAppId(arg);
            if (!cliAppId) {
                std::cerr << TR_FMT(MsgKey::ErrInvalidAppIdCli, arg) << "\n";
                return 1;
            }
        }
    }

    if (workerMode && cliAppId) {
        // Child worker process: runs silent extraction and terminates cleanly so Steam releases "Running" state
        const bool ok = ExtractLocalApp(*cliAppId, forceEticket, false);
        return ok ? 0 : 1;
    }

    if (onlineMode) {
        if (cliAppId) {
            return OnlineSession::RunSilent(*cliAppId, cliAccount, forceEticket);
        }
        TuiSessionGuard tuiGuard(_isatty(_fileno(stdin)) != 0);
        return OnlineSession::RunInteractive();
    }

    const bool isInteractive = !cliAppId.has_value() && (_isatty(_fileno(stdin)) != 0);

    if (!isInteractive) {
        if (cliAppId) {
            const bool ok = ExtractLocalApp(*cliAppId, forceEticket, false);
            return ok ? 0 : 1;
        }

        // Piped/redirected non-interactive stdin
        std::string line;
        if (!std::getline(std::cin, line)) {
            return 0;
        }
        line = std::string{TrimWhitespace(line)};
        if (line == "q" || line == "Q") {
            return 0;
        }
        if (line == "o" || line == "O") {
            return OnlineSession::RunInteractive();
        }
        auto parsed = ParseAppId(line);
        if (parsed) {
            const bool ok = ExtractLocalApp(*parsed, forceEticket, false);
            return ok ? 0 : 1;
        }
        return 0;
    }

    // Interactive TUI session
    TuiSessionGuard tuiGuard(true);
    std::string inputAppId;
    auto layout = DrawLevel1Frame();
    UpdateLevel1Input(layout, inputAppId);

    while (true) {
        KeyEvent ev = TuiEngine::ReadKey();

        if (ev.code == KeyCode::Resize) {
            TuiEngine::ClearScreen();
            layout = DrawLevel1Frame();
            UpdateLevel1Input(layout, inputAppId);
            continue;
        }

        // Single-key 'o' or 'O' directly jumps to Online Mode (no Enter required!)
        if (ev.code == KeyCode::Char && (ev.ch == 'o' || ev.ch == 'O')) {
            if (inputAppId.empty() && !TuiEngine::HasInputPending()) {
                TuiEngine::ClearScreen();
                OnlineSession::RunInteractive();
                inputAppId.clear();
                layout = DrawLevel1Frame();
                UpdateLevel1Input(layout, inputAppId);
            }
            continue;
        }

        // ESC -> Clear input if non-empty; otherwise exit
        if (ev.code == KeyCode::Escape) {
            if (!inputAppId.empty()) {
                inputAppId.clear();
                UpdateLevel1Input(layout, inputAppId);
                continue;
            }
            return 0;
        }

        // Single-key 'q' or 'Q' directly exits
        if (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q')) {
            if (inputAppId.empty() && !TuiEngine::HasInputPending()) {
                return 0;
            }
            continue;
        }

        // Digits 0-9
        if (ev.code == KeyCode::Char && ev.ch >= '0' && ev.ch <= '9') {
            if (inputAppId.size() < 10) {
                inputAppId.push_back(ev.ch);
                while (TuiEngine::HasInputPending() && inputAppId.size() < 10) {
                    KeyEvent nextEv = TuiEngine::ReadKey();
                    if (nextEv.code == KeyCode::Char && nextEv.ch >= '0' && nextEv.ch <= '9') {
                        inputAppId.push_back(nextEv.ch);
                    }
                }
                UpdateLevel1Input(layout, inputAppId);
            }
            continue;
        }

        // Backspace
        if (ev.code == KeyCode::Backspace) {
            if (!inputAppId.empty()) {
                inputAppId.pop_back();
                UpdateLevel1Input(layout, inputAppId);
            }
            continue;
        }

        // Enter -> Start local extraction (standard ticket)
        // 'e' / 'E' -> Start local extraction with forced encrypted ticket (--force-eticket)
        const bool isEnter = (ev.code == KeyCode::Enter);
        const bool isForceE = (ev.code == KeyCode::Char && (ev.ch == 'e' || ev.ch == 'E'));

        if ((isEnter || isForceE) && !inputAppId.empty()) {
            auto appId = ParseAppId(inputAppId);
            if (appId && *appId > 0) {
                const bool runForceEticket = isForceE;
                if (!FindSteamInstallPath().has_value()) {
                    TuiEngine::ClearScreen();
                    bool goOnline = TuiEngine::ShowConfirmModal(
                        TR(MsgKey::NoSteamConfirmTitle).data(),
                        TR(MsgKey::NoSteamConfirmMsg).data(),
                        TR(MsgKey::NoSteamConfirmPrompt).data(),
                        true);
                    if (goOnline) {
                        TuiEngine::ClearScreen();
                        OnlineSession::RunInteractive();
                        inputAppId.clear();
                        layout = DrawLevel1Frame();
                        UpdateLevel1Input(layout, inputAppId);
                        continue;
                    }
                }

                int w = 80, h = 25;
                TuiEngine::GetScreenSize(w, h);
                const int modalW = std::clamp(w - 12, 60, 80);
                const int modalH = 6;
                const int top = (h - modalH) / 2, left = (w - modalW) / 2;

                TuiEngine::ClearScreen();
                TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ExtractingTitle).data());
                std::string msg = TR_FMT(MsgKey::ExtractingLocalMsg, *appId);
                TuiEngine::PrintBounded(top + 2, left + 4, msg, static_cast<size_t>(modalW - 8), "\x1b[1;33m");
                std::cout.flush();

                bool ok = RunLocalExtractionWorker(*appId, runForceEticket);

                TuiEngine::ClearScreen();
                if (ok) {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::ExtractSuccessTitle).data(),
                        TR_FMT(MsgKey::ExtractSuccessMsg, *appId),
                        TR_FMT(MsgKey::ExtractSuccessDetail, *appId));
                } else {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::ExtractWarnTitle).data(),
                        TR_FMT(MsgKey::ExtractWarnMsg, *appId),
                        "");
                }
                // Reset input to return to fresh startup state
                inputAppId.clear();
                layout = DrawLevel1Frame();
                UpdateLevel1Input(layout, inputAppId);
            }
            continue;
        }
    }
}
#endif

} // namespace OST::ExtractTickets

int main(int argc, char** argv) {
#if !defined(_WIN64)
    std::cerr << "[ERROR] extract_tickets 必须编译为 64 位 Windows 程序。\n"
              << "        extract_tickets must be built as a 64-bit Windows executable.\n";
    return 1;
#else
    OST::ExtractTickets::ConsoleCodePageGuard cpGuard;
    const int rc = OST::ExtractTickets::Run(argc, argv);
    return rc;
#endif
}
