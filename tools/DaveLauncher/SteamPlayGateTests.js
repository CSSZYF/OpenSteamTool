"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "SteamPlayGate.js"), "utf8");

function fixture(options = "-offline", behavior = {}) {
    const calls = [];
    const warnings = [];
    let clock = 0;
    let offline = !!behavior.offline;
    let connected = behavior.connected === undefined ? !offline : behavior.connected;
    let details = options;
    let releaseDetails;
    let timerId = 0;
    let timerScheduled = false;
    const timers = new Map();
    function drainTimers() {
        timerScheduled = false;
        const next = [...timers.values()].sort((a, b) => a.due - b.due || a.id - b.id)[0];
        if (!next) return;
        timers.delete(next.id);
        clock = Math.max(clock, next.due);
        next.callback();
        if (timers.size && !timerScheduled) { timerScheduled = true; setImmediate(drainTimers); }
    }
    const context = {
        Date: { now: () => clock },
        setTimeout: (callback, delay) => {
            const id = ++timerId;
            timers.set(id, { id, callback, due: clock + delay });
            if (!timerScheduled) { timerScheduled = true; setImmediate(drainTimers); }
            return id;
        },
        clearTimeout: (id) => timers.delete(id),
        console: { warn: (message) => warnings.push(message) },
        __OSTOfflinePlayPrerequisite: { noRunningApps: true, sampledAt: 0 },
        appDetailsStore: {
            RequestAppDetails: async (appId) => {
                calls.push(["details", appId]);
                if (behavior.detailsError) throw new Error("private-options-or-ticket-value");
                if (behavior.detailsPending) return new Promise((resolve) => { releaseDetails = resolve; });
                return { strLaunchOptions: details };
            }
        },
        App: {
            BIsOfflineMode: () => offline,
            GetServicesInitialized: () => behavior.ready !== false,
            cm: { BConnectedToServer: () => connected }
        },
        SteamClient: {
            Apps: {
                RunGame: function (...args) { calls.push(["run", this, args, offline, connected]); return "native-result"; }
            },
            User: {
                GoOffline: () => {
                    calls.push(["offline"]);
                    if (behavior.offlineError) throw new Error("private-native-value");
                    if (!behavior.timeout) { offline = true; connected = false; }
                },
                GoOnline: () => { calls.push(["online"]); offline = false; connected = true; }
            }
        }
    };
    context.window = context;
    vm.createContext(context);
    return {
        context, calls, warnings,
        install: () => vm.runInContext(source, context),
        gate: () => context.__OSTOfflinePlayGate,
        run: (...args) => context.SteamClient.Apps.RunGame(...args),
        setOptions: (value) => { details = value; },
        releaseDetails: () => releaseDetails({ strLaunchOptions: details }),
        clock: () => clock
    };
}

const tests = [];
function test(name, body) { tests.push([name, body]); }
function runs(f) { return f.calls.filter((entry) => entry[0] === "run"); }

test("other games immediately preserve native this, arguments and return value", () => {
    const f = fixture(); f.install();
    const receiver = { marker: true };
    const result = f.context.SteamClient.Apps.RunGame.call(receiver, "42", "args", -1, 100);
    assert.equal(result, "native-result");
    assert.equal(f.calls.length, 1);
    assert.equal(runs(f)[0][1], receiver);
    assert.deepEqual(runs(f)[0][2], ["42", "args", -1, 100]);
});

test("normal Dave launch preserves all four original parameters", async () => {
    const f = fixture("-windowed -name \"original value\""); f.install();
    assert.equal(await f.run("1868140", "untouched", -1, 100), "native-result");
    assert.deepEqual(runs(f)[0][2], ["1868140", "untouched", -1, 100]);
    assert.equal(f.calls.some((call) => call[0] === "offline"), false);
});

test("offline is confirmed before the original Steam launch", async () => {
    const f = fixture(); f.install();
    assert.equal(await f.run(1868140, "", -1, 100), "native-result");
    assert.deepEqual(f.calls.map((call) => call[0]), ["details", "offline", "run"]);
    assert.equal(runs(f)[0][3], true);
    assert.equal(runs(f)[0][4], false);
    assert.equal(f.gate().lastEvent, "offline_launch");
    assert.equal(f.gate().busy, false);
});

test("an already offline client launches without toggling either mode", async () => {
    const f = fixture("-offline", { offline: true }); f.install();
    await f.run("1868140", "", -1, 100);
    assert.deepEqual(f.calls.map((call) => call[0]), ["details", "run"]);
});

test("offline flag alone is insufficient while the server connection persists", async () => {
    const f = fixture("-offline", { offline: true, connected: true }); f.install();
    await f.run("1868140");
    assert.equal(runs(f).length, 0);
    assert.equal(f.gate().lastError, "offline_timeout");
    assert.equal(f.calls.some((call) => call[0] === "online"), false);
});

test("timed out transition does not launch and restores original online mode", async () => {
    const f = fixture("-offline", { timeout: true }); f.install();
    await f.run("1868140");
    assert.equal(runs(f).length, 0);
    assert.equal(f.gate().lastError, "offline_timeout");
    assert.equal(f.clock(), 45000);
    assert.deepEqual(f.calls.map((call) => call[0]), ["details", "offline", "online"]);
    const status = f.install();
    assert.equal(status.installed, true);
    assert.equal(status.error, "offline_timeout");
    assert.equal(status.eventId, 1);
    assert.equal(status.busy, false);
    assert.equal(status.lastEvent, "failed");
});

test("duplicate clicks dispatch the game exactly once", async () => {
    const f = fixture(); f.install();
    const first = f.run("1868140", "", -1, 100);
    const second = f.run("1868140", "", -1, 100);
    assert.equal(first, second);
    await first;
    assert.equal(runs(f).length, 1);
});

test("install is idempotent", async () => {
    const f = fixture();
    assert.equal(f.install().installed, true);
    const wrapper = f.gate().wrapper;
    assert.equal(f.install().installed, true);
    assert.equal(f.gate().wrapper, wrapper);
    await f.run("1868140");
    assert.equal(runs(f).length, 1);
});

test("an older installed wrapper is unwrapped rather than nested", async () => {
    const f = fixture(); f.install();
    const original = f.gate().original;
    f.gate().version = "0";
    assert.equal(f.install().installed, true);
    assert.equal(f.gate().original, original);
    await f.run("1868140");
    assert.equal(runs(f).length, 1);
    assert.equal(f.calls.filter((call) => call[0] === "details").length, 1);
});

test("replacement cancels an old pending launch", async () => {
    const f = fixture(); f.install();
    const pending = f.run("1868140");
    f.gate().version = "0";
    f.install();
    await pending;
    assert.equal(runs(f).length, 0);
});

test("only exact Windows argument tokens enable the gate", async () => {
    const cases = [
        ["-OFFLINE", true], ['"-offline"', true], ["\t-offline\t-windowed", true],
        ["--offline", false], ["-offline=false", false], ["prefix-offline", false],
        ['-name "words -offline"', false], ["-logFile -offline", false],
        ['-logFile "-offline"', false], ["-logFile=foo -offline", true],
        ['-logFile "a log" -offline', true], ['-name "a\\\" -offline"', false],
        ['-name "a\\\\" -offline', true], ['-name "a"" -offline"', false],
        ['-name "" -offline', true], ['-name "C:\\path with spaces\\\\" -offline', true]
    ];
    for (const [options, expected] of cases) {
        const f = fixture(options); f.install(); await f.run("1868140");
        assert.equal(f.calls.some((call) => call[0] === "offline"), expected, options);
        assert.equal(runs(f).length, 1, options);
    }
});

test("native failures expose fixed status only and never launch", async () => {
    for (const behavior of [{ detailsError: true }, { offlineError: true }]) {
        const f = fixture("-offline", behavior); f.install(); await f.run("1868140");
        assert.equal(runs(f).length, 0);
        assert.equal(f.gate().lastError, "launch_gate_failed");
        assert.equal(JSON.stringify(f.warnings).includes("private"), false);
        assert.equal(f.gate().busy, false);
    }
});

test("missing state or unready services never launches the requested offline game", async () => {
    const f = fixture("-offline", { ready: false }); f.install(); await f.run("1868140");
    assert.equal(runs(f).length, 0);
    assert.equal(f.gate().lastError, "services_not_ready");
});

test("details timeout clears busy and a late response never dispatches the game", async () => {
    const f = fixture("-offline", { detailsPending: true }); f.install();
    await f.run("1868140");
    assert.equal(f.clock(), 15000);
    assert.equal(f.gate().lastError, "details_timeout");
    assert.equal(f.gate().busy, false);
    assert.equal(runs(f).length, 0);
    f.releaseDetails();
    await new Promise(setImmediate);
    assert.equal(runs(f).length, 0);
    assert.deepEqual(f.calls.map((call) => call[0]), ["details"]);
});

test("offline launch requires a recent explicit no-running-apps sample", async () => {
    const invalid = [undefined, null, {}, { noRunningApps: false, sampledAt: 0 },
        { noRunningApps: true, sampledAt: -6001 }, { noRunningApps: true, sampledAt: 1 },
        { noRunningApps: true, sampledAt: "0" }, { noRunningApps: true, sampledAt: NaN }];
    for (const sample of invalid) {
        const f = fixture(); f.context.__OSTOfflinePlayPrerequisite = sample; f.install();
        await f.run("1868140");
        assert.equal(f.gate().lastError, "apps_running_or_unknown");
        assert.equal(runs(f).length, 0);
        assert.deepEqual(f.calls.map((call) => call[0]), ["details"]);
    }
    const f = fixture();
    f.context.__OSTOfflinePlayPrerequisite = { noRunningApps: true, sampledAt: -6000 };
    f.install(); await f.run("1868140");
    assert.equal(runs(f).length, 1);
});

test("normal launch does not require a mode control API", async () => {
    const f = fixture("-windowed");
    delete f.context.App;
    delete f.context.SteamClient.User;
    delete f.context.__OSTOfflinePlayPrerequisite;
    f.install(); await f.run("1868140");
    assert.equal(runs(f).length, 1);
});

(async function () {
    for (const [name, body] of tests) {
        await body();
        console.log("PASS " + name);
    }
    console.log("SteamPlayGate: " + tests.length + " tests passed.");
})().catch((error) => { console.error(error); process.exitCode = 1; });
