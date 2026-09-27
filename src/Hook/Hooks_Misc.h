#pragma once

#include "dllmain.h"

// Catch-all for lightweight info-capture int3 traps and launch-time rewrites
// that don't fit a dedicated category — currently:
//   * GetAppIDForCurrentPipe  -> captures the SteamEngine pointer
//   * SpawnProcess            -> OnlineFix detection + 480 rewrite
//   * GetAppDataFromAppInfo   -> captures the CAppInfoCache pointer
namespace Hooks_Misc {
    void Install();
    void Uninstall();

    // Returns the AppId for the current Steam pipe via the captured engine
    // pointer, or 0 if we haven't yet observed the host calling
    // GetAppIDForCurrentPipe.
    AppId_t GetAppIDForCurrentPipeWrap();

    // True while a -onlinefix game is the active spawn.
    bool IsOnlineFixActive();

    // True once SteamNetworkingSockets (IPC interface 46) has been observed.
    bool IsNetworkingSocketsActive();

    // Call when the game uses SteamNetworkingSockets (IPC interface 46).
    void NotifyNetworkingSocketsUsed();

    // True once P2P started — GetAppID reports 480; before, the real appid.
    bool ShouldReportOnlineFixAppId();

    // Grow a CUtlBuffer to at least 'newCapacity' bytes and set m_Put = newCapacity.
    // Uses CUtlBuffer::EnsureCapacity from steamclient, resolved on first call.
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity,bool updatePut = false);

    // Resolve the real appid: if OnlineFix is active and pipe/context matches 480
    // return real appid, otherwise fall back to GetAppIDForCurrentPipe().
    AppId_t ResolveAppId();

    // Atomically reset OnlineFix state (called when Steam signals exit sync completion
    // or when a non-onlinefix game launches).
    void ResetOnlineFixState();

    // DLC-starve window (experimental): armed briefly when a Lua-unlocked game spawns.
    // While active, Hooks_NetPacket drops the base game's outbound server ownership /
    // family requests so the client falls back to LOCAL (package 0) ownership — i.e.
    // "thinks it's offline" for that window, which makes injected DLC on a family-shared
    // base game resolve as owned while the client stays online.
    void BeginDlcStarveWindow();
    bool IsDlcStarveWindow();

    // Get localized game name via GetAppDataFromAppInfo (cached).
    std::string GetGameNameByAppID(AppId_t appId);

}
