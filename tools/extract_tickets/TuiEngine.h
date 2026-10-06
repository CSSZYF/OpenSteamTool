#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

enum class KeyCode {
    None,
    Char,
    Up,
    Down,
    Left,
    Right,
    Enter,
    Escape,
    Backspace,
    Delete,
    Tab,
    Resize,
};

struct KeyEvent {
    KeyCode code{KeyCode::None};
    char ch{0};
};

class TuiEngine {
public:
    // Terminal setup and RAII alternate buffer
    static void EnableVirtualTerminal();
    static void EnterAlternateScreen();
    static void ExitAlternateScreen();
    static void ShowCursor(bool show);
    static void MoveCursor(int row, int col);
    static void ClearScreen();
    static void RepositionCursor();

    // Screen dimensions
    static void GetScreenSize(int& outWidth, int& outHeight);
    [[nodiscard]] static bool EnsureMinTerminalSize(int minW = 76, int minH = 20);

    // Text formatting and East Asian width alignment
    static size_t GetDisplayWidth(std::string_view utf8Str);
    static std::string TruncateToWidth(std::string_view utf8Str, size_t maxWidth);
    static std::string Pad(std::string_view utf8Str, size_t targetWidth, bool center = false);
    static void PrintBounded(int row, int col, std::string_view text, size_t maxWidth, std::string_view ansiStyle = "");

    // Box and UI drawing
    static void DrawHeader(std::string_view title, std::string_view statusTag);
    static void DrawFooter(std::string_view shortcuts);
    static void DrawBox(int top, int left, int width, int height, std::string_view title = "");
    
    // Modal confirmation dialog (returns true for Yes, false for No)
    static bool ShowConfirmModal(std::string_view title,
                                std::string_view question,
                                std::string_view detail = "",
                                bool defaultYes = false);

    // Modal message alert dialog (returns true for Enter, false for ESC)
    static bool ShowMessageModal(std::string_view title,
                                 std::string_view message,
                                 std::string_view detail = "");

    static void FlushInputBuffer() noexcept;

    // Modal text/password input dialog (returns std::nullopt if cancelled via ESC)
    static std::optional<std::string> PromptInputModal(std::string_view title,
                                                       std::string_view prompt,
                                                       std::string_view defaultValue = "",
                                                       bool isPassword = false);

    // Progress bar inside an existing box or standalone
    static void DrawProgressBar(int row, int col, int width,
                                size_t current, size_t total,
                                std::string_view label = "");

    static bool IsActive() noexcept;
    static void SetActive(bool active) noexcept;
    static void EraseToEndOfLine();

    // Input reading
    static KeyEvent ReadKey();
};

class TuiSessionGuard {
public:
    explicit TuiSessionGuard(bool active = true) : m_active(active) {
        if (m_active) {
            TuiEngine::SetActive(true);
            TuiEngine::EnableVirtualTerminal();
            TuiEngine::EnterAlternateScreen();
            TuiEngine::ShowCursor(false);
            TuiEngine::ClearScreen();
        }
    }

    ~TuiSessionGuard() {
        if (m_active) {
            TuiEngine::ShowCursor(true);
            TuiEngine::ExitAlternateScreen();
            TuiEngine::SetActive(false);
        }
    }

    TuiSessionGuard(const TuiSessionGuard&) = delete;
    TuiSessionGuard& operator=(const TuiSessionGuard&) = delete;

private:
    bool m_active{true};
};

} // namespace OST::ExtractTickets
