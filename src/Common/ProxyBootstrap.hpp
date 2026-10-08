#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace OST::Proxy {

/**
 * @brief Defensive system DLL loader.
 * Loads original DLL directly from the Windows System32 directory using LOAD_WITH_ALTERED_SEARCH_PATH,
 * completely immune to local DLL sideloading and recursive self-loading.
 */
class SystemDllLoader {
public:
    [[nodiscard]] static HMODULE Load(std::wstring_view dllName) noexcept {
        wchar_t sysDir[MAX_PATH];
        const UINT len = ::GetSystemDirectoryW(sysDir, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return nullptr;
        }

        std::wstring fullPath;
        fullPath.reserve(static_cast<size_t>(len) + 1 + dllName.size());
        fullPath.append(sysDir, len);
        if (fullPath.back() != L'\\') {
            fullPath.push_back(L'\\');
        }
        fullPath.append(dllName);

        return ::LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
};

/**
 * @brief Unified proxy DLL bootstrap and process filtering anchor.
 * Manages thread library call disabling, host process inspection, and guarded payload injection.
 */
class Bootstrap {
public:
    [[nodiscard]] static bool EqualIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) {
                return false;
            }
        }
        return true;
    }

    static BOOL OnAttach(
        HMODULE hModule,
        std::wstring_view targetHost = L"steam.exe",
        std::wstring_view payloadDll = L"OpenSteamTool.dll") noexcept {

        ::DisableThreadLibraryCalls(hModule);

        wchar_t exePath[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const std::wstring_view fullPath(exePath, len);
            const auto lastSlash = fullPath.find_last_of(L"\\/");
            const std::wstring_view exeName = (lastSlash != std::wstring_view::npos)
                ? fullPath.substr(lastSlash + 1)
                : fullPath;

            // If not target host, allow the proxy DLL to load peacefully without injecting
            if (!EqualIgnoreCase(exeName, targetHost)) {
                return TRUE;
            }
        }

        // Host is verified: inject payload DLL
        // Windows loader guarantees that LoadLibraryW on the same module in the same process
        // runs DllMain only once, ensuring thread-safe idempotent injection.
        std::wstring payload(payloadDll);
        return ::LoadLibraryW(payload.c_str()) != nullptr ? TRUE : FALSE;
    }
};

} // namespace OST::Proxy
