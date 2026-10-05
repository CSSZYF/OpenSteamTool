#pragma once

namespace HookManager {
    void InstallEarlyUIHooks();
    void InstallUIHooks();
    void UninstallUIHooks();

    void InstallClientHooks();
    void UninstallClientHooks();

    void DetachWorkerThreads();
}
