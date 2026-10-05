#include "TuiEngine.h"

#include <conio.h>
#include <algorithm>
#include <format>
#include <iostream>

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

void TuiEngine::EnableVirtualTerminal() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
        dwMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
    }
    SetConsoleOutputCP(CP_UTF8);
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
    std::cout << "\x1b[" << row << ";" << col << "H";
}

void TuiEngine::ClearScreen() {
    // Reposition cursor to top-left without clearing the entire buffer to black.
    // Avoids high-frequency terminal screen flashing on keystrokes and updates.
    std::cout << "\x1b[H";
    std::cout.flush();
}

void TuiEngine::EraseToEndOfLine() {
    std::cout << "\x1b[K";
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
    if (outWidth < 80) outWidth = 80;
    if (outHeight < 24) outHeight = 24;
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

    // Side borders
    for (int r = 1; r < height - 1; ++r) {
        MoveCursor(top + r, left);
        std::cout << "\x1b[36m│\x1b[0m";
        MoveCursor(top + r, left + width - 1);
        std::cout << "\x1b[36m│\x1b[0m";
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
    int w = 80, h = 25;
    GetScreenSize(w, h);

    const int modalW = std::clamp(static_cast<int>(GetDisplayWidth(question)) + 12, 56, w - 4);
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
    std::cout << "\x1b[1;37m" << TruncateToWidth(question, modalW - 8) << "\x1b[0m";

    // Detail line
    if (!detail.empty()) {
        MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << TruncateToWidth(detail, modalW - 8) << "\x1b[0m";
    }

    // Options line
    MoveCursor(top + modalH - 2, left + 4);
    if (defaultYes) {
        std::cout << "\x1b[1;32m[ Y: 确定 (默认 Enter) ]\x1b[0m    \x1b[90m[ N: 取消 ]\x1b[0m";
    } else {
        std::cout << "\x1b[90m[ Y: 确定 ]\x1b[0m    \x1b[1;33m[ N: 取消 (默认 Enter) ]\x1b[0m";
    }
    std::cout.flush();

    while (true) {
        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Char) {
            if (ev.ch == 'y' || ev.ch == 'Y') return true;
            if (ev.ch == 'n' || ev.ch == 'N') return false;
        } else if (ev.code == KeyCode::Enter) {
            return defaultYes;
        } else if (ev.code == KeyCode::Escape) {
            return false;
        }
    }
}

void TuiEngine::ShowMessageModal(std::string_view title,
                                 std::string_view message,
                                 std::string_view detail) {
    int w = 80, h = 25;
    GetScreenSize(w, h);

    const int modalW = std::clamp(static_cast<int>(GetDisplayWidth(message)) + 12, 54, w - 4);
    const int modalH = detail.empty() ? 7 : 9;
    const int top = (h - modalH) / 2;
    const int left = (w - modalW) / 2;

    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        std::cout << std::string(modalW, ' ');
    }

    DrawBox(top, left, modalW, modalH, title);

    MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;37m" << TruncateToWidth(message, modalW - 8) << "\x1b[0m";

    if (!detail.empty()) {
        MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << TruncateToWidth(detail, modalW - 8) << "\x1b[0m";
    }

    MoveCursor(top + modalH - 2, left + 4);
    std::cout << "\x1b[1;36m[ 按 Enter 或 ESC 关闭 ]\x1b[0m";
    std::cout.flush();

    while (true) {
        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Enter || ev.code == KeyCode::Escape) {
            break;
        }
    }
}

std::optional<std::string> TuiEngine::PromptInputModal(std::string_view title,
                                                       std::string_view prompt,
                                                       std::string_view defaultValue,
                                                       bool isPassword) {
    int w = 80, h = 25;
    GetScreenSize(w, h);

    const int modalW = std::clamp(static_cast<int>(GetDisplayWidth(prompt)) + 16, 56, w - 4);
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
    std::cout << "\x1b[90m[ Enter: 提交  ESC: 取消 ]\x1b[0m";

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
        std::string truncated = TruncateToWidth(displayVal, static_cast<size_t>(inputWidth - 4));
        std::string boxContent = std::format("[ {} ]", Pad(truncated, static_cast<size_t>(inputWidth - 4)));
        std::cout << "\x1b[1;30;47m" << boxContent << "\x1b[0m";
        std::cout.flush();

        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Escape) {
            return std::nullopt;
        }
        if (ev.code == KeyCode::Enter) {
            return value;
        }
        if (ev.code == KeyCode::Backspace) {
            if (!value.empty()) {
                value.pop_back();
            }
        } else if (ev.code == KeyCode::Char) {
            if (ev.ch >= 32 && ev.ch < 127 && value.size() < static_cast<size_t>(inputWidth - 6)) {
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

    const int barWidth = std::clamp(width - 25, 10, 40);
    const int filled = static_cast<int>(pct * barWidth);

    std::string barStr = "[" + std::string(filled, '=') + (filled < barWidth ? ">" : "") + std::string(barWidth - filled - (filled < barWidth ? 1 : 0), ' ') + "]";

    MoveCursor(row, col);
    std::cout << "\x1b[1;32m" << barStr << " \x1b[1;37m" << std::format("{:>3}% ({}/{}) ", percentInt, current, total)
              << "\x1b[90m" << TruncateToWidth(label, width - barWidth - 18) << "\x1b[0m";
    std::cout.flush();
}

KeyEvent TuiEngine::ReadKey() {
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
