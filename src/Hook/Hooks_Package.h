#pragma once

#include "dllmain.h"

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // Queries whether the current Steam account genuinely owns the license for this AppId/DLC.
    // Checks memory cache first, falling back to direct oCheckAppOwnership if CUser is captured.
    bool IsAppTrulyOwned(AppId_t appId);

}
