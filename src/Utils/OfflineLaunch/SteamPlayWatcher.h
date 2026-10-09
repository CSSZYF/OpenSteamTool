#pragma once

namespace SteamPlayWatcher {

// Starts the optional Play gate companion after Steam initialization. The
// companion watches the supplied Steam PID and exits when that process exits.
void Start();

} // namespace SteamPlayWatcher
