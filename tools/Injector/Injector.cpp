#include "Injector.h"

#if __has_include(<OSTPlatform/SteamLocator.hpp>)
#include <OSTPlatform/SteamLocator.hpp>
#else
#include "../../src/OSTPlatform/include/OSTPlatform/SteamLocator.hpp"
#endif

#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <set>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <optional>
#include <utility>
#include <cwctype>
#include <atomic>

namespace Injector {

namespace {

    // Move-only RAII Windows handle wrapper
    class ScopedHandle {
    public:
        constexpr ScopedHandle() noexcept = default;
        explicit ScopedHandle(HANDLE h) noexcept : handle_(IsValid(h) ? h : nullptr) {}
        ~ScopedHandle() noexcept { Reset(); }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
            other.handle_ = nullptr;
        }
        ScopedHandle& operator=(ScopedHandle&& other) noexcept {
            if (this != &other) {
                Reset(other.handle_);
                other.handle_ = nullptr;
            }
            return *this;
        }

        [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
        [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr; }
        explicit operator bool() const noexcept { return IsValid(); }

        void Reset(HANDLE h = nullptr) noexcept {
            if (handle_) {
                ::CloseHandle(handle_);
            }
            handle_ = IsValid(h) ? h : nullptr;
        }

        HANDLE* Put() noexcept {
            Reset();
            return &handle_;
        }

    private:
        static bool IsValid(HANDLE h) noexcept {
            return h != nullptr && h != INVALID_HANDLE_VALUE;
        }
        HANDLE handle_ = nullptr;
    };

    // Lightweight ScopeGuard for RAII cleanups with dismiss capability
    template <typename F>
    class ScopeGuard {
    public:
        explicit ScopeGuard(F&& func) : func_(std::forward<F>(func)) {}
        ~ScopeGuard() noexcept {
            if (active_) func_();
        }

        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
        ScopeGuard(ScopeGuard&& other) noexcept : func_(std::move(other.func_)), active_(other.active_) {
            other.active_ = false;
        }

        void Dismiss() noexcept { active_ = false; }

    private:
        F func_;
        bool active_ = true;
    };

    // Global stop flag for graceful termination of background services
    std::atomic<bool> g_stopRequested{ false };

    BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
        switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true);
            return TRUE;
        default:
            return FALSE;
        }
    }

    // Single-instance protection for the background watcher daemon
    class SingleInstanceGuard {
    public:
        SingleInstanceGuard() {
            // Using Local namespace guarantees seamless execution under standard user privileges
            // without requiring SeCreateGlobalPrivilege.
            hMutex_.Reset(CreateMutexW(nullptr, TRUE, L"Local\\OpenSteamTool_AutoInject_Watcher"));
            const DWORD gle = GetLastError();
            isAlreadyRunning_ = (gle == ERROR_ALREADY_EXISTS);
        }
    
        [[nodiscard]] bool IsConflict() const noexcept {
            return isAlreadyRunning_ || !hMutex_.IsValid();
        }

    private:
        ScopedHandle hMutex_;
        bool isAlreadyRunning_ = false;
    };

    // Best-effort privilege elevation for administrator sessions
    void EnableDebugPrivilege() {
        ScopedHandle hToken;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, hToken.Put())) {
            TOKEN_PRIVILEGES tp{};
            if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid)) {
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hToken.Get(), FALSE, &tp, sizeof(tp), nullptr, nullptr);
            }
        }
    }

    // Query Steam installation executable with 5-tier fast short-circuit detection
    std::optional<std::filesystem::path> QuerySteamRegistryPath() {
        if (auto dir = OSTPlatform::SteamLocator::ResolvePath()) {
            return *dir / "steam.exe";
        }
        return std::nullopt;
    }

    // Attach or allocate console for interactive sessions and force UTF-8 output
    void EnsureInteractiveConsole() {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD fileType = (hOut != NULL && hOut != INVALID_HANDLE_VALUE) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
        if (fileType == FILE_TYPE_UNKNOWN) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                AllocConsole();
            }
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
        }
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::ios::sync_with_stdio(false);
    }

    // Successor process detection with active liveness verification
    std::optional<DWORD> PollSuccessorProcess(DWORD oldPid, std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_stopRequested.load()) return std::nullopt;

            auto pids = SnapshotProcessIds(L"steam.exe");
            PrioritizeCandidatePids(pids);
            for (DWORD pid : pids) {
                if (pid != oldPid) {
                    // Verify candidate is an active living process, avoiding zombie handles
                    ScopedHandle hCheck(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
                    if (hCheck) {
                        DWORD exitCode = 0;
                        if (GetExitCodeProcess(hCheck.Get(), &exitCode) && exitCode == STILL_ACTIVE) {
                            return pid;
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        return std::nullopt;
    }

    void PrintUsage() {
        std::cout << "OpenSteamTool Injector (ost-Injector)\n";
        std::cout << "Usage: ost-Injector.exe [options]\n\n";
        std::cout << "Options:\n";
        std::cout << "  -watch, --watch, -daemon, /watch  Run as background auto-injection watcher\n";
        std::cout << "  -silent, --silent, -s, /s         Perform silent injection once and exit\n";
        std::cout << "  -help, --help, -h, /?             Display this help message and exit\n";
    }

} // namespace

// ============================================================================
// Public Interface Implementations
// ============================================================================

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

[[nodiscard]] inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}

bool IsModulePresent(DWORD pid, std::wstring_view moduleName) {
    // Retry on ERROR_BAD_LENGTH or ERROR_PARTIAL_COPY when target process dynamic module list is transient
    for (int attempt = 0; attempt < 3; ++attempt) {
        ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
        if (!snap) {
            DWORD gle = GetLastError();
            if (gle == ERROR_BAD_LENGTH || gle == ERROR_PARTIAL_COPY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            return false;
        }

        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap.Get(), &me)) {
            do {
                if (EqualsIgnoreCase(moduleName, me.szModule)) {
                    return true;
                }
            } while (Module32NextW(snap.Get(), &me));
        }
        break;
    }
    return false;
}

std::vector<DWORD> SnapshotProcessIds(std::wstring_view processName) {
    std::vector<DWORD> pids;
    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return pids;

    DWORD currentSessionId = 0;
    const bool hasSession = ProcessIdToSessionId(GetCurrentProcessId(), &currentSessionId);

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap.Get(), &pe)) {
        do {
            if (EqualsIgnoreCase(processName, pe.szExeFile)) {
                if (hasSession) {
                    DWORD targetSessionId = 0;
                    if (ProcessIdToSessionId(pe.th32ProcessID, &targetSessionId) &&
                        targetSessionId != currentSessionId) {
                        continue; // Skip processes from other terminal sessions (Session 0, other users)
                    }
                }
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap.Get(), &pe));
    }
    return pids;
}

void PrioritizeCandidatePids(std::vector<DWORD>& pids) {
    if (pids.size() <= 1) return;

    struct PidScore {
        DWORD pid = 0;
        int score = 0;
        ULONGLONG creationTime = 0;
    };

    std::vector<PidScore> scored;
    scored.reserve(pids.size());

    for (DWORD pid : pids) {
        int score = 0;
        ULONGLONG createTime = 0;

        // Single snapshot to inspect all key modules at once (reducing overhead by ~66%)
        bool hasClient = false;
        bool hasUI = false;
        bool hasTier0 = false;

        ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
        if (snap) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap.Get(), &me)) {
                do {
                    if (!hasClient && EqualsIgnoreCase(L"steamclient64.dll", me.szModule)) {
                        hasClient = true;
                    } else if (!hasUI && EqualsIgnoreCase(L"steamui.dll", me.szModule)) {
                        hasUI = true;
                    } else if (!hasTier0 && EqualsIgnoreCase(L"tier0_s64.dll", me.szModule)) {
                        hasTier0 = true;
                    }
                } while (Module32NextW(snap.Get(), &me) && (!hasClient || !hasUI || !hasTier0));
            }
        }

        if (hasClient) score += 100;
        if (hasUI)     score += 50;
        if (hasTier0)  score += 20;

        ScopedHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (h) {
            FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
            if (GetProcessTimes(h.Get(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                ULARGE_INTEGER uli{};
                uli.LowPart = ftCreate.dwLowDateTime;
                uli.HighPart = ftCreate.dwHighDateTime;
                createTime = uli.QuadPart;
            }
        }

        scored.push_back({ pid, score, createTime });
    }

    std::sort(scored.begin(), scored.end(), [](const PidScore& a, const PidScore& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.creationTime > b.creationTime; // Newer process first
    });

    for (size_t i = 0; i < pids.size(); ++i) {
        pids[i] = scored[i].pid;
    }
}

RuntimeConfig ResolveConfig(const std::filesystem::path& baseDir) {
    EnableDebugPrivilege();

    std::error_code ecCwd;
    std::filesystem::current_path(baseDir, ecCwd);

    std::filesystem::path iniPath = baseDir / "config.ini";
    std::filesystem::path resolvedExe;
    std::filesystem::path resolvedDll = baseDir / "OpenSteamTool.dll";

    if (std::filesystem::exists(iniPath)) {
        std::vector<wchar_t> bufExe(1024, L'\0');
        std::vector<wchar_t> bufDll(1024, L'\0');
        GetPrivateProfileStringW(L"Settings", L"ExePath", L"", bufExe.data(), static_cast<DWORD>(bufExe.size()), iniPath.c_str());
        GetPrivateProfileStringW(L"Settings", L"DllPath", L"", bufDll.data(), static_cast<DWORD>(bufDll.size()), iniPath.c_str());

        if (bufExe[0] != L'\0') resolvedExe = bufExe.data();
        if (bufDll[0] != L'\0') {
            std::filesystem::path rawDll(bufDll.data());
            resolvedDll = rawDll.is_absolute() ? rawDll : (baseDir / rawDll);
        }
    }

    if (resolvedExe.empty()) {
        auto regPath = QuerySteamRegistryPath();
        resolvedExe = regPath.value_or(L"C:\\Program Files (x86)\\Steam\\steam.exe");

        if (!std::filesystem::exists(iniPath)) {
            // Write UTF-16 LE BOM to force Windows PrivateProfile APIs into native Unicode mode,
            // preventing character corruption on non-ASCII / Chinese paths.
            std::ofstream ofs(iniPath, std::ios::binary);
            if (ofs.is_open()) {
                const unsigned char bom[2] = { 0xFF, 0xFE };
                ofs.write(reinterpret_cast<const char*>(bom), sizeof(bom));
                ofs.close();
            }

            WritePrivateProfileStringW(L"Settings", L"ExePath", resolvedExe.c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"Settings", L"DllPath", L"OpenSteamTool.dll", iniPath.c_str());
        }
    }

    resolvedExe.make_preferred();
    resolvedDll.make_preferred();
    return { resolvedExe, resolvedDll, baseDir };
}

ExecutionResult InjectPayload(DWORD pid, const std::filesystem::path& dllPath, bool isSilent) {
    if (!std::filesystem::exists(dllPath)) {
        return { Status::PayloadNotFound, ERROR_FILE_NOT_FOUND, "Payload DLL not found: " + dllPath.string() };
    }

    if (IsModulePresent(pid, dllPath.filename().wstring())) {
        return { Status::AlreadyInjected, 0, "Module already loaded in target process." };
    }

    constexpr DWORD kAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

    ScopedHandle hProcess(OpenProcess(kAccess, FALSE, pid));
    if (!hProcess) {
        DWORD gle = GetLastError();
        if (!isSilent) {
            std::cerr << "[-] OpenProcess failed (PID=" << pid << ", Error=" << gle << ").\n";
        }
        return { Status::ProcessAccessDenied, gle, "OpenProcess failed. Administrator privilege may be required." };
    }

    BOOL isWow64 = FALSE;
    if (IsWow64Process(hProcess.Get(), &isWow64) && isWow64) {
        return { Status::IncompatibleArchitecture, ERROR_IMAGE_MACHINE_TYPE_MISMATCH_EXE,
            "Target process is 32-bit (WOW64), but OpenSteamTool payload is 64-bit." };
    }

    std::wstring nativeDllPath = dllPath.wstring();
    const SIZE_T byteCount = (nativeDllPath.size() + 1) * sizeof(wchar_t);

    void* remoteMem = VirtualAllocEx(hProcess.Get(), nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        DWORD gle = GetLastError();
        return { Status::RemoteAllocFailed, gle, "VirtualAllocEx failed." };
    }

    ScopeGuard freeRemoteGuard([&] {
        VirtualFreeEx(hProcess.Get(), remoteMem, 0, MEM_RELEASE);
    });

    if (!WriteProcessMemory(hProcess.Get(), remoteMem, nativeDllPath.c_str(), byteCount, nullptr)) {
        DWORD gle = GetLastError();
        return { Status::RemoteAllocFailed, gle, "WriteProcessMemory failed." };
    }

    // In a 64-bit Windows environment, system DLLs share identical virtual base addresses across 64-bit processes
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pfnLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pfnLoadLibraryW) {
        return { Status::RemoteThreadFailed, GetLastError(), "Failed to resolve LoadLibraryW." };
    }

    ScopedHandle hThread(CreateRemoteThread(hProcess.Get(), nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pfnLoadLibraryW), remoteMem, 0, nullptr));
    if (!hThread) {
        DWORD gle = GetLastError();
        return { Status::RemoteThreadFailed, gle, "CreateRemoteThread failed." };
    }

    DWORD waitRes = WaitForSingleObject(hThread.Get(), 15000);
    if (waitRes != WAIT_OBJECT_0) {
        // Critical defense: Do NOT free remote memory on timeout to avoid crashing target process (UAF)
        freeRemoteGuard.Dismiss();
        return { Status::ComponentTimeout, waitRes, "Remote LoadLibraryW execution timed out." };
    }

    // Verify target process did not terminate or crash during remote thread execution
    DWORD procExitCode = 0;
    if (GetExitCodeProcess(hProcess.Get(), &procExitCode) && procExitCode != STILL_ACTIVE) {
        return { Status::TargetExited, procExitCode, "Target process terminated or crashed during injection." };
    }

    DWORD exitCode = 0;
    GetExitCodeThread(hThread.Get(), &exitCode);
    if (exitCode == 0) {
        // Double-check module list in case 64-bit HMODULE lower 32 bits align to zero
        if (!IsModulePresent(pid, dllPath.filename().wstring())) {
            return { Status::LoadLibraryFailed, 0,
                "LoadLibraryW returned NULL in target process. Check if OpenSteamTool.dll is blocked by antivirus or missing dependencies." };
        }
    }

    return { Status::Success, 0, "Injection succeeded." };
}

ExecutionResult AwaitAndInject(DWORD pid, const std::filesystem::path& dllPath,
                              std::chrono::milliseconds timeout,
                              bool isSilent) {
    if (IsModulePresent(pid, dllPath.filename().wstring())) {
        return { Status::AlreadyInjected, 0, "Module already loaded in target process." };
    }

    // Hot-Attach Detection: If steamclient64.dll is already loaded, Steam is running stably
    // and past the UI-loading phase. Inject immediately without waiting.
    if (IsModulePresent(pid, L"steamclient64.dll")) {
        if (!isSilent) {
            std::cout << "[!] 检测到 Steam 已完全启动（steamclient64.dll 已加载），执行热补丁注入...\n";
        }
        return InjectPayload(pid, dllPath, isSilent);
    }

    ScopedHandle hWatch(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!hWatch) {
        return { Status::ProcessAccessDenied, GetLastError(), "Failed to open process for monitoring." };
    }

    const auto startTime = std::chrono::steady_clock::now();
    bool beaconSeen = false;

    while (std::chrono::steady_clock::now() - startTime < timeout) {
        if (g_stopRequested.load()) {
            return { Status::TargetExited, 0, "Operation cancelled by user or system shutdown." };
        }

        // 1. Immediate detection before entering sleep:
        //    Soft beacon detection (tier0_s64.dll indicates core runtime is initializing)
        if (!beaconSeen) {
            if (IsModulePresent(pid, L"tier0_s64.dll")) {
                beaconSeen = true;
            }
        }

        // 2. UI readiness check: If steamui.dll is present, inject immediately without pre-sleep penalty
        if (IsModulePresent(pid, L"steamui.dll")) {
            // Smooth 20ms kernel wait to let PE mapping and loader linking settle
            WaitForSingleObject(hWatch.Get(), 20);
            return InjectPayload(pid, dllPath, isSilent);
        }

        // 3. Staged dynamic interval:
        //    Phase 1 (before beacon seen): 200ms gentle polling to avoid burning CPU during early startup
        //    Phase 2 (after beacon seen):  50ms tight burst to precisely catch the UI loading moment
        DWORD currentInterval = beaconSeen ? 50 : 200;

        // 4. Kernel wait: Sleep without active spinning. If target process exits,
        //    WaitForSingleObject immediately wakes up with WAIT_OBJECT_0 at zero CPU cost.
        DWORD waitRes = WaitForSingleObject(hWatch.Get(), currentInterval);
        if (waitRes == WAIT_OBJECT_0) {
            return { Status::TargetExited, 0, "Target process terminated before steamui.dll loaded." };
        }
    }

    return { Status::ComponentTimeout, 0, "Timed out waiting for steamui.dll to load." };
}

void LogMessage(const std::filesystem::path& baseDir, const std::string& msg, bool isSilent) {
    if (!isSilent) {
        std::cout << msg << std::endl;
    }
    try {
        std::filesystem::path logFile = baseDir / "inject.log";
        std::ofstream ofs(logFile, std::ios::app | std::ios::binary);
        if (ofs.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto timeT = std::chrono::system_clock::to_time_t(now);
            std::tm tmNow{};
            localtime_s(&tmNow, &timeT);

            std::ostringstream ss;
            ss << "[" << std::put_time(&tmNow, "%Y-%m-%d %H:%M:%S") << "] " << msg << "\r\n";
            std::string line = ss.str();
            ofs.write(line.c_str(), line.size());
        }
    } catch (...) {}
}

void ShowErrorAlert(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"OpenSteamTool Injector Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int RunWatcher(const RuntimeConfig& config) {
    SingleInstanceGuard guard;
    if (guard.IsConflict()) {
        return 0; // Instance already running
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    LogMessage(config.baseDir, "[Watcher] 后台自动注入监听已启动，等待 steam.exe 启动...", true);
    std::set<DWORD> injectedPids;

    struct FailureRecord {
        int count = 0;
        bool permanent = false;
    };
    std::unordered_map<DWORD, FailureRecord> failedPids;

    while (!g_stopRequested.load()) {
        auto pids = SnapshotProcessIds(L"steam.exe");
        if (!pids.empty()) {
            PrioritizeCandidatePids(pids);

            for (DWORD pid : pids) {
                if (injectedPids.contains(pid)) continue;
                if (auto it = failedPids.find(pid); it != failedPids.end()) {
                    if (it->second.permanent || it->second.count >= 2) {
                        continue;
                    }
                }

                auto res = AwaitAndInject(pid, config.payloadDll, std::chrono::seconds(15), true);
                if (res.status == Status::TargetExited) {
                    // Handle Steam cold-start updater restart (bootstrap exit & successor spawn)
                    auto successorPid = PollSuccessorProcess(pid, std::chrono::seconds(8));
                    if (successorPid && !injectedPids.contains(*successorPid)) {
                        LogMessage(config.baseDir, "[Watcher] 捕获到 Steam 更新继任进程 (PID: " + std::to_string(*successorPid) + ")，继续注入...", true);
                        res = AwaitAndInject(*successorPid, config.payloadDll, std::chrono::seconds(25), true);
                        if (res.IsOk()) {
                            injectedPids.insert(*successorPid);
                            failedPids.erase(*successorPid);
                            LogMessage(config.baseDir, "[Watcher] 成功自动注入 OpenSteamTool 到 Steam (PID: " + std::to_string(*successorPid) + ")", true);
                        } else {
                            LogMessage(config.baseDir, "[Watcher] 注入继任进程失败 (PID: " + std::to_string(*successorPid) + "): " + res.message, true);
                            failedPids[*successorPid] = { 2, true };
                        }
                    }
                } else if (res.IsOk()) {
                    injectedPids.insert(pid);
                    failedPids.erase(pid);
                    LogMessage(config.baseDir, "[Watcher] 成功自动注入 OpenSteamTool 到 Steam (PID: " + std::to_string(pid) + ")", true);
                } else {
                    if (res.status == Status::ProcessAccessDenied) {
                        failedPids[pid] = { 1, true };
                        LogMessage(config.baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + "): 权限不足 (Access Denied)。如果 Steam 以管理员权限运行，请以管理员身份启动 Injector。", true);
                    } else if (res.status == Status::IncompatibleArchitecture || res.status == Status::PayloadNotFound) {
                        failedPids[pid] = { 1, true };
                        LogMessage(config.baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + "): " + res.message, true);
                    } else {
                        auto& rec = failedPids[pid];
                        rec.count++;
                        if (rec.count >= 2) {
                            rec.permanent = true;
                            LogMessage(config.baseDir, "[Watcher] PID: " + std::to_string(pid) + " 注入超时，可能为后台辅助进程，已挂起重试。", true);
                        } else {
                            LogMessage(config.baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + "): " + res.message, true);
                        }
                    }
                }
            }
        }

        // Direct kernel liveness checks to purge exited processes without stale snapshot race conditions
        std::erase_if(injectedPids, [](DWORD cachedPid) {
            ScopedHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, cachedPid));
            if (!h) return true;
            DWORD exitCode = 0;
            return !GetExitCodeProcess(h.Get(), &exitCode) || exitCode != STILL_ACTIVE;
        });
        std::erase_if(failedPids, [](const auto& item) {
            ScopedHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.first));
            if (!h) return true;
            DWORD exitCode = 0;
            return !GetExitCodeProcess(h.Get(), &exitCode) || exitCode != STILL_ACTIVE;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    LogMessage(config.baseDir, "[Watcher] 后台监听服务正常退出。", true);
    return 0;
}

int RunSilentOnce(const RuntimeConfig& config) {
    auto pids = SnapshotProcessIds(L"steam.exe");
    if (pids.empty()) return 0;
    PrioritizeCandidatePids(pids);

    DWORD activePid = pids.front();
    auto res = AwaitAndInject(activePid, config.payloadDll, std::chrono::seconds(30), true);
    if (res.status == Status::TargetExited) {
        auto successorPid = PollSuccessorProcess(activePid, std::chrono::seconds(10));
        if (successorPid) {
            activePid = *successorPid;
            res = AwaitAndInject(activePid, config.payloadDll, std::chrono::seconds(25), true);
        }
    }

    if (res.IsOk()) {
        LogMessage(config.baseDir, "[Silent] 成功静默注入 OpenSteamTool 到 Steam (PID: " + std::to_string(activePid) + ")", true);
        return 0;
    } else {
        LogMessage(config.baseDir, "[Silent] 注入失败 (PID: " + std::to_string(activePid) + "): " + res.message, true);
        return 1;
    }
}

int RunInteractive(const RuntimeConfig& config) {
    SetConsoleTitleW(L"OpenSteamTool Injector (ost-Injector)");

    std::cout << "=================================================\n";
    std::cout << "       OpenSteamTool Injector (ost-Injector)     \n";
    std::cout << "       Supported modes: manual, -silent, -watch   \n";
    std::cout << "=================================================\n\n";

    std::cout << "[+] Target Executable : " << config.targetExe.string() << "\n";
    std::cout << "[+] Payload DLL       : " << config.payloadDll.string() << "\n\n";

    if (!std::filesystem::exists(config.payloadDll)) {
        std::wstring err = L"Error: Payload DLL was not found at:\n" + config.payloadDll.wstring() + L"\n\nPlease ensure OpenSteamTool.dll exists.";
        std::cerr << "[-] " << config.payloadDll.string() << " not found!\n";
        ShowErrorAlert(err);
        return 1;
    }

    auto existingPids = SnapshotProcessIds(L"steam.exe");
    if (!existingPids.empty()) {
        PrioritizeCandidatePids(existingPids);
        DWORD pid = existingPids.front();
        std::cout << "[+] Found running Steam process (PID: " << pid << ")\n";

        if (IsModulePresent(pid, config.payloadDll.filename().wstring())) {
            std::cout << "[!] 当前 Steam 进程已加载过 OpenSteamTool.dll！\n";
            std::cout << "[!] 无需重复注入。\n";
            std::cout << "This console will close in 3 seconds...\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return 0;
        }

        std::cout << "[+] Waiting for Steam UI initialization...\n";
        auto res = AwaitAndInject(pid, config.payloadDll, std::chrono::seconds(30), false);
        if (res.IsOk()) {
            std::cout << "[+] Injection completed successfully.\n";
            std::cout << "This console will close in 3 seconds...\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return 0;
        } else {
            std::cerr << "[-] Injection failed: " << res.message << "\n";
            ShowErrorAlert(L"DLL injection into running Steam process failed: " + Utf8ToWide(res.message));
            return 1;
        }
    }

    // Steam is not running: launch it
    if (!std::filesystem::exists(config.targetExe)) {
        std::wstring err = L"Error: Target Steam executable does not exist at:\n" + config.targetExe.wstring();
        std::cerr << "[-] " << config.targetExe.string() << " not found!\n";
        ShowErrorAlert(err);
        return 1;
    }

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring quotedCmd = L"\"" + config.targetExe.wstring() + L"\"";
    std::vector<wchar_t> cmdBuffer(quotedCmd.begin(), quotedCmd.end());
    cmdBuffer.push_back(L'\0');

    std::cout << "[+] Launching Steam executable...\n";
    std::wstring workingDir = config.targetExe.parent_path().wstring();

    if (!CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi)) {
        DWORD gle = GetLastError();
        std::wstring err = L"CreateProcessW failed. Error: " + std::to_wstring(gle);
        std::cerr << "[-] CreateProcessW failed. Error: " << gle << "\n";
        ShowErrorAlert(err);
        return 1;
    }

    ScopedHandle hProcess(pi.hProcess);
    ScopedHandle hThread(pi.hThread);
    DWORD currentPid = pi.dwProcessId;

    std::cout << "[+] Waiting for Steam UI to initialize (PID: " << currentPid << ")...\n";
    auto res = AwaitAndInject(currentPid, config.payloadDll, std::chrono::seconds(15), false);

    // Handle Steam cold-start updater restart (bootstrap exit & successor spawn)
    if (res.status == Status::TargetExited) {
        std::cout << "[!] 检测到 Steam 引导进程退出，正在捕获自更新后的继任主进程...\n";
        auto successorPid = PollSuccessorProcess(currentPid, std::chrono::seconds(15));
        if (successorPid) {
            std::cout << "[+] 成功捕获继任 Steam 进程 (PID: " << *successorPid << ")，继续等待注入...\n";
            res = AwaitAndInject(*successorPid, config.payloadDll, std::chrono::seconds(30), false);
        }
    }

    if (res.IsOk()) {
        std::cout << "[+] Injection completed successfully.\n";
        std::cout << "This console will close in 3 seconds...\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    } else {
        std::cerr << "[-] Injection failed: " << res.message << "\n";
        ShowErrorAlert(L"DLL injection failed: " + Utf8ToWide(res.message));
        return 1;
    }
}

} // namespace Injector

// Windows standard entry point
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isWatchMode = false;
    bool isSilentMode = false;
    bool isHelpRequested = false;

    if (argvW) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argvW[i];
            for (auto& c : arg) c = towlower(c);
            if (arg == L"-watch" || arg == L"--watch" || arg == L"-daemon" || arg == L"/watch") {
                isWatchMode = true;
            } else if (arg == L"-silent" || arg == L"--silent" || arg == L"-s" || arg == L"/s" || arg == L"/silent") {
                isSilentMode = true;
            } else if (arg == L"-help" || arg == L"--help" || arg == L"-h" || arg == L"/?" || arg == L"/h" || arg == L"/help") {
                isHelpRequested = true;
            }
        }
        LocalFree(argvW);
    }

    if (isHelpRequested) {
        Injector::EnsureInteractiveConsole();
        Injector::PrintUsage();
        return 0;
    }

    if (!isWatchMode && !isSilentMode) {
        Injector::EnsureInteractiveConsole();
    }

    std::vector<wchar_t> pathBuffer(1024, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    while (len == pathBuffer.size()) {
        pathBuffer.resize(pathBuffer.size() * 2, L'\0');
        len = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    }
    std::filesystem::path baseDir = (len > 0)
        ? std::filesystem::path(pathBuffer.data()).parent_path()
        : std::filesystem::current_path();

    auto config = Injector::ResolveConfig(baseDir);

    if (isWatchMode) {
        return Injector::RunWatcher(config);
    }
    if (isSilentMode) {
        return Injector::RunSilentOnce(config);
    }

    return Injector::RunInteractive(config);
}
