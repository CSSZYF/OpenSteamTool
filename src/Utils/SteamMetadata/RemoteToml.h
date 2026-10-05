#pragma once
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;
    };

    // Load the exact local cache entry first. Only hit the network synchronously
    // when no usable cache exists, keeping metadata lookup off SteamUI's critical
    // startup path once a matching SHA has been cached.
    Result Fetch(const Request& request);

    // Best-effort refresh for cache entries that were served by Fetch() during
    // this startup. Call only after critical initialization is complete; updated
    // metadata is written atomically for the next launch and is not hot-reloaded.
    void RefreshQueuedCaches();

} // namespace RemoteToml
