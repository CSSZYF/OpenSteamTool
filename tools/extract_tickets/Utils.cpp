#include "Utils.h"
#include "Log.h"
#include "RaiiGuards.h"
#include "TuiEngine.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iostream>

namespace OST::ExtractTickets {

std::string_view TrimWhitespace(std::string_view str) noexcept {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    return str;
}

bool IsDecimal(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool IsValidManifestId(std::string_view value) noexcept {
    if (value.empty()) return false;
    bool hasNonZero = false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        if (ch != '0') hasNonZero = true;
    }
    return hasNonZero;
}

bool IsHex64(std::string_view value) noexcept {
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::optional<uint32_t> ParseAppId(std::string_view value) noexcept {
    value = TrimWhitespace(value);
    if (value.empty()) return std::nullopt;
    uint32_t parsed{0};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<uint32_t> ReadAppIdFromConsole() {
    std::cout << "AppID: ";
    std::string input;
    if (!std::getline(std::cin, input)) return std::nullopt;
    return ParseAppId(input);
}

std::string SanitizeComment(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return out;
}

std::vector<std::string> TokenizeQuoted(std::string_view line) {
    std::vector<std::string> tokens;
    tokens.reserve(4);

    size_t pos = 0;
    while (pos < line.size()) {
        pos = line.find('"', pos);
        if (pos == std::string_view::npos) break;

        std::string token;
        token.reserve(32);
        size_t i = pos + 1;
        bool closed = false;

        while (i < line.size()) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                if (line[i + 1] == '"') {
                    token.push_back('"');
                    i += 2;
                    continue;
                }
                if (line[i + 1] == '\\') {
                    token.push_back('\\');
                    i += 2;
                    continue;
                }
            }
            if (line[i] == '"') {
                closed = true;
                pos = i + 1;
                break;
            }
            token.push_back(line[i]);
            ++i;
        }

        if (!closed) break;
        tokens.push_back(std::move(token));
    }

    return tokens;
}

std::string ToHexString(std::span<const uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return out;
}

void PrintHex(const char* label, std::span<const uint8_t> data) {
    if (TuiEngine::IsActive()) return;
    std::cout << label << " (" << data.size() << " bytes):\n";

    constexpr size_t kBytesPerRow = 16;
    static constexpr char kHex[] = "0123456789abcdef";

    for (size_t row = 0; row < data.size(); row += kBytesPerRow) {
        std::string line;
        for (int shift = 12; shift >= 0; shift -= 4) {
            line += kHex[(row >> shift) & 0xF];
        }
        line += "  ";

        std::string ascii;
        for (size_t col = 0; col < kBytesPerRow; ++col) {
            if (row + col < data.size()) {
                const uint8_t byte = data[row + col];
                line += kHex[byte >> 4];
                line += kHex[byte & 0xF];
                line += ' ';
                ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
            } else {
                line += "   ";
            }
            if (col == 7) line += ' ';
        }

        std::cout << line << " " << ascii << "\n";
    }
}

bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> data) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "Failed to create " << path.string() << ".\n";
        }
        LOG_ERROR("Utils", "创建文件失败: {}", path.string());
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!output) {
        if (!TuiEngine::IsActive()) {
            std::cerr << "Failed to write " << path.string() << ".\n";
        }
        LOG_ERROR("Utils", "写入文件失败: {}", path.string());
        return false;
    }
    return true;
}

std::string JoinPath(std::string_view base, std::string_view name) {
    std::string result;
    result.reserve(base.size() + 1 + name.size());
    result.append(base);
    std::replace(result.begin(), result.end(), '/', '\\');
    if (!result.empty() && result.back() != '\\') {
        result.push_back('\\');
    }
    const size_t nameStart = result.size();
    result.append(name);
    std::replace(result.begin() + nameStart, result.end(), '/', '\\');
    return result;
}

std::string NormalizeDir(std::string dir) {
    std::replace(dir.begin(), dir.end(), '/', '\\');
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir;
}

namespace {
    std::wstring Utf8ToWide(std::string_view utf8) {
        if (utf8.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), len);
        return w;
    }

    std::string WideToUtf8(std::wstring_view wide) {
        if (wide.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), s.data(), len, nullptr, nullptr);
        return s;
    }
} // namespace

std::optional<std::string> QueryRegistryString(HKEY root, const char* subKey, const char* valueName) {
    std::wstring wSubKey = Utf8ToWide(subKey ? subKey : "");
    std::wstring wValueName = Utf8ToWide(valueName ? valueName : "");

    HKEY rawKey{nullptr};
    LSTATUS status = RegOpenKeyExW(root, wSubKey.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &rawKey);
    if (status != ERROR_SUCCESS) {
        status = RegOpenKeyExW(root, wSubKey.c_str(), 0, KEY_READ, &rawKey);
    }
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    ScopedHKey key{rawKey};

    DWORD valueType{0};
    DWORD byteSize{0};
    status = RegQueryValueExW(key, wValueName.c_str(), nullptr, &valueType, nullptr, &byteSize);
    if (status != ERROR_SUCCESS || (valueType != REG_SZ && valueType != REG_EXPAND_SZ) || byteSize == 0) {
        return std::nullopt;
    }

    std::wstring wValue(byteSize / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(
        key,
        wValueName.c_str(),
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(wValue.data()),
        &byteSize);

    if (status != ERROR_SUCCESS) return std::nullopt;
    wValue.resize(byteSize / sizeof(wchar_t));
    while (!wValue.empty() && (wValue.back() == L'\0' || wValue.back() == L' ')) {
        wValue.pop_back();
    }

    if (valueType == REG_EXPAND_SZ) {
        wchar_t expanded[MAX_PATH * 2]{0};
        DWORD expLen = ExpandEnvironmentStringsW(wValue.c_str(), expanded, static_cast<DWORD>(sizeof(expanded) / sizeof(wchar_t)));
        if (expLen > 0 && expLen < sizeof(expanded) / sizeof(wchar_t)) {
            wValue = expanded;
        }
    }

    return WideToUtf8(wValue);
}

std::optional<std::string> FindSteamInstallPath() {
    constexpr const char* kSteamKey = "Software\\Valve\\Steam";

    if (auto path = QueryRegistryString(HKEY_CURRENT_USER, kSteamKey, "SteamPath")) {
        std::string norm = NormalizeDir(*path);
        LOG_DEBUG("SteamPath", "Found SteamPath in HKEY_CURRENT_USER: {}", norm);
        return norm;
    }

    if (auto path = QueryRegistryString(HKEY_LOCAL_MACHINE, kSteamKey, "InstallPath")) {
        std::string norm = NormalizeDir(*path);
        LOG_DEBUG("SteamPath", "Found InstallPath in HKEY_LOCAL_MACHINE: {}", norm);
        return norm;
    }

    const wchar_t* defaultPaths[] = {
        L"C:\\Program Files (x86)\\Steam",
        L"C:\\Program Files\\Steam"
    };
    for (const wchar_t* p : defaultPaths) {
        std::filesystem::path checkExe = std::filesystem::path(p) / L"steam.exe";
        DWORD attr = GetFileAttributesW(checkExe.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string norm = NormalizeDir(WideToUtf8(p));
            LOG_DEBUG("SteamPath", "Found Steam install path at default location: {}", norm);
            return norm;
        }
    }

    return std::nullopt;
}

} // namespace OST::ExtractTickets
