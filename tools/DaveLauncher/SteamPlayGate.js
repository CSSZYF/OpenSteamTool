(function () {
    "use strict";

    var version = "1";
    var appId = "1868140";
    var timeoutMs = 45000;
    var detailsTimeoutMs = 15000;
    var pollMs = 250;
    var client = window.SteamClient;
    if (!client || !client.Apps || typeof client.Apps.RunGame !== "function") {
        return { installed: false, version: version, reason: "launch_api_unavailable" };
    }

    var apps = client.Apps;
    var previous = window.__OSTOfflinePlayGate;
    function snapshot(gate) {
        return {
            installed: true,
            version: version,
            error: gate.lastError || "",
            eventId: gate.generation || 0,
            busy: !!gate.busy,
            lastEvent: gate.lastEvent || "installed"
        };
    }
    if (previous && previous.version === version && previous.wrapper === apps.RunGame) {
        return snapshot(previous);
    }
    if (previous) {
        previous.generation = (previous.generation || 0) + 1;
        if (previous.wrapper === apps.RunGame && typeof previous.original === "function") {
            apps.RunGame = previous.original;
        }
    }

    var original = apps.RunGame;
    var state = {
        version: version,
        original: original,
        wrapper: null,
        busy: false,
        lastError: "",
        lastEvent: "installed",
        generation: 0
    };
    var pending = null;

    // Parse launch options using Windows backslash/quote rules. Unlike a full
    // process command line, this string has no specially parsed argv[0].
    function splitOptions(text) {
        var result = [];
        var index = 0;
        while (index < text.length) {
            while (text[index] === " " || text[index] === "\t") index++;
            if (index >= text.length) break;
            var value = "";
            var quoted = false;
            while (index < text.length) {
                var slashes = 0;
                while (text[index] === "\\") { slashes++; index++; }
                if (text[index] === '"') {
                    value += "\\".repeat(Math.floor(slashes / 2));
                    if (slashes % 2) {
                        value += '"';
                        index++;
                    } else if (quoted && text[index + 1] === '"') {
                        value += '"';
                        index += 2;
                    } else {
                        quoted = !quoted;
                        index++;
                    }
                } else {
                    value += "\\".repeat(slashes);
                    if (index >= text.length || (!quoted && (text[index] === " " || text[index] === "\t"))) break;
                    value += text[index++];
                }
            }
            result.push(value);
        }
        return result;
    }

    function needsOffline(options) {
        var args = splitOptions(options);
        for (var index = 0; index < args.length; index++) {
            var arg = args[index].toLowerCase();
            if (arg === "-logfile") { index++; continue; }
            if (arg === "-offline") return true;
        }
        return false;
    }

    function readState() {
        var app = window.App;
        if (!app || typeof app.BIsOfflineMode !== "function" ||
            typeof app.GetServicesInitialized !== "function" || !app.cm ||
            typeof app.cm.BConnectedToServer !== "function") throw new Error("state_api_unavailable");
        var offline = app.BIsOfflineMode();
        var connected = app.cm.BConnectedToServer();
        var ready = app.GetServicesInitialized();
        if (typeof offline !== "boolean" || typeof connected !== "boolean" || typeof ready !== "boolean") {
            throw new Error("state_invalid");
        }
        return { offline: offline, connected: connected, ready: ready };
    }

    function isCurrent(generation) {
        return window.__OSTOfflinePlayGate === state && state.generation === generation && apps.RunGame === state.wrapper;
    }

    async function readDetails() {
        var timer;
        try {
            return await Promise.race([
                Promise.resolve().then(function () { return window.appDetailsStore.RequestAppDetails(Number(appId)); }),
                new Promise(function (resolve, reject) {
                    timer = setTimeout(function () { reject(new Error("details_timeout")); }, detailsTimeoutMs);
                })
            ]);
        } finally {
            clearTimeout(timer);
        }
    }

    function requireNoRunningApps() {
        var prerequisite = window.__OSTOfflinePlayPrerequisite;
        if (!prerequisite || prerequisite.noRunningApps !== true ||
            typeof prerequisite.sampledAt !== "number" || !Number.isFinite(prerequisite.sampledAt)) {
            throw new Error("apps_running_or_unknown");
        }
        var age = Date.now() - prerequisite.sampledAt;
        if (age < 0 || age > 6000) throw new Error("apps_running_or_unknown");
    }

    async function launch(receiver, args, generation) {
        var restoreOnline = false;
        var dispatched = false;
        try {
            state.lastEvent = "reading_options";
            if (!window.appDetailsStore || typeof window.appDetailsStore.RequestAppDetails !== "function") {
                throw new Error("details_api_unavailable");
            }
            var details = await readDetails();
            if (!isCurrent(generation)) throw new Error("gate_replaced");
            if (!details || typeof details.strLaunchOptions !== "string") throw new Error("options_unavailable");
            if (!needsOffline(details.strLaunchOptions)) {
                state.lastEvent = "normal_launch";
                dispatched = true;
                return original.apply(receiver, args);
            }

            var current = readState();
            if (!current.ready) throw new Error("services_not_ready");
            requireNoRunningApps();
            var deadline = Date.now() + timeoutMs;
            if (!current.offline) {
                if (!client.User || typeof client.User.GoOffline !== "function") throw new Error("offline_api_unavailable");
                restoreOnline = true;
                state.lastEvent = "requesting_offline";
                client.User.GoOffline();
            }
            state.lastEvent = "waiting_offline";
            while (true) {
                if (!isCurrent(generation)) throw new Error("gate_replaced");
                current = readState();
                if (current.ready && current.offline && !current.connected) break;
                if (Date.now() >= deadline) throw new Error("offline_timeout");
                await new Promise(function (resolve) { setTimeout(resolve, pollMs); });
            }
            if (!isCurrent(generation)) throw new Error("gate_replaced");
            state.lastEvent = "offline_launch";
            // Once handed back to Steam, the helper owns launch/readiness/recovery.
            // Never bring Steam online from this gate after dispatching the game.
            dispatched = true;
            return original.apply(receiver, args);
        } catch (error) {
            var allowed = ["state_api_unavailable", "state_invalid", "details_api_unavailable", "details_timeout", "apps_running_or_unknown",
                "gate_replaced", "options_unavailable", "services_not_ready", "offline_api_unavailable", "offline_timeout"];
            var reason = error && allowed.indexOf(error.message) >= 0 ? error.message : "launch_gate_failed";
            state.lastError = reason;
            state.lastEvent = "failed";
            if (restoreOnline && !dispatched && isCurrent(generation) && client.User && typeof client.User.GoOnline === "function") {
                try { client.User.GoOnline(); }
                catch (ignored) { state.lastError = "online_restore_failed"; }
            }
            // Do not copy native exceptions, launch options, tickets or UI data.
            console.warn("OpenSteamTool offline launch gate stopped before launch.");
            return undefined;
        } finally {
            state.busy = false;
            pending = null;
        }
    }

    state.wrapper = function () {
        if (String(arguments[0]) !== appId) return original.apply(this, arguments);
        if (state.busy) return pending;
        state.busy = true;
        state.lastError = "";
        var generation = ++state.generation;
        pending = launch(this, Array.prototype.slice.call(arguments), generation);
        return pending;
    };
    apps.RunGame = state.wrapper;
    if (apps.RunGame !== state.wrapper) return { installed: false, version: version, reason: "launch_api_readonly" };
    window.__OSTOfflinePlayGate = state;
    return snapshot(state);
})()
