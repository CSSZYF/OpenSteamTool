#include "RemoteToml.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <vector>
#include <windows.h>

namespace RemoteToml {

namespace {
    constexpr const char* kGithubTemplate =
        "https://raw.githubusercontent.com/mmxlyo/steam-monitor/"
        "{channel}/{component}/{sha256}.toml";
    constexpr const char* kJsdelivrTemplate =
        "https://cdn.jsdelivr.net/gh/mmxlyo/steam-monitor@"
        "{channel}/{component}/{sha256}.toml";

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    static std::vector<std::string> BuildUrlTemplates()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (remoteUrlTemplate.empty())
            return { kGithubTemplate, kJsdelivrTemplate };

        if (!IsValidTemplate(remoteUrlTemplate)) {
            LOG_WARN("RemoteToml: remote.url_template must contain "
                     "{channel}, {component}, and {sha256}; remote fetch disabled");
            return {};
        }

        return { remoteUrlTemplate };
    }

    static std::string ReadNonEmptyFile(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            return {};

        const auto sz = std::filesystem::file_size(path, ec);
        if (ec || sz == 0)
            return {};

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return {};

        std::string body(static_cast<size_t>(sz), '\0');
        if (!ifs.read(body.data(), static_cast<std::streamsize>(sz)))
            return {};
        return body;
    }

    struct CacheValidator {
        std::string url;
        std::string etag;
    };

    static bool IsUsableEtag(std::string_view etag)
    {
        return !etag.empty() && etag.find_first_of("\r\n") == std::string_view::npos;
    }

    static CacheValidator ReadValidator(const std::filesystem::path& path)
    {
        CacheValidator validator;
        const std::string body = ReadNonEmptyFile(path);
        const size_t split = body.find('\n');
        if (split == std::string::npos)
            return validator;

        validator.url = body.substr(0, split);
        size_t etagEnd = body.find('\n', split + 1);
        if (etagEnd == std::string::npos)
            etagEnd = body.size();
        validator.etag = body.substr(split + 1, etagEnd - split - 1);

        if (!validator.url.empty() && validator.url.back() == '\r')
            validator.url.pop_back();
        if (!validator.etag.empty() && validator.etag.back() == '\r')
            validator.etag.pop_back();

        if (validator.url.empty() || !IsUsableEtag(validator.etag))
            return {};
        return validator;
    }

    struct CachePaths {
        std::filesystem::path cacheDir;
        std::filesystem::path cachePath;
        std::filesystem::path localPath;
        std::filesystem::path validatorPath;
        std::filesystem::path localValidatorPath;
    };

    struct PendingRefresh {
        Request request;
        std::string sha256;
    };

    static std::mutex g_refreshMutex;
    static std::vector<PendingRefresh> g_refreshQueue;

    static CachePaths ResolveCachePaths(const Request& request, std::string_view sha256)
    {
        namespace fs = std::filesystem;
        using OSTPlatform::Encoding::PathFromUtf8;

        const fs::path steamRoot = PathFromUtf8(request.dllPath).parent_path();
        fs::path baseDir = PathFromUtf8(GetStorageDirectory());
        if (baseDir.empty())
            baseDir = steamRoot;

        CachePaths paths;
        paths.cacheDir = baseDir / "opensteamtool" / request.channel / request.component;
        paths.cachePath = paths.cacheDir / (std::string(sha256) + ".toml");
        paths.localPath = paths.cachePath;
        paths.validatorPath = paths.cacheDir / (std::string(sha256) + ".etag");
        paths.localValidatorPath = paths.validatorPath;

        std::error_code ec;
        if ((!fs::exists(paths.localPath, ec) || ec) && IsPortableMode()) {
            ec.clear();
            const fs::path steamCachePath =
                steamRoot / "opensteamtool" / request.channel / request.component /
                (std::string(sha256) + ".toml");
            if (fs::exists(steamCachePath, ec) && !ec) {
                paths.localPath = steamCachePath;
                paths.localValidatorPath = steamCachePath.parent_path() /
                    (std::string(sha256) + ".etag");
            }
        }
        return paths;
    }

    static void QueueRefresh(const Request& request, std::string_view sha256)
    {
        std::lock_guard<std::mutex> lock(g_refreshMutex);
        for (const auto& pending : g_refreshQueue) {
            if (pending.sha256 == sha256 &&
                pending.request.channel == request.channel &&
                pending.request.component == request.component) {
                return;
            }
        }
        g_refreshQueue.push_back({request, std::string(sha256)});
    }

    static OSTPlatform::Http::Result FetchRemote(const Request& request,
                                                 std::string_view sha256,
                                                 bool allowFallback,
                                                 std::string* outLastUrl)
    {
        const std::vector<std::string> urlTemplates = BuildUrlTemplates();
        OSTPlatform::Http::Result http;
        const size_t sourceCount = allowFallback
            ? urlTemplates.size()
            : (urlTemplates.empty() ? 0 : 1);

        for (size_t i = 0; i < sourceCount; ++i) {
            std::string url = ExpandTemplate(urlTemplates[i], request, sha256);
            if (outLastUrl)
                *outLastUrl = url;

            LOG_INFO("RemoteToml({}/{}): requesting {}",
                     request.channel, request.component, url);

            http = OSTPlatform::Http::Execute(L"GET", url.c_str(),
                                              nullptr, 0, nullptr);

            if (http.ok && http.status == 200)
                break;

            if (http.ok && http.status == 404) {
                LOG_WARN("RemoteToml({}/{}): mirror has no such file (HTTP 404): {}",
                         request.channel, request.component, url);
                break;   // all mirrors serve the same tracker data
            }

            if (i + 1 < sourceCount) {
                LOG_WARN("RemoteToml({}/{}): mirror failed ({} ok={} HTTP={}), falling back",
                         request.channel, request.component, url, http.ok, http.status);
            }
        }
        return http;
    }

    static bool WriteCacheAtomically(const CachePaths& paths,
                                     std::string_view sha256,
                                     std::string_view body)
    {
        namespace fs = std::filesystem;
        using OSTPlatform::Encoding::PathToUtf8;

        std::error_code mkdirEc;
        fs::create_directories(paths.cacheDir, mkdirEc);
        if (mkdirEc) {
            LOG_WARN("RemoteToml: could not create cache dir {} ({})",
                     PathToUtf8(paths.cacheDir), mkdirEc.message());
        }

        const fs::path tempPath = paths.cacheDir /
            (std::string(sha256) + ".tmp." +
             std::to_string(::GetCurrentProcessId()) + "." +
             std::to_string(::GetCurrentThreadId()));

        bool writeOk = false;
        {
            std::ofstream ofs(tempPath, std::ios::binary);
            if (ofs) {
                ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
                ofs.flush();
                writeOk = ofs.good();
            }
        }

        if (!writeOk) {
            std::error_code rmEc;
            fs::remove(tempPath, rmEc);
            LOG_WARN("RemoteToml: could not write cache {}", PathToUtf8(tempPath));
            return false;
        }

        if (MoveFileExW(tempPath.c_str(), paths.cachePath.c_str(), MOVEFILE_REPLACE_EXISTING))
            return true;

        std::error_code renameEc;
        fs::rename(tempPath, paths.cachePath, renameEc);
        if (!renameEc)
            return true;

        LOG_WARN("RemoteToml: atomic cache rename failed for {} ({})",
                 PathToUtf8(paths.cachePath), renameEc.message());
        std::error_code rmEc;
        fs::remove(tempPath, rmEc);
        return false;
    }

    static bool WriteValidator(const std::filesystem::path& path,
                               std::string_view url,
                               std::string_view etag)
    {
        if (url.empty() || !IsUsableEtag(etag))
            return false;

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
        ofs << url << '\n' << etag << '\n';
        return ofs.good();
    }

    static void RemoveFileBestEffort(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    static std::string CanonicalUrl(const Request& request, std::string_view sha256)
    {
        const std::vector<std::string> urlTemplates = BuildUrlTemplates();
        if (urlTemplates.empty())
            return {};
        return ExpandTemplate(urlTemplates.front(), request, sha256);
    }
} // namespace

Result Fetch(const Request& request)
{
    using OSTPlatform::Encoding::PathToUtf8;
    Result out;

    // 1. SHA-256 of the DLL.
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256OfFile failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Cache first: once metadata for an exact DLL SHA exists locally,
    // SteamUI must not wait on network timeouts before hooks can be installed.
    const CachePaths paths = ResolveCachePaths(request, out.sha256);
    const std::string cachedBody = ReadNonEmptyFile(paths.localPath);
    if (!cachedBody.empty()) {
        LOG_INFO("RemoteToml({}/{}): loaded from cache {}",
                 request.channel, request.component, PathToUtf8(paths.localPath));
        QueueRefresh(request, out.sha256);
        out.body = cachedBody;
        out.ok = true;
        out.fromCache = true;
        return out;
    }

    // 3. Cache miss -> synchronously fetch remote metadata because startup
    // cannot use these hooks without a matching file.
    std::string lastUrl;
    OSTPlatform::Http::Result http =
        FetchRemote(request, out.sha256, true, &lastUrl);

    // 4. Remote OK → write cache atomically, return body.
    if (http.ok && http.status == 200 && !http.body.empty()) {
        if (WriteCacheAtomically(paths, out.sha256, http.body)) {
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, PathToUtf8(paths.cachePath));

            const std::string canonicalUrl = CanonicalUrl(request, out.sha256);
            if (lastUrl == canonicalUrl && IsUsableEtag(http.etag)) {
                if (!WriteValidator(paths.validatorPath, lastUrl, http.etag))
                    RemoveFileBestEffort(paths.validatorPath);
            } else {
                RemoveFileBestEffort(paths.validatorPath);
            }
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Total failure — caller handles popup / degraded mode.
    LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
             request.channel, request.component,
             lastUrl.empty() ? "<none>" : lastUrl, http.status);
    return out;
}

void RefreshQueuedCaches()
{
    using OSTPlatform::Encoding::PathToUtf8;

    std::vector<PendingRefresh> pending;
    {
        std::lock_guard<std::mutex> lock(g_refreshMutex);
        pending.swap(g_refreshQueue);
    }

    for (const auto& item : pending) {
        const CachePaths paths = ResolveCachePaths(item.request, item.sha256);
        const std::string canonicalUrl = CanonicalUrl(item.request, item.sha256);
        if (canonicalUrl.empty()) {
            LOG_INFO("RemoteToml({}/{}): background refresh disabled; keeping cache {}",
                     item.request.channel, item.request.component,
                     PathToUtf8(paths.localPath));
            continue;
        }

        const CacheValidator validator = ReadValidator(paths.localValidatorPath);
        const bool hasValidator = validator.url == canonicalUrl &&
                                  IsUsableEtag(validator.etag);

        std::wstring requestHeaders;
        if (hasValidator) {
            requestHeaders = L"If-None-Match: ";
            requestHeaders += OSTPlatform::Encoding::Utf8ToWide(validator.etag);
            requestHeaders += L"\r\n";
        }

        LOG_INFO("RemoteToml({}/{}): validating {}{}",
                 item.request.channel, item.request.component, canonicalUrl,
                 hasValidator ? " with ETag" : "");
        OSTPlatform::Http::Result http = OSTPlatform::Http::Execute(
            L"GET", canonicalUrl.c_str(), nullptr, 0,
            hasValidator ? requestHeaders.c_str() : nullptr);

        if (http.ok && http.status == 304 && hasValidator) {
            LOG_DEBUG("RemoteToml({}/{}): cache validator matched; remote metadata unchanged",
                      item.request.channel, item.request.component);
            continue;
        }

        if (!http.ok || http.status != 200 || http.body.empty()) {
            LOG_INFO("RemoteToml({}/{}): background refresh unavailable; keeping cache {}",
                     item.request.channel, item.request.component,
                     PathToUtf8(paths.localPath));
            continue;
        }

        const std::string cachedBody = ReadNonEmptyFile(paths.localPath);
        std::filesystem::path validatorTarget = paths.localValidatorPath;
        if (http.body == cachedBody) {
            LOG_DEBUG("RemoteToml({}/{}): cache already current",
                      item.request.channel, item.request.component);
        } else if (WriteCacheAtomically(paths, item.sha256, http.body)) {
            LOG_INFO("RemoteToml({}/{}): refreshed cache {}; update applies next startup",
                     item.request.channel, item.request.component,
                     PathToUtf8(paths.cachePath));
            validatorTarget = paths.validatorPath;
        } else {
            continue;
        }

        if (IsUsableEtag(http.etag)) {
            if (!WriteValidator(validatorTarget, canonicalUrl, http.etag)) {
                RemoveFileBestEffort(validatorTarget);
            } else if (validatorTarget == paths.validatorPath &&
                       paths.localValidatorPath != paths.validatorPath) {
                RemoveFileBestEffort(paths.localValidatorPath);
            } else if (validatorTarget == paths.localValidatorPath &&
                       paths.validatorPath != paths.localValidatorPath) {
                RemoveFileBestEffort(paths.validatorPath);
            }
        } else {
            RemoveFileBestEffort(paths.localValidatorPath);
            if (paths.validatorPath != paths.localValidatorPath)
                RemoveFileBestEffort(paths.validatorPath);
        }
    }
}

} // namespace RemoteToml
