#pragma once

#ifndef _WIN64
#error "OpenSteamTool Injector is strictly designed for 64-bit Windows architecture (x64)."
#endif

#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

namespace Injector {

    enum class Status {
        Success,
        AlreadyInjected,
        TargetExited,
        ComponentTimeout,
        ProcessAccessDenied,
        RemoteAllocFailed,
        RemoteThreadFailed,
        LoadLibraryFailed,
        PayloadNotFound,
        IncompatibleArchitecture
    };

    struct ExecutionResult {
        Status status = Status::Success;
        DWORD win32Error = 0;
        std::string message;

        [[nodiscard]] bool IsOk() const noexcept {
            return status == Status::Success || status == Status::AlreadyInjected;
        }
    };

    struct RuntimeConfig {
        std::filesystem::path targetExe;
        std::filesystem::path payloadDll;
        std::filesystem::path baseDir;
    };

    // Configuration & Environment
    RuntimeConfig ResolveConfig(const std::filesystem::path& baseDir);

    // Process & Module Inspection
    bool IsModulePresent(DWORD pid, std::wstring_view moduleName);
    std::vector<DWORD> SnapshotProcessIds(std::wstring_view processName);
    void PrioritizeCandidatePids(std::vector<DWORD>& pids);

    // Core Injection & Lifecycle Orchestration
    ExecutionResult InjectPayload(DWORD pid, const std::filesystem::path& dllPath, bool isSilent = false);
    ExecutionResult AwaitAndInject(DWORD pid, const std::filesystem::path& dllPath,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(30),
                                  bool isSilent = false);

    // Execution Modes
    int RunWatcher(const RuntimeConfig& config);
    int RunSilentOnce(const RuntimeConfig& config);
    int RunInteractive(const RuntimeConfig& config);

    // UI & Logging
    void LogMessage(const std::filesystem::path& baseDir, const std::string& msg, bool isSilent = false);
    void ShowErrorAlert(const std::wstring& message);
    std::wstring Utf8ToWide(std::string_view utf8);

} // namespace Injector
