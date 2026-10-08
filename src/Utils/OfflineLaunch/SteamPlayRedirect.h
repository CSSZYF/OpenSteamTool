#pragma once

#include "OSTPlatform/include/Trap.h"
#include "Steam/Types.h"

namespace SteamPlayRedirect {

// Replaces only the supported game's process arguments. The original Steam
// SpawnProcess call still creates and tracks the helper normally; it never waits
// inside a trap handler or fabricates an undocumented function return value.
void TryRedirect(OSTPlatform::Trap::Context& context, AppId_t appId,
                 const char* executable, const char* commandLine);

} // namespace SteamPlayRedirect
