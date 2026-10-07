#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

// String & numeric validation
[[nodiscard]] inline bool EqualIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view TrimWhitespace(std::string_view str) noexcept;
[[nodiscard]] bool IsDecimal(std::string_view value) noexcept;
[[nodiscard]] bool IsValidManifestId(std::string_view value) noexcept;
[[nodiscard]] bool IsHex64(std::string_view value) noexcept;
[[nodiscard]] std::optional<uint32_t> ParseAppId(std::string_view value) noexcept;
[[nodiscard]] std::string SanitizeComment(std::string_view text);
[[nodiscard]] std::vector<std::string> TokenizeQuoted(std::string_view line);

// Hex formatting
[[nodiscard]] std::string ToHexString(std::span<const uint8_t> data);

// File I/O
bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> data);

// Unicode & UTF-8 Path helpers
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide);
[[nodiscard]] inline std::filesystem::path Utf8Path(std::string_view utf8) {
    return std::filesystem::path(Utf8ToWide(utf8));
}

// Path & Registry
[[nodiscard]] std::string JoinPath(std::string_view base, std::string_view name);
[[nodiscard]] std::string NormalizeDir(std::string dir);
[[nodiscard]] std::optional<std::string> QueryRegistryString(HKEY root, const char* subKey, const char* valueName);
[[nodiscard]] std::optional<std::string> FindSteamInstallPath();

} // namespace OST::ExtractTickets
