#include "Config.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestClient.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <mutex>

namespace Config {
namespace {

    struct Snapshot {
        std::string manifestProvider = "manifestdex";
        ManifestTimeouts manifestTimeouts;
        LogLevel logLevel = LogLevel::Debug;
        std::string logDir;
        std::vector<std::string> luaPaths;
        std::string remoteUrlTemplate;
        bool statsEnableApi = true;
        std::vector<InjectDll> injectDlls;
        CloudSettings cloud;
        bool manifestLockOwnedGames = false;
        bool manifestAutoSyncOnUpdate = true;
    };

    std::mutex g_mutex;
    bool g_loadedOnce = false;

    uint32_t manifestTimeoutResolve = 5000;
    uint32_t manifestTimeoutConnect = 5000;
    uint32_t manifestTimeoutSend    = 10000;
    uint32_t manifestTimeoutRecv    = 10000;
    LogLevel logLevel = LogLevel::Debug;
    std::string logDir;
    std::vector<std::string> luaPaths;
    std::string remoteUrlTemplate;
    bool statsEnableApi = true;
    std::vector<InjectDll> injectDlls;
    bool cloudEnabled = false;
    std::string cloudLibrary;
    bool manifestLockOwnedGames = false;
    bool manifestAutoSyncOnUpdate = true;

    const char* ToString(LogLevel level) {
        switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        }
        return "???";
    }

    const std::unordered_set<AppId_t>& GetDefaultAntiCheatAppids() {
        static const std::unordered_set<AppId_t> kDefaults = {
            // Valve VAC
            730, 570, 440, 550, 1422450, 240, 300,
            // EAC
            1172470, 252490, 381210, 2073850, 230410, 236390, 976730, 1240440,
            // BattlEye / ACE / Ricochet / nProtect
            578080, 359550, 1085660, 1938090, 2195250, 2669320, 553850
        };
        return kDefaults;
    }

    Snapshot MakeDefaultSnapshot(const std::string& configPath) {
        Snapshot snapshot;
        const char* storageDir = GetStorageDirectory();
        if (storageDir && storageDir[0] != '\0') {
            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                OSTPlatform::Encoding::PathFromUtf8(storageDir) / "opensteamtool");
        } else {
            std::filesystem::path p = OSTPlatform::Encoding::PathFromUtf8(configPath);
            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(p.parent_path() / "opensteamtool");
        }
        return snapshot;
    }

    void ApplySnapshot(const Snapshot& snapshot) {
        manifestTimeoutResolve = snapshot.manifestTimeouts.resolve;
        manifestTimeoutConnect = snapshot.manifestTimeouts.connect;
        manifestTimeoutSend    = snapshot.manifestTimeouts.send;
        manifestTimeoutRecv    = snapshot.manifestTimeouts.recv;
        logLevel               = snapshot.logLevel;
        logDir                 = snapshot.logDir;
        luaPaths               = snapshot.luaPaths;
        remoteUrlTemplate      = snapshot.remoteUrlTemplate;
        statsEnableApi         = snapshot.statsEnableApi;
        injectDlls             = snapshot.injectDlls;
        cloudEnabled           = snapshot.cloud.enabled;
        cloudLibrary           = snapshot.cloud.library;
        manifestLockOwnedGames = snapshot.manifestLockOwnedGames;
        manifestAutoSyncOnUpdate = snapshot.manifestAutoSyncOnUpdate;
    }

    void ApplyManifestProvider(const std::string& provider) {
        if (!ManifestClient::SetProvider(provider)) {
            LOG_WARN("Unknown manifest.url \"{}\", keeping default", provider);
            ManifestClient::SetProvider("manifestdex");
        }
    }

    LoadResult ApplySnapshotLocked(const Snapshot& snapshot) {
        std::lock_guard lock(g_mutex);
        LoadResult result;
        result.luaPathsChanged = luaPaths != snapshot.luaPaths;
        ApplySnapshot(snapshot);
        g_loadedOnce = true;
        result.applied = true;
        return result;
    }

} // namespace

    LoadResult Load(const std::string& configPath) {
        Snapshot snapshot = MakeDefaultSnapshot(configPath);
        std::error_code ec;
        if (!std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(configPath), ec)) {
            LOG_INFO("Config file not found, using defaults");
            ApplyManifestProvider(snapshot.manifestProvider);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ToString(GetLogLevel()),
                     (uint32_t)GetLuaPaths().size(),
                     GetStatsEnableApi(),
                     GetRemoteUrlTemplate().empty() ? "<default>" : GetRemoteUrlTemplate());
            return result;
        }

        try {
            auto tbl = toml::parse_file(OSTPlatform::Encoding::PathFromUtf8(configPath).wstring());

            // [manifest]
            if (auto manifest = tbl["manifest"].as_table()) {
                if (auto val = (*manifest)["url"].value<std::string>()) {
                    snapshot.manifestProvider = *val;
                }
                if (auto val = (*manifest)["timeout_resolve_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.resolve = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_connect_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.connect = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_send_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.send = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_recv_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.recv = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["lock_owned_games"].value<bool>())
                    snapshot.manifestLockOwnedGames = *val;
                if (auto val = (*manifest)["auto_sync_on_update"].value<bool>())
                    snapshot.manifestAutoSyncOnUpdate = *val;
            }

            // [log]
            if (auto log = tbl["log"].as_table()) {
                if (auto val = (*log)["level"].value<std::string>()) {
                    if (*val == "trace")           snapshot.logLevel = LogLevel::Trace;
                    else if (*val == "debug")       snapshot.logLevel = LogLevel::Debug;
                    else if (*val == "info")        snapshot.logLevel = LogLevel::Info;
                    else if (*val == "warn")        snapshot.logLevel = LogLevel::Warn;
                    else if (*val == "error")       snapshot.logLevel = LogLevel::Error;
                }
                if (auto val = (*log)["dir"].value<std::string>()) {
                    std::filesystem::path p = OSTPlatform::Encoding::PathFromUtf8(*val);
                    if (p.is_relative()) {
                        const char* storageDir = GetStorageDirectory();
                        if (storageDir && storageDir[0] != '\0') {
                            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                                OSTPlatform::Encoding::PathFromUtf8(storageDir) / p);
                        } else {
                            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                                OSTPlatform::Encoding::PathFromUtf8(configPath).parent_path() / p);
                        }
                    } else {
                        snapshot.logDir = *val;
                    }
                }
            }

            // [lua]
            if (auto lua = tbl["lua"].as_table()) {
                if (auto arr = (*lua)["paths"].as_array()) {
                    for (auto& elem : *arr) {
                        if (auto str = elem.value<std::string>()) {
                            snapshot.luaPaths.push_back(*str);
                        }
                    }
                }
            }

            // [remote]
            if (auto remote = tbl["remote"].as_table()) {
                if (auto val = (*remote)["url_template"].value<std::string>()) {
                    snapshot.remoteUrlTemplate = *val;
                }
            }

            // [stats]
            if (auto stats = tbl["stats"].as_table()) {
                if (auto val = (*stats)["enable_api"].value<bool>()) {
                    snapshot.statsEnableApi = *val;
                }
            }

            // Global injection exclusion list: read primarily from [injects], with fallbacks to [exclude_appids], [inject_blacklist], or [inject]
            bool hasGlobalExclude = false;
            std::unordered_set<AppId_t> globalExcludeAppids;
            auto parseExclude = [&](const toml::table* tblNode) {
                if (!tblNode) return;
                auto readArr = [&](std::string_view key) {
                    if (auto ids = (*tblNode)[key].as_array()) {
                        hasGlobalExclude = true;
                        for (auto& id : *ids) {
                            if (auto v = id.value<int64_t>()) {
                                globalExcludeAppids.insert(static_cast<AppId_t>(*v));
                            }
                        }
                    }
                };
                readArr("exclude_appids");
                readArr("appids");
            };

            if (auto inj = tbl["injects"].as_table()) {
                parseExclude(inj);
            } else if (auto bl = tbl["exclude_appids"].as_table()) {
                parseExclude(bl);
            } else if (auto bl2 = tbl["inject_blacklist"].as_table()) {
                parseExclude(bl2);
            } else if (auto bl3 = tbl["inject"].as_table()) {
                parseExclude(bl3);
            } else if (auto arr = tbl["exclude_appids"].as_array()) {
                hasGlobalExclude = true;
                for (auto& id : *arr) {
                    if (auto v = id.value<int64_t>()) {
                        globalExcludeAppids.insert(static_cast<AppId_t>(*v));
                    }
                }
            }

            // [[inject]]
            if (auto arr = tbl["inject"].as_array()) {
                std::filesystem::path configDir = OSTPlatform::Encoding::PathFromUtf8(configPath).parent_path();
                for (auto& node : *arr) {
                    auto t = node.as_table();
                    if (!t) continue;
                    auto path = (*t)["path"].value<std::string>();
                    if (!path || path->empty()) continue;

                    // Relative paths resolve next to opensteamtool.toml, DLL dir, or steam.exe
                    std::filesystem::path full = OSTPlatform::Encoding::PathFromUtf8(*path);
                    if (full.is_relative()) {
                        std::filesystem::path candidate = configDir / full;
                        std::error_code ec;
                        if (std::filesystem::exists(candidate, ec)) {
                            full = candidate;
                        } else if (DllDir[0] != '\0' && std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(DllDir) / full, ec)) {
                            full = OSTPlatform::Encoding::PathFromUtf8(DllDir) / full;
                        } else if (SteamInstallPath[0] != '\0' && std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / full, ec)) {
                            full = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / full;
                        } else {
                            full = candidate;
                        }
                    }
                    std::error_code ec;
                    if (!std::filesystem::exists(full, ec)) {
                        LOG_WARN("inject dll not found: {}", OSTPlatform::Encoding::PathToUtf8(full));
                        continue;
                    }

                    InjectDll dll;
                    dll.path = OSTPlatform::Encoding::PathToUtf8(full);
                    if (auto val = (*t)["when_cmdline"].value<std::string>()) dll.whenCmdline = *val;
                    if (auto val = (*t)["all_games"].value<bool>())           dll.allGames   = *val;
                    else                                                      dll.allGames   = false;
                    if (auto ids = (*t)["when_appids"].as_array()) {
                        for (auto& id : *ids) {
                            if (auto v = id.value<int64_t>()) {
                                dll.whenAppids.insert(static_cast<AppId_t>(*v));
                            }
                        }
                    }

                    // Anti-cheat / compatibility exclusion list:
                    // 1. If individual [[inject]] has explicit exclude_appids, use it strictly.
                    // 2. Otherwise, if global [injects].exclude_appids was explicitly configured, use it strictly (no merging).
                    // 3. Otherwise (unconfigured), fall back to built-in default anti-cheat blacklist.
                    if (auto ruleExclude = (*t)["exclude_appids"].as_array()) {
                        for (auto& id : *ruleExclude) {
                            if (auto v = id.value<int64_t>()) {
                                dll.excludeAppids.insert(static_cast<AppId_t>(*v));
                            }
                        }
                    } else if (hasGlobalExclude) {
                        dll.excludeAppids = globalExcludeAppids;
                    } else {
                        dll.excludeAppids = GetDefaultAntiCheatAppids();
                    }

                    // Whitelist priority: if an AppID is explicitly specified in when_appids,
                    // whitelist takes precedence over exclusion list, so it will not be excluded for this rule.
                    for (AppId_t whitelistedId : dll.whenAppids) {
                        dll.excludeAppids.erase(whitelistedId);
                    }

                    snapshot.injectDlls.push_back(std::move(dll));
                }
            }

            // [cloud]
            if (auto cloud = tbl["cloud"].as_table()) {
                if (auto val = (*cloud)["enabled"].value<bool>())
                    snapshot.cloud.enabled = *val;
                if (auto val = (*cloud)["library"].value<std::string>())
                    snapshot.cloud.library = *val;
            }

            ApplyManifestProvider(snapshot.manifestProvider);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ToString(snapshot.logLevel),
                     (uint32_t)snapshot.luaPaths.size(),
                     snapshot.statsEnableApi,
                     snapshot.remoteUrlTemplate.empty() ? "<default>" : snapshot.remoteUrlTemplate);
            return result;

        } catch (const toml::parse_error& e) {
            LOG_WARN("Config parse error: {}", e.what());
        } catch (...) {
            LOG_WARN("Config load failed");
        }
        bool shouldApplyDefault = false;
        {
            std::lock_guard lock(g_mutex);
            shouldApplyDefault = !g_loadedOnce;
        }
        if (shouldApplyDefault) {
            ApplyManifestProvider(snapshot.manifestProvider);
            std::lock_guard lock(g_mutex);
            const bool luaChanged = luaPaths != snapshot.luaPaths;
            ApplySnapshot(snapshot);
            g_loadedOnce = true;
            return {true, luaChanged};
        }
        return {};
    }

    ManifestTimeouts GetManifestTimeouts() {
        std::lock_guard lock(g_mutex);
        return {
            manifestTimeoutResolve,
            manifestTimeoutConnect,
            manifestTimeoutSend,
            manifestTimeoutRecv,
        };
    }

    LogLevel GetLogLevel() {
        std::lock_guard lock(g_mutex);
        return logLevel;
    }

    std::string GetLogDir() {
        std::lock_guard lock(g_mutex);
        return logDir;
    }

    std::vector<std::string> GetLuaPaths() {
        std::lock_guard lock(g_mutex);
        return luaPaths;
    }

    std::string GetRemoteUrlTemplate() {
        std::lock_guard lock(g_mutex);
        return remoteUrlTemplate;
    }

    bool GetStatsEnableApi() {
        std::lock_guard lock(g_mutex);
        return statsEnableApi;
    }

    CloudSettings GetCloudSettings() {
        std::lock_guard lock(g_mutex);
        return {
            cloudEnabled,
            cloudLibrary,
        };
    }

    std::vector<InjectDll> GetInjectDlls() {
        std::lock_guard lock(g_mutex);
        return injectDlls;
    }

    bool GetManifestLockOwnedGames() {
        std::lock_guard lock(g_mutex);
        return manifestLockOwnedGames;
    }

    bool GetManifestAutoSyncOnUpdate() {
        std::lock_guard lock(g_mutex);
        return manifestAutoSyncOnUpdate;
    }

}
