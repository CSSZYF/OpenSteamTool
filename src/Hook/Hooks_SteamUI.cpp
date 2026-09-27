#include "Hooks_SteamUI.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Hook/Hooks_Package.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    using namespace std::chrono_literals;
    constexpr int  kMaxRetry      = 500;
    constexpr auto kRetryInterval = 10ms;

    static std::string_view ExtractFileName(std::string_view path) {
        const size_t pos = path.find_last_of("\\/");
        return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
    }

    static std::wstring_view ExtractFileNameW(std::wstring_view path) {
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);
    }

    static bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
        return (a.size() == b.size()) && (_strnicmp(a.data(), b.data(), a.size()) == 0);
    }

    static bool EqualsIgnoreCaseW(std::wstring_view a, std::wstring_view b) noexcept {
        return (a.size() == b.size()) && (_wcsnicmp(a.data(), b.data(), a.size()) == 0);
    }

    static bool IsSteamClientPath(const char* path) {
        if (!path) return false;
        const std::string_view fn = ExtractFileName(path);
        return EqualsIgnoreCase(fn, "steamclient64.dll") ||
               EqualsIgnoreCase(fn, "steamclient.dll")   ||
               EqualsIgnoreCase(fn, "steamclient64")     ||
               EqualsIgnoreCase(fn, "steamclient");
    }

    static bool IsSteamClientPathW(const wchar_t* path) {
        if (!path) return false;
        const std::wstring_view fn = ExtractFileNameW(path);
        return EqualsIgnoreCaseW(fn, L"steamclient64.dll") ||
               EqualsIgnoreCaseW(fn, L"steamclient.dll")   ||
               EqualsIgnoreCaseW(fn, L"steamclient64")     ||
               EqualsIgnoreCaseW(fn, L"steamclient");
    }

    // Original pointers for system module lookup APIs
    static decltype(&GetModuleHandleA)   oGetModuleHandleA   = &GetModuleHandleA;
    static decltype(&GetModuleHandleW)   oGetModuleHandleW   = &GetModuleHandleW;
    static decltype(&GetModuleHandleExA) oGetModuleHandleExA = &GetModuleHandleExA;
    static decltype(&GetModuleHandleExW) oGetModuleHandleExW = &GetModuleHandleExW;

    HMODULE WINAPI hkGetModuleHandleA(LPCSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPath(lpModuleName)) {
            return reinterpret_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleA(lpModuleName);
    }

    HMODULE WINAPI hkGetModuleHandleW(LPCWSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPathW(lpModuleName)) {
            return reinterpret_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleW(lpModuleName);
    }

    BOOL WINAPI hkGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPath(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            constexpr DWORD kKnownFlags = GET_MODULE_HANDLE_EX_FLAG_PIN |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                                          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
            if ((dwFlags & ~kKnownFlags) != 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                if (!oGetModuleHandleExA((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCSTR>(client_hModule), phModule))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
                return TRUE;
            }
            *phModule = reinterpret_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExA(dwFlags, lpModuleName, phModule);
    }

    BOOL WINAPI hkGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPathW(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            constexpr DWORD kKnownFlags = GET_MODULE_HANDLE_EX_FLAG_PIN |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                                          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
            if ((dwFlags & ~kKnownFlags) != 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                if (!oGetModuleHandleExW((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCWSTR>(client_hModule), phModule))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
                return TRUE;
            }
            *phModule = reinterpret_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExW(dwFlags, lpModuleName, phModule);
    }


    HOOK_FUNC(LoadModuleWithPath, void*, const char* path, bool flags)
    {
        LOG_STEAMUI_INFO("LoadModuleWithPath called with path: {}, flags: {}",
                         path ? path : "(null)", flags);

        const bool isSteamClient = IsSteamClientPath(path);

        if (isSteamClient) {
            bool logged = false;
            for (int i = 0; i < kMaxRetry && !g_HooksInstalled.load(); ++i) {
                if (!logged) {
                    LOG_STEAMUI_DEBUG("LoadModuleWithPath: waiting for hooks to be installed...");
                    logged = true;
                }
                std::this_thread::sleep_for(kRetryInterval);
            }
        }

        void* h = oLoadModuleWithPath(path, flags);

        if (isSteamClient) {
            if (!g_HooksInstalled.load()) {
                LOG_STEAMUI_WARN("LoadModuleWithPath: hooks initialization timed out after {} ms, aborting diversion",
                                 kMaxRetry * static_cast<int>(kRetryInterval.count()));
                return h;
            }

            if (client_hModule) {
                if (g_IsDiversionActive.load()) {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: diverted {} (original {}) -> diversion {}",
                                     path ? path : "steamclient64.dll", h, static_cast<void*>(client_hModule));
                } else {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: returned fallback steamclient64.dll ({})",
                                     static_cast<void*>(client_hModule));
                }
                return client_hModule;
            }
        }

        return h;
    }

    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    // Apps to drop from or restore to the library UI
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::vector<AppId_t> g_pendingAdditions;
    std::unordered_set<AppId_t> g_removedAppIds;

    constexpr uint32_t k_EAppStateUpdatingMask =
        k_EAppStateDownloading         | // 0x00800000
        k_EAppStateStaging             | // 0x01000000
        k_EAppStateCommitting          | // 0x02000000
        k_EAppStateVerifyingStaged     | // 0x04000000
        k_EAppStateVerifyingInstalled  | // 0x00200000
        k_EAppStatePreallocating       | // 0x00400000
        k_EAppStateReconfiguring       | // 0x00100000
        k_EAppStateStopping            | // 0x08000000
        k_EAppStateUpdateRunning       | // 0x00000100
        k_EAppStateUpdatePaused        | // 0x00000200
        k_EAppStateUpdateStarted       | // 0x00000400
        k_EAppStateUpdateQueued         | // 0x00000008
        k_EAppStateUpdateRequired;       // 0x00000002

    constexpr uint32_t k_EAppStateActiveTransferMask =
        k_EAppStateDownloading         | // 0x00800000
        k_EAppStateStaging             | // 0x01000000
        k_EAppStateCommitting          | // 0x02000000
        k_EAppStateVerifyingStaged     | // 0x04000000
        k_EAppStateVerifyingInstalled  | // 0x00200000
        k_EAppStatePreallocating       | // 0x00400000
        k_EAppStateUpdateRunning;        // 0x00000100

    struct AppStateEntry {
        uint32_t stateFlags = 0;
        uint32_t changeNumber = 0;
    };

    struct AppStateTracker {
        std::unordered_map<AppId_t, AppStateEntry> trackedStates;
        std::vector<AppId_t> activeUpdatingApps;
        std::unordered_set<AppId_t> inFlightSyncs;
        std::mutex mutex;
    };
    static AppStateTracker g_appStateTracker;

    class AutoSyncWorkerPool {
    private:
        std::queue<AppId_t> m_taskQueue;
        std::mutex m_queueMutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_stopping{false};
        std::thread m_workerThread;

        void WorkerLoop() {
            while (true) {
                AppId_t appId = 0;
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_cv.wait(lock, [this]() {
                        return m_stopping.load(std::memory_order_relaxed) || !m_taskQueue.empty();
                    });

                    if (m_stopping.load(std::memory_order_relaxed) && m_taskQueue.empty()) {
                        break;
                    }

                    appId = m_taskQueue.front();
                    m_taskQueue.pop();
                }

                // 500ms interruptible debounce to allow Steam to flush ACF and release handles
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                        return m_stopping.load(std::memory_order_relaxed);
                    });
                    if (m_stopping.load(std::memory_order_relaxed)) {
                        std::lock_guard<std::mutex> trackerLock(g_appStateTracker.mutex);
                        g_appStateTracker.inFlightSyncs.erase(appId);
                        break;
                    }
                }

                // InFlight cleanup guard
                struct InFlightGuard {
                    AppId_t id;
                    ~InFlightGuard() {
                        std::lock_guard<std::mutex> lock(g_appStateTracker.mutex);
                        g_appStateTracker.inFlightSyncs.erase(id);
                    }
                } guard{appId};

                // 1. Account ownership check
                if (!Hooks_Package::IsAppTrulyOwned(appId)) {
                    LOG_STEAMUI_DEBUG("AutoSync: appId={} is not genuinely owned by current account - skipping background sync", appId);
                    continue;
                }

                // 2. Lua configuration check
                if (!LuaConfig::HasDepot(appId, false)) {
                    LOG_STEAMUI_DEBUG("AutoSync: appId={} is not configured with addappid - skipping background sync", appId);
                    continue;
                }

                LOG_STEAMUI_INFO("AutoSync: detected update completion for owned appId={}, triggering background sync", appId);
                try {
                    PipeManager::DenuvoAuth::SyncOrGenerate(appId, "", false);
                } catch (const std::exception& ex) {
                    LOG_STEAMUI_ERROR("AutoSync: exception during SyncOrGenerate for appId={}: {}", appId, ex.what());
                } catch (...) {
                    LOG_STEAMUI_ERROR("AutoSync: unknown exception during SyncOrGenerate for appId={}", appId);
                }
            }
        }

    public:
        void Start() {
            if (m_workerThread.joinable()) return;
            m_stopping.store(false, std::memory_order_relaxed);
            m_workerThread = std::thread(&AutoSyncWorkerPool::WorkerLoop, this);
        }

        void Stop() {
            m_stopping.store(true, std::memory_order_relaxed);
            m_cv.notify_all();
            if (m_workerThread.joinable()) {
                m_workerThread.join();
            }
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                std::queue<AppId_t> empty;
                m_taskQueue.swap(empty);
            }
            {
                std::lock_guard<std::mutex> trackerLock(g_appStateTracker.mutex);
                g_appStateTracker.inFlightSyncs.clear();
            }
        }

        void Enqueue(AppId_t appId) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_taskQueue.push(appId);
            m_cv.notify_one();
        }
    };
    static AutoSyncWorkerPool g_autoSyncWorkerPool;

    static std::atomic<std::shared_ptr<const std::vector<AppId_t>>> s_installedSnapshot{
        std::make_shared<const std::vector<AppId_t>>()
    };

    static void ScanInstalledAppIds() {
        try {
            std::string steamPath = SteamInstallPath;
            if (steamPath.empty()) return;

            std::filesystem::path libVdf =
                std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(steamPath)) / "steamapps" / "libraryfolders.vdf";
            std::ifstream file(libVdf);
            if (!file.is_open()) return;

            std::unordered_set<AppId_t> foundInstalled;
            std::string line;
            bool inAppsBlock = false;
            int braceDepth = 0;
            int appsDepth = -1;

            while (std::getline(file, line)) {
                std::string_view sv = line;
                while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) sv.remove_prefix(1);
                if (sv.empty() || sv.starts_with("//") || sv.starts_with("#")) continue;

                size_t commentPos = sv.find("//");
                if (commentPos != std::string_view::npos) {
                    sv = sv.substr(0, commentPos);
                    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
                    if (sv.empty()) continue;
                }

                int openBraces = static_cast<int>(std::count(sv.begin(), sv.end(), '{'));
                braceDepth += openBraces;
                int closeBraces = static_cast<int>(std::count(sv.begin(), sv.end(), '}'));
                if (closeBraces > 0) {
                    braceDepth = std::max(0, braceDepth - closeBraces);
                    if (inAppsBlock && appsDepth >= 0 && braceDepth <= appsDepth) {
                        inAppsBlock = false;
                        appsDepth = -1;
                    }
                }

                if (!inAppsBlock) {
                    std::string lower(sv);
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (lower.find("\"apps\"") != std::string_view::npos) {
                        inAppsBlock = true;
                        appsDepth = (openBraces > 0) ? braceDepth - (openBraces - closeBraces) : braceDepth;
                        if (closeBraces > 0 && braceDepth <= appsDepth) {
                            inAppsBlock = false;
                            appsDepth = -1;
                        }
                    }
                } else {
                    size_t firstQuote = sv.find('"');
                    if (firstQuote != std::string_view::npos) {
                        size_t secondQuote = sv.find('"', firstQuote + 1);
                        if (secondQuote != std::string_view::npos) {
                            std::string_view appIdStr = sv.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                            AppId_t appId = 0;
                            auto [p, ec] = std::from_chars(appIdStr.data(), appIdStr.data() + appIdStr.size(), appId);
                            if (ec == std::errc{} && p == appIdStr.data() + appIdStr.size() && appId != 0 && appId != k_uAppIdInvalid) {
                                foundInstalled.insert(appId);
                            }
                        }
                    }
                }
            }

            if (foundInstalled.empty()) return;

            auto configuredSnapshot = LuaConfig::GetConfiguredAppIdsSnapshot();
            if (!configuredSnapshot || configuredSnapshot->empty()) {
                return;
            }

            std::vector<AppId_t> filteredInstalled;
            filteredInstalled.reserve(std::min(foundInstalled.size(), size_t(1500)));

            for (AppId_t appId : *configuredSnapshot) {
                if (foundInstalled.contains(appId)) {
                    filteredInstalled.push_back(appId);
                }
            }

            s_installedSnapshot.store(
                std::make_shared<const std::vector<AppId_t>>(std::move(filteredInstalled)));
        } catch (const std::exception& ex) {
            LOG_STEAMUI_DEBUG("ScanInstalledAppIds: exception: {}", ex.what());
        } catch (...) {
        }
    }

    class InstalledScannerPool {
    private:
        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_stopping{false};
        std::atomic<bool> m_rescanRequested{false};

        void Loop() {
            while (!m_stopping.load(std::memory_order_relaxed)) {
                ScanInstalledAppIds();

                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::seconds(30), [this]() {
                    return m_stopping.load(std::memory_order_relaxed) ||
                           m_rescanRequested.exchange(false, std::memory_order_relaxed);
                });
            }
        }

    public:
        void Start() {
            if (m_thread.joinable()) return;
            m_stopping.store(false, std::memory_order_relaxed);
            m_thread = std::thread(&InstalledScannerPool::Loop, this);
        }

        void Stop() {
            m_stopping.store(true, std::memory_order_relaxed);
            m_cv.notify_all();
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

        void TriggerRescan() {
            m_rescanRequested.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_one();
        }
    };
    static InstalledScannerPool g_installedScannerPool;

    constexpr size_t k_SliceBatchSize = 64;
    static size_t s_timeSliceCursor = 0;

    static void Process1000GamesAutoSync(void* pController) noexcept {
        try {
            if (!Config::GetManifestAutoSyncOnUpdate() || !CAPTURE_READY(GetAppByID)) return;
            void* pCtrl = pController ? pController : g_pController;
            if (!pCtrl) return;

            std::lock_guard lock(g_appStateTracker.mutex);

            auto safeEnqueue = [](AppId_t id) {
                if (g_appStateTracker.inFlightSyncs.insert(id).second) {
                    try {
                        g_autoSyncWorkerPool.Enqueue(id);
                    } catch (...) {
                        g_appStateTracker.inFlightSyncs.erase(id);
                    }
                }
            };

            // ── 通道一：活跃传输集检查（通常仅 0~1 款下载中游戏，耗时 20 纳秒）──
            for (auto it = g_appStateTracker.activeUpdatingApps.begin(); it != g_appStateTracker.activeUpdatingApps.end(); ) {
                AppId_t appId = *it;
                CSteamApp* pApp = oGetAppByID(pCtrl, appId, false);
                if (!pApp) {
                    it = g_appStateTracker.activeUpdatingApps.erase(it);
                    continue;
                }

                const uint32_t currentState = static_cast<uint32_t>(pApp->AppStateFlags);
                const bool isNowFullyInstalled = ((currentState & k_EAppStateUpdatingMask) == 0) &&
                                                 ((currentState & k_EAppStateFullyInstalled) != 0);

                if (isNowFullyInstalled) {
                    g_appStateTracker.trackedStates[appId].stateFlags = currentState;
                    g_appStateTracker.trackedStates[appId].changeNumber = pApp->ChangeNumber;
                    safeEnqueue(appId);
                    it = g_appStateTracker.activeUpdatingApps.erase(it);
                } else if ((currentState & k_EAppStateActiveTransferMask) == 0) {
                    g_appStateTracker.trackedStates[appId].stateFlags = currentState;
                    it = g_appStateTracker.activeUpdatingApps.erase(it);
                } else {
                    g_appStateTracker.trackedStates[appId].stateFlags = currentState;
                    ++it;
                }
            }

            // ── 通道二：时间片平摊步进（单帧耗时约 0.0015 ms）──
            auto installedSnapshot = s_installedSnapshot.load();
            if (!installedSnapshot || installedSnapshot->empty()) {
                return;
            }

            const auto& installedList = *installedSnapshot;
            if (s_timeSliceCursor >= installedList.size()) {
                s_timeSliceCursor = 0;
            }

            const size_t batchEnd = std::min(s_timeSliceCursor + k_SliceBatchSize, installedList.size());
            for (size_t i = s_timeSliceCursor; i < batchEnd; ++i) {
                AppId_t appId = installedList[i];
                if (appId == 0 || appId == k_uAppIdInvalid) continue;

                CSteamApp* pApp = oGetAppByID(pCtrl, appId, false);
                if (!pApp) continue;
                // Skip DLC/child apps; state tracking and manifest syncing are anchored on the base game
                if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId) continue;

                const uint32_t currentState = static_cast<uint32_t>(pApp->AppStateFlags);
                const uint32_t currentChangeNum = pApp->ChangeNumber;

                auto entryIt = g_appStateTracker.trackedStates.find(appId);
                if (entryIt == g_appStateTracker.trackedStates.end()) {
                    g_appStateTracker.trackedStates[appId] = { currentState, currentChangeNum };
                    if ((currentState & k_EAppStateActiveTransferMask) != 0) {
                        if (std::ranges::find(g_appStateTracker.activeUpdatingApps, appId) == g_appStateTracker.activeUpdatingApps.end()) {
                            g_appStateTracker.activeUpdatingApps.push_back(appId);
                        }
                    }
                    continue;
                }

                const uint32_t lastState = entryIt->second.stateFlags;
                const uint32_t lastChangeNum = entryIt->second.changeNumber;

                const bool wasUpdating = (lastState & k_EAppStateUpdatingMask) != 0;
                const bool isNowFullyInstalled = ((currentState & k_EAppStateUpdatingMask) == 0) &&
                                                 ((currentState & k_EAppStateFullyInstalled) != 0);

                // 判定 1：常规状态机跳变完成
                if (wasUpdating && isNowFullyInstalled) {
                    entryIt->second = { currentState, currentChangeNum };
                    safeEnqueue(appId);
                }
                // 判定 2：秒级小补丁/错失 Updating 的版本号跃迁自愈兜底
                else if (isNowFullyInstalled && currentChangeNum != lastChangeNum) {
                    if (lastChangeNum == 0) {
                        entryIt->second.changeNumber = currentChangeNum;
                    } else if ((currentState & k_EAppStateAppRunning) == 0) {
                        entryIt->second = { currentState, currentChangeNum };
                        safeEnqueue(appId);
                    }
                }
                // 判定 3：捕获到游戏开始活跃传输，晋升至通道一以享受每帧直达监控
                else if ((currentState & k_EAppStateActiveTransferMask) != 0) {
                    if (std::ranges::find(g_appStateTracker.activeUpdatingApps, appId) == g_appStateTracker.activeUpdatingApps.end()) {
                        g_appStateTracker.activeUpdatingApps.push_back(appId);
                    }
                    entryIt->second.stateFlags = currentState;
                } else if (currentState != lastState) {
                    entryIt->second.stateFlags = currentState;
                }
            }

            s_timeSliceCursor = (batchEnd >= installedList.size()) ? 0 : batchEnd;
        } catch (...) {
        }
    }

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp)
        {
            if (LuaConfig::HasDepot(pApp->nAppID, false))
            {
                if (pApp->OwnershipFlags == k_EAppOwnershipFlags_None)
                {
                    pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                        k_EAppOwnershipFlags_OwnsLicense | k_EAppOwnershipFlags_LicensePermanent);
                }
                uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
                if (t)
                {
                    pApp->PurchasedTime = t;
                    LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                      pApp->PurchasedTime, pApp->nAppID);
                }
            }
            else
            {
                if (!LuaConfig::IsOwned(pApp->nAppID))
                {
                    std::lock_guard<std::mutex> lock(g_removalMutex);
                    if (g_removedAppIds.contains(pApp->nAppID))
                    {
                        pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                        pApp->PurchasedTime = 0;
                        pApp->MasterSubAppID = 0;
                    }
                }
            }
        }

        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds.empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds.size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (pController && !g_pController)
        {
            g_pController = pController;
        }

        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> drainingRemovals;
            std::vector<AppId_t> drainingAdditions;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                if (!g_pendingRemovals.empty()) {
                    drainingRemovals.swap(g_pendingRemovals);
                }
                if (!g_pendingAdditions.empty()) {
                    drainingAdditions.swap(g_pendingAdditions);
                }
            }

            if (!drainingAdditions.empty())
            {
                std::vector<AppId_t> parentsToNotify;

                for (AppId_t appId : drainingAdditions)
                {
                    if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                    {
                        pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                            k_EAppOwnershipFlags_OwnsLicense | k_EAppOwnershipFlags_LicensePermanent);

                        uint32_t t = LuaConfig::GetPurchaseTime(appId);
                        if (t)
                        {
                            pApp->PurchasedTime = t;
                        }

                        const bool isDlc = ((pApp->eProtoAppType & 32) != 0) ||
                                           (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid);

                        if (isDlc)
                        {
                            if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId)
                            {
                                if (std::ranges::find(parentsToNotify, pApp->ParentAppID) == parentsToNotify.end())
                                {
                                    parentsToNotify.push_back(pApp->ParentAppID);
                                }
                            }
                        }
                    }

                    LOG_STEAMUI_INFO("RunFrame: restoring added appId {}", appId);
                    oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
                }

                for (AppId_t parentId : parentsToNotify)
                {
                    LOG_STEAMUI_INFO("RunFrame: notifying parent appId {} of DLC addition", parentId);
                    oMarkAppChange(g_pAppChangeSource, parentId, EAppChangeFlags::AppInfoOrConfig);
                }
            }

            if (!drainingRemovals.empty())
            {
                std::vector<AppId_t> parentsToNotify;

                for (AppId_t appId : drainingRemovals)
                {
                    if (LuaConfig::IsOwned(appId) || LuaConfig::HasDepot(appId, false))
                    {
                        LOG_STEAMUI_DEBUG("RunFrame: appId {} is still owned or active in config, skipping removal", appId);
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.erase(appId);
                        continue;
                    }

                    if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                    {
                        pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                        pApp->PurchasedTime = 0;
                        pApp->MasterSubAppID = 0;

                        const bool isDlc = ((pApp->eProtoAppType & 32) != 0) ||
                                           (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid);

                        if (isDlc)
                        {
                            if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId)
                            {
                                if (std::ranges::find(parentsToNotify, pApp->ParentAppID) == parentsToNotify.end())
                                {
                                    parentsToNotify.push_back(pApp->ParentAppID);
                                }
                            }
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.insert(appId);
                    }

                    LOG_STEAMUI_INFO("RunFrame: removing appId {}", appId);
                    oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
                }

                for (AppId_t parentId : parentsToNotify)
                {
                    LOG_STEAMUI_INFO("RunFrame: notifying parent appId {} of DLC change", parentId);
                    oMarkAppChange(g_pAppChangeSource, parentId, EAppChangeFlags::AppInfoOrConfig);
                }
            }

            if (!drainingAdditions.empty() || !drainingRemovals.empty())
            {
                g_installedScannerPool.TriggerRescan();
            }
        }

        Process1000GamesAutoSync(pController);

        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(LoadModuleWithPath);
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);

        // System module handle redirection for Diversion shadow memory isolation
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW))) _ost_detour_transaction_ok_ = false;

        HOOK_END();

        g_appStateTracker.trackedStates.reserve(1500);
        g_appStateTracker.activeUpdatingApps.reserve(16);
        g_autoSyncWorkerPool.Start();
        g_installedScannerPool.Start();
    }

    void Uninstall()
    {
        g_installedScannerPool.Stop();
        g_autoSyncWorkerPool.Stop();

        UNHOOK_BEGIN();
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW))) _ost_detour_transaction_ok_ = false;

        UNINSTALL_HOOK(LoadModuleWithPath);
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }


    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingAdditions, appId);
        g_removedAppIds.insert(appId);
        if (std::ranges::find(g_pendingRemovals, appId) == g_pendingRemovals.end()) {
            g_pendingRemovals.push_back(appId);
        }
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }

    void QueueAddition(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
        if (std::ranges::find(g_pendingAdditions, appId) == g_pendingAdditions.end()) {
            g_pendingAdditions.push_back(appId);
        }
    }

    bool IsRemoved(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        return g_removedAppIds.contains(appId);
    }

    void TriggerInstalledScanner()
    {
        g_installedScannerPool.TriggerRescan();
    }
}
