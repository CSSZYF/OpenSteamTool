#include "SteamPlayWatcher.h"

#include "dllmain.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace SteamPlayWatcher {
namespace {
std::atomic<bool> g_startAttempted{false};
}

void Start() {
    if (g_startAttempted.exchange(true)) return;
    try {
        if (DllDir[0] == '\0') return;
        const auto directory = OSTPlatform::Encoding::PathFromUtf8(DllDir) / L"dave-launcher";
        const auto helper = directory / L"DaveLauncher.exe";
        const auto gate = directory / L"SteamPlayGate.js";
        std::error_code error;
        if (!helper.is_absolute() || !std::filesystem::is_regular_file(helper, error) || error) {
            LOG_MISC_DEBUG("SteamPlayWatcher: optional companion is not installed; watcher not started");
            return;
        }
        error.clear();
        if (!std::filesystem::is_regular_file(gate, error) || error) {
            LOG_MISC_DEBUG("SteamPlayWatcher: optional Play gate is not installed; watcher not started");
            return;
        }

        std::wstring command = L"\"" + helper.wstring() + L"\" --watch-play " +
            std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
            const DWORD failure = GetLastError();
            LOG_MISC_WARN("SteamPlayWatcher: companion could not start (Windows error={})", failure);
            return;
        }
        const DWORD watcherPid = process.dwProcessId;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        LOG_MISC_INFO("SteamPlayWatcher: started optional Play gate companion pid={}", watcherPid);
    }
    catch (...) {
        LOG_MISC_WARN("SteamPlayWatcher: companion startup preparation failed");
    }
}

} // namespace SteamPlayWatcher
