#pragma once
#include <string>

namespace RemoteToml {

    enum class FetchMode {
        PreferCache,
        RemoteOnly,
    };

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

    // Prefer the exact local cache during normal startup. RemoteOnly bypasses
    // that cache and is reserved for an explicit lazy refresh.
    Result Fetch(const Request& request, FetchMode mode = FetchMode::PreferCache);

} // namespace RemoteToml
