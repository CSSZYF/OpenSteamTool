#include "TuiEngine.h"
#include "Crypto.h"
#include "I18n.h"
#include "Utils.h"

#include <conio.h>
#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace OST::ExtractTickets {

namespace {
    bool g_tuiActive = false;
}

bool TuiEngine::IsActive() noexcept {
    return g_tuiActive;
}

void TuiEngine::SetActive(bool active) noexcept {
    g_tuiActive = active;
}

#if defined(_WIN32)
namespace {
    BOOL WINAPI ConsoleCtrlHandler(DWORD fdwCtrlType) {
        switch (fdwCtrlType) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
                if (g_tuiActive) {
                    TuiEngine::ShowCursor(true);
                    TuiEngine::ExitAlternateScreen();
                }
                return FALSE;
            default:
                return FALSE;
        }
    }
} // namespace
#endif

void TuiEngine::EnableVirtualTerminal() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
        SetConsoleOutputCP(CP_UTF8);
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE) {
        DWORD dwInMode = 0;
        if (GetConsoleMode(hIn, &dwInMode)) {
            dwInMode = (dwInMode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hIn, dwInMode);
        }
    }

#if defined(_WIN32)
    static bool handlerInstalled = false;
    if (!handlerInstalled) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        handlerInstalled = true;
    }
    SetConsoleTitleW(L"extract_tickets");
#endif
}

void TuiEngine::EnterAlternateScreen() {
    std::cout << "\x1b[?1049h\x1b[2J\x1b[H";
    std::cout.flush();
}

void TuiEngine::ExitAlternateScreen() {
    std::cout << "\x1b[?1049l";
    std::cout.flush();
}

void TuiEngine::ShowCursor(bool show) {
    std::cout << (show ? "\x1b[?25h" : "\x1b[?25l");
    std::cout.flush();
}

void TuiEngine::MoveCursor(int row, int col) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    std::cout << "\x1b[" << row << ";" << col << "H";
}

void TuiEngine::ClearScreen() {
    std::cout << "\x1b[2J\x1b[H";
    std::cout.flush();
}

void TuiEngine::RepositionCursor() {
    // Reposition cursor to top-left without wiping buffer to black, preventing flicker during typing
    std::cout << "\x1b[H";
    std::cout.flush();
}

void TuiEngine::GetScreenSize(int& outWidth, int& outHeight) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hOut, &csbi)) {
        outWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        outHeight = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        outWidth = 80;
        outHeight = 25;
    }
}

bool TuiEngine::EnsureMinTerminalSize(int minW, int minH) {
    int w = 80, h = 25;
    GetScreenSize(w, h);
    if (w < minW || h < minH) {
        ClearScreen();
        int midY = (std::max)(1, h / 2 - 1);
        MoveCursor(midY, 1);
        std::string warn1 = std::format("[!] 终端窗口尺寸过小 (当前: {}x{}, 推荐最低: {}x{})", w, h, minW, minH);
        std::string warn2 = "请拉大终端窗口以恢复界面显示... (Please enlarge terminal window)";
        std::cout << "\x1b[1;33m" << Pad(warn1, static_cast<size_t>(w), true) << "\x1b[0m\n";
        MoveCursor(midY + 1, 1);
        std::cout << "\x1b[90m" << Pad(warn2, static_cast<size_t>(w), true) << "\x1b[0m";
        std::cout.flush();
        return false;
    }
    return true;
}

namespace {
    bool IsWideCodePoint(uint32_t cp) {
        if (cp < 0x1100) return false;
        return (cp >= 0x1100 && cp <= 0x115F) ||
               (cp >= 0x2329 && cp <= 0x232A) ||
               (cp >= 0x2E80 && cp <= 0x303E) ||
               (cp >= 0x3040 && cp <= 0xA4CF) ||
               (cp >= 0xAC00 && cp <= 0xD7A3) ||
               (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFE10 && cp <= 0xFE19) ||
               (cp >= 0xFE30 && cp <= 0xFE6F) ||
               (cp >= 0xFF00 && cp <= 0xFF60) ||
               (cp >= 0xFFE0 && cp <= 0xFFE6) ||
               (cp >= 0x1F300 && cp <= 0x1F9FF) || // Emojis
               (cp >= 0x20000 && cp <= 0x3FFFD);
    }

    size_t NextCodePoint(std::string_view s, size_t pos, uint32_t& outCp) {
        if (pos >= s.size()) return 0;
        unsigned char c0 = static_cast<unsigned char>(s[pos]);
        if (c0 < 0x80) {
            outCp = c0;
            return 1;
        } else if ((c0 & 0xE0) == 0xC0 && pos + 1 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
            outCp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
            return 2;
        } else if ((c0 & 0xF0) == 0xE0 && pos + 2 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
            outCp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            return 3;
        } else if ((c0 & 0xF8) == 0xF0 && pos + 3 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[pos + 3]);
            outCp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            return 4;
        }
        outCp = c0;
        return 1;
    }
} // namespace

size_t TuiEngine::GetDisplayWidth(std::string_view utf8Str) {
    size_t width = 0;
    size_t pos = 0;
    uint32_t cp = 0;
    while (pos < utf8Str.size()) {
        size_t len = NextCodePoint(utf8Str, pos, cp);
        if (len == 0) break;
        pos += len;
        width += IsWideCodePoint(cp) ? 2 : 1;
    }
    return width;
}

std::string TuiEngine::TruncateToWidth(std::string_view utf8Str, size_t maxWidth) {
    size_t width = 0;
    size_t pos = 0;
    uint32_t cp = 0;
    while (pos < utf8Str.size()) {
        size_t len = NextCodePoint(utf8Str, pos, cp);
        if (len == 0) break;
        size_t charWidth = IsWideCodePoint(cp) ? 2 : 1;
        if (width + charWidth > maxWidth) {
            break;
        }
        width += charWidth;
        pos += len;
    }
    return std::string{utf8Str.substr(0, pos)};
}

std::string TuiEngine::Pad(std::string_view utf8Str, size_t targetWidth, bool center) {
    size_t curWidth = GetDisplayWidth(utf8Str);
    if (curWidth >= targetWidth) {
        return TruncateToWidth(utf8Str, targetWidth);
    }
    size_t padTotal = targetWidth - curWidth;
    if (center) {
        size_t leftPad = padTotal / 2;
        size_t rightPad = padTotal - leftPad;
        return std::string(leftPad, ' ') + std::string{utf8Str} + std::string(rightPad, ' ');
    } else {
        return std::string{utf8Str} + std::string(padTotal, ' ');
    }
}

void TuiEngine::PrintBounded(int row, int col, std::string_view text, size_t maxWidth, std::string_view ansiStyle) {
    if (maxWidth == 0) return;
    MoveCursor(row, col);
    std::string truncated = TruncateToWidth(text, maxWidth);
    std::string padded = Pad(truncated, maxWidth);
    if (!ansiStyle.empty()) {
        std::cout << ansiStyle << padded << "\x1b[0m";
    } else {
        std::cout << padded;
    }
}

void TuiEngine::DrawHeader(std::string_view title, std::string_view statusTag) {
    int w = 80, h = 25;
    GetScreenSize(w, h);

    MoveCursor(1, 1);
    std::string tagStr = statusTag.empty() ? "" : std::format("[{}]", statusTag);
    size_t tagWidth = GetDisplayWidth(tagStr);
    size_t availTitle = (w > static_cast<int>(tagWidth) + 4) ? (w - tagWidth - 4) : 20;

    std::string truncatedTitle = TruncateToWidth(title, availTitle);
    size_t titleWidth = GetDisplayWidth(truncatedTitle);

    size_t spaceBetween = 2;
    if (w > static_cast<int>(titleWidth + tagWidth + 2)) {
        spaceBetween = w - titleWidth - tagWidth - 2;
    }

    std::cout << "\x1b[1;37;44m " << truncatedTitle
              << std::string(spaceBetween, ' ')
              << tagStr << " \x1b[0m";
}

void TuiEngine::DrawFooter(std::string_view shortcuts) {
    int w = 80, h = 25;
    GetScreenSize(w, h);

    MoveCursor(h, 1);
    std::string padded = Pad(std::format(" {}", shortcuts), static_cast<size_t>(w));
    std::cout << "\x1b[1;30;47m" << padded << "\x1b[0m";
    std::cout.flush();
}

void TuiEngine::DrawBox(int top, int left, int width, int height, std::string_view title) {
    if (width < 4 || height < 3) return;
    if (top < 1) top = 1;
    if (left < 1) left = 1;

    // Top border
    MoveCursor(top, left);
    std::cout << "\x1b[36m╭";
    if (title.empty()) {
        for (int i = 0; i < width - 2; ++i) std::cout << "─";
    } else {
        std::string titleFormatted = std::format(" [ {} ] ", title);
        size_t titleW = GetDisplayWidth(titleFormatted);
        if (titleW + 4 <= static_cast<size_t>(width)) {
            std::cout << "─" << "\x1b[1;37m" << titleFormatted << "\x1b[0;36m";
            for (size_t i = 0; i < width - 2 - 1 - titleW; ++i) std::cout << "─";
        } else {
            for (int i = 0; i < width - 2; ++i) std::cout << "─";
        }
    }
    std::cout << "╮\x1b[0m";

    // Side borders and clear interior with spaces
    std::string interiorSpaces(static_cast<size_t>(width - 2), ' ');
    for (int r = 1; r < height - 1; ++r) {
        MoveCursor(top + r, left);
        std::cout << "\x1b[36m│\x1b[0m" << interiorSpaces << "\x1b[36m│\x1b[0m";
    }

    // Bottom border
    MoveCursor(top + height - 1, left);
    std::cout << "\x1b[36m╰";
    for (int i = 0; i < width - 2; ++i) std::cout << "─";
    std::cout << "╯\x1b[0m";
}

bool TuiEngine::ShowConfirmModal(std::string_view title,
                                 std::string_view question,
                                 std::string_view detail,
                                 bool defaultYes) {
    if (!g_tuiActive) {
        std::cout << "\n[" << title << "] " << question;
        if (!detail.empty()) std::cout << "\n" << detail;
        std::cout << "\n确认? [" << (defaultYes ? "Y/n" : "y/N") << "]: ";
        std::cout.flush();
        std::string s;
        if (std::getline(std::cin, s)) {
            s = std::string(TrimWhitespace(s));
            if (s.empty()) return defaultYes;
            return (s[0] == 'y' || s[0] == 'Y');
        }
        return defaultYes;
    }

    int w = 80, h = 25;
    GetScreenSize(w, h);

    size_t maxTextW = std::max(GetDisplayWidth(question), GetDisplayWidth(detail));
    maxTextW = std::max(maxTextW, GetDisplayWidth(title) + 8);
    const int modalW = std::clamp(static_cast<int>(maxTextW) + 12, 60, w - 4);
    const int modalH = detail.empty() ? 7 : 9;
    const int top = (h - modalH) / 2;
    const int left = (w - modalW) / 2;

    // Clear modal area
    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        std::cout << std::string(modalW, ' ');
    }

    // Draw box
    DrawBox(top, left, modalW, modalH, title);

    // Question line
    MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;37m" << Pad(TruncateToWidth(question, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";

    // Detail line
    if (!detail.empty()) {
        MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << Pad(TruncateToWidth(detail, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";
    }

    bool selectedYes = defaultYes;

    auto renderButtons = [&]() {
        MoveCursor(top + modalH - 2, left + 4);
        std::string_view btnYes = TR(MsgKey::BtnYes);
        std::string_view btnNo = TR(MsgKey::BtnNo);
        if (selectedYes) {
            std::cout << "\x1b[1;97;42m" << btnYes << "\x1b[0m    \x1b[90m" << btnNo << "\x1b[0m";
        } else {
            std::cout << "\x1b[90m" << btnYes << "\x1b[0m    \x1b[1;97;41m" << btnNo << "\x1b[0m";
        }
        size_t buttonsW = GetDisplayWidth(btnYes) + GetDisplayWidth(btnNo) + 4;
        if (static_cast<size_t>(modalW - 8) > buttonsW) {
            std::cout << std::string(static_cast<size_t>(modalW - 8) - buttonsW, ' ');
        }
        std::cout.flush();
    };

    renderButtons();

    FlushInputBuffer();

    while (true) {
        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Left || ev.code == KeyCode::Right || ev.code == KeyCode::Tab) {
            selectedYes = !selectedYes;
            renderButtons();
            continue;
        }
        if (ev.code == KeyCode::Char) {
            if (ev.ch == 'y' || ev.ch == 'Y') {
                FlushInputBuffer();
                return true;
            }
            if (ev.ch == 'n' || ev.ch == 'N') {
                FlushInputBuffer();
                return false;
            }
        } else if (ev.code == KeyCode::Enter) {
            FlushInputBuffer();
            return selectedYes;
        } else if (ev.code == KeyCode::Escape) {
            FlushInputBuffer();
            return false;
        }
    }
}

void TuiEngine::FlushInputBuffer() noexcept {
    while (_kbhit()) {
        (void)_getch();
    }
}

bool TuiEngine::ShowMessageModal(std::string_view title,
                                 std::string_view message,
                                 std::string_view detail) {
    if (!g_tuiActive) {
        std::cout << "\n[" << title << "] " << message;
        if (!detail.empty()) std::cout << "\n" << detail;
        std::cout << "\n按 Enter 键继续...";
        std::cout.flush();
        std::string s;
        std::getline(std::cin, s);
        return true;
    }

    int w = 80, h = 25;
    GetScreenSize(w, h);

    size_t maxTextW = std::max(GetDisplayWidth(message), GetDisplayWidth(detail));
    maxTextW = std::max(maxTextW, GetDisplayWidth(title) + 8);
    const int modalW = std::clamp(static_cast<int>(maxTextW) + 12, 56, w - 4);
    const int modalH = detail.empty() ? 7 : 9;
    const int top = (h - modalH) / 2;
    const int left = (w - modalW) / 2;

    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        std::cout << std::string(modalW, ' ');
    }

    DrawBox(top, left, modalW, modalH, title);

    MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;37m" << Pad(TruncateToWidth(message, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";

    if (!detail.empty()) {
        MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << Pad(TruncateToWidth(detail, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";
    }

    MoveCursor(top + modalH - 2, left + 4);
    std::string_view btnClose = TR(MsgKey::BtnClose);
    std::cout << "\x1b[1;97;44m" << btnClose << "\x1b[0m";
    size_t btnW = GetDisplayWidth(btnClose);
    if (static_cast<size_t>(modalW - 8) > btnW) {
        std::cout << std::string(static_cast<size_t>(modalW - 8) - btnW, ' ');
    }
    std::cout.flush();

    while (true) {
        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Enter) {
            FlushInputBuffer();
            return true;
        }
        if (ev.code == KeyCode::Escape) {
            FlushInputBuffer();
            return false;
        }
    }
}

std::optional<std::string> TuiEngine::PromptInputModal(std::string_view title,
                                                       std::string_view prompt,
                                                       std::string_view defaultValue,
                                                       bool isPassword) {
    if (!g_tuiActive) {
        if (isPassword) {
            std::string promptStr = std::string(prompt);
            SecureString pwd = ReadPasswordFromConsole(promptStr.c_str());
            return pwd.Empty() ? std::nullopt : std::optional<std::string>(std::string(pwd.Data()));
        }
        std::cout << "\n" << prompt;
        if (!defaultValue.empty()) {
            std::cout << " [" << defaultValue << "]";
        }
        std::cout << ": ";
        std::cout.flush();
        std::string s;
        if (std::getline(std::cin, s)) {
            if (s.empty() && !defaultValue.empty()) s = defaultValue;
            return s;
        }
        return std::nullopt;
    }

    int w = 80, h = 25;
    GetScreenSize(w, h);

    size_t maxTextW = std::max(GetDisplayWidth(prompt), GetDisplayWidth(title) + 8);
    const int modalW = std::clamp(static_cast<int>(maxTextW) + 16, 58, w - 4);
    const int modalH = 8;
    const int top = (h - modalH) / 2;
    const int left = (w - modalW) / 2;

    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        std::cout << std::string(modalW, ' ');
    }

    DrawBox(top, left, modalW, modalH, title);

    MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;37m" << TruncateToWidth(prompt, modalW - 8) << "\x1b[0m";

    MoveCursor(top + modalH - 2, left + 4);
    std::cout << "\x1b[90m" << TR(MsgKey::HintInputEnterEsc) << "\x1b[0m";

    std::string value{defaultValue};
    const int inputTop = top + 4;
    const int inputLeft = left + 4;
    const int inputWidth = modalW - 8;

    while (true) {
        MoveCursor(inputTop, inputLeft);
        std::string displayVal;
        if (isPassword) {
            displayVal = std::string(value.size(), '*');
        } else {
            displayVal = value;
        }

        const size_t visibleWidth = static_cast<size_t>((std::max)(4, inputWidth - 4));
        std::string visibleText;
        if (GetDisplayWidth(displayVal) > visibleWidth) {
            // Horizontally scroll to show tail while user is typing
            visibleText = displayVal.substr(displayVal.size() - visibleWidth);
        } else {
            visibleText = displayVal;
        }
        std::string boxContent = std::format("[ {} ]", Pad(visibleText, visibleWidth));
        std::cout << "\x1b[30;107m" << boxContent << "\x1b[0m";
        std::cout.flush();

        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Escape) {
            if (isPassword && !value.empty()) {
                SecureZeroMemory(value.data(), value.size());
            }
            FlushInputBuffer();
            return std::nullopt;
        }
        if (ev.code == KeyCode::Enter) {
            std::string result = std::move(value);
            if (isPassword && !value.empty()) {
                SecureZeroMemory(value.data(), value.size());
            }
            FlushInputBuffer();
            return result;
        }
        if (ev.code == KeyCode::Backspace) {
            if (!value.empty()) {
                value.pop_back();
            }
        } else if (ev.code == KeyCode::Char) {
            if (ev.ch >= 32 && ev.ch < 127 && value.size() < 128) {
                value.push_back(ev.ch);
            }
        }
    }
}

void TuiEngine::DrawProgressBar(int row, int col, int width,
                                size_t current, size_t total,
                                std::string_view label) {
    if (width < 20) return;
    const double pct = (total == 0) ? 1.0 : std::clamp(static_cast<double>(current) / static_cast<double>(total), 0.0, 1.0);
    const int percentInt = static_cast<int>(pct * 100.0);

    const int barWidth = std::clamp(width - 25, 10, 32);
    const int filled = static_cast<int>(pct * barWidth);

    std::string barStr = "[" + std::string(filled, '=') + (filled < barWidth ? ">" : "") + std::string(barWidth - filled - (filled < barWidth ? 1 : 0), ' ') + "]";
    std::string statsStr = std::format(" {:>3}% ({}/{}) ", percentInt, current, total);
    size_t prefixW = static_cast<size_t>(barWidth) + GetDisplayWidth(statsStr);

    size_t maxLabelW = (static_cast<size_t>(width) > prefixW) ? (static_cast<size_t>(width) - prefixW) : 0;
    std::string truncatedLabel = TruncateToWidth(label, maxLabelW);
    std::string paddedLabel = Pad(truncatedLabel, maxLabelW);

    MoveCursor(row, col);
    std::cout << "\x1b[1;32m" << barStr << "\x1b[0m"
              << "\x1b[1;37m" << statsStr << "\x1b[0m"
              << "\x1b[90m" << paddedLabel << "\x1b[0m";
    std::cout.flush();
}

KeyEvent TuiEngine::ReadKey() {
    static int s_lastW = 0, s_lastH = 0;
    if (s_lastW == 0 && s_lastH == 0) {
        GetScreenSize(s_lastW, s_lastH);
    }

    while (!_kbhit()) {
        int curW = 0, curH = 0;
        GetScreenSize(curW, curH);
        if (curW != s_lastW || curH != s_lastH) {
            s_lastW = curW;
            s_lastH = curH;
            return { KeyCode::Resize, 0 };
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    int ch = _getch();
    if (ch == 0 || ch == 0xE0) {
        int arrow = _getch();
        switch (arrow) {
            case 0x48: return { KeyCode::Up, 0 };
            case 0x50: return { KeyCode::Down, 0 };
            case 0x4B: return { KeyCode::Left, 0 };
            case 0x4D: return { KeyCode::Right, 0 };
            case 0x53: return { KeyCode::Delete, 0 };
            default:   return { KeyCode::None, 0 };
        }
    }
    if (ch == 13 || ch == 10) return { KeyCode::Enter, 0 };
    if (ch == 27) return { KeyCode::Escape, 0 };
    if (ch == 8 || ch == 127) return { KeyCode::Backspace, 0 };
    if (ch == 9) return { KeyCode::Tab, 0 };

    return { KeyCode::Char, static_cast<char>(ch) };
}

} // namespace OST::ExtractTickets
