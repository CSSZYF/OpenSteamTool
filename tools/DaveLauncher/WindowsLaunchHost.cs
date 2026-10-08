using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace DaveLauncher
{
    internal sealed class WindowsLaunchHost : ILaunchHost, IDisposable
    {
        private readonly string baseDir;
        private readonly LauncherConfig config;
        private readonly NativeSteamControl control = new NativeSteamControl("http://127.0.0.1:8080/");
        private readonly string sessionDir;
        private readonly string gameLog;
        private readonly List<string> events = new List<string>();
        private string steamExe;
        private string gameExe;
        private Process game;
        private DateTime launchTime;
        private bool startRequested;
        private bool completed;
        private readonly bool probe;

        public WindowsLaunchHost(string directory, LauncherConfig config, bool probe = false)
        {
            baseDir = directory;
            this.config = config;
            this.probe = probe;
            sessionDir = Path.Combine(directory, "sessions", DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(sessionDir);
            gameLog = Path.Combine(sessionDir, "Dave.Player.log");
            Report("开始戴夫原生离线启动流程。");
        }

        public void Preview()
        {
            ResolvePaths();
            CheckNoGames();
            Report("预览通过：游戏已安装，原生控制标记存在。未切换模式或启动进程。");
        }

        private void ResolvePaths()
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")) {
                string value = key == null ? null : key.GetValue("SteamPath") as string;
                if (String.IsNullOrWhiteSpace(value)) throw new InvalidOperationException("找不到 Steam 安装目录。");
                steamExe = Path.Combine(Path.GetFullPath(value), "steam.exe");
            }
            if (!File.Exists(steamExe)) throw new InvalidOperationException("Steam 主程序不存在。");
            string root = Path.GetDirectoryName(steamExe);
            if (!File.Exists(Path.Combine(root, ".cef-enable-remote-debugging")))
                throw new InvalidOperationException("当前 Steam 未启用本机原生控制入口。需要启用 CEF 本机调试后重启 Steam。");
            var libraries = new List<string> { root };
            string folders = Path.Combine(root, "steamapps", "libraryfolders.vdf");
            if (File.Exists(folders)) {
                string text = ReadLimited(folders, 1024 * 1024);
                foreach (Match match in Regex.Matches(text, "\"path\"\\s+\"([^\"]+)\""))
                    libraries.Add(match.Groups[1].Value.Replace("\\\\", "\\"));
            }
            foreach (string library in libraries.Distinct(StringComparer.OrdinalIgnoreCase)) {
                string manifest = Path.Combine(library, "steamapps", "appmanifest_1868140.acf");
                if (!File.Exists(manifest)) continue;
                string text = ReadLimited(manifest, 1024 * 1024);
                Match install = Regex.Match(text, "\"installdir\"\\s+\"([^\"]+)\"");
                if (!install.Success) continue;
                string common = Path.GetFullPath(Path.Combine(library, "steamapps", "common")) + Path.DirectorySeparatorChar;
                string candidate = Path.GetFullPath(Path.Combine(common, install.Groups[1].Value, "DaveTheDiver.exe"));
                if (!candidate.StartsWith(common, StringComparison.OrdinalIgnoreCase)) continue;
                if (File.Exists(candidate)) { gameExe = candidate; break; }
            }
            if (gameExe == null) throw new InvalidOperationException("未找到潜水员戴夫（1868140）的已安装主程序。");
        }

        public async Task PrepareAsync(CancellationToken token)
        {
            ResolvePaths();
            CheckNoGames();
            Process[] clients = Process.GetProcessesByName("steam");
            try {
                foreach (Process client in clients) {
                    if (!String.Equals(client.MainModule.FileName, steamExe, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("正在运行的 Steam 路径与注册安装位置不一致。");
                }
                if (clients.Length == 0) {
                    Report("启动 Steam，等待登录缓存和客户端初始化。");
                    using (Process started = Process.Start(new ProcessStartInfo(steamExe, "-silent") {
                        UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(steamExe)
                    })) { }
                }
            }
            finally { foreach (Process client in clients) client.Dispose(); }
            DateTime deadline = DateTime.UtcNow.AddSeconds(120);
            while (DateTime.UtcNow < deadline) {
                token.ThrowIfCancellationRequested();
                SteamState state = null;
                try { state = await control.ReadStateAsync(token).ConfigureAwait(false); }
                catch (OperationCanceledException) { throw; }
                catch (Exception) { }
                if (state != null && state.ServicesReady) {
                    CheckNoGames();
                    Report("Steam 原生控制就绪。");
                    return;
                }
                await Task.Delay(750, token).ConfigureAwait(false);
            }
            throw new InvalidOperationException("Steam 原生接口未就绪。若登录缓存过期，请先登录；没有启动游戏或改写登录配置。");
        }

        private static void CheckNoGames()
        {
            Process[] games = Process.GetProcessesByName("DaveTheDiver");
            try { if (games.Length != 0) throw new InvalidOperationException("戴夫已经在运行，请先退出游戏。"); }
            finally { foreach (Process process in games) process.Dispose(); }
            using (RegistryKey apps = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\Apps")) {
                if (apps == null) return;
                foreach (string name in apps.GetSubKeyNames()) using (RegistryKey app = apps.OpenSubKey(name)) {
                    if (app != null && Convert.ToInt32(app.GetValue("Running", 0)) != 0)
                        throw new InvalidOperationException("Steam 中仍有应用运行（" + name + "），已停止切换离线。");
                }
            }
        }

        public Task<SteamState> ReadSteamAsync(CancellationToken token) { return control.ReadStateAsync(token); }
        public async Task GoOfflineAsync(CancellationToken token)
        {
            CheckNoGames();
            Report("请求 Steam 原生离线模式。");
            await control.GoOfflineAsync(token).ConfigureAwait(false);
        }
        public Task WaitForOfflineAsync(CancellationToken token) { return WaitModeAsync(true, token); }
        public Task WaitForOnlineAsync(CancellationToken token) { return WaitModeAsync(false, token); }
        public async Task GoOnlineAsync(CancellationToken token)
        {
            Report("请求 Steam 原生在线模式。");
            await control.GoOnlineAsync(token).ConfigureAwait(false);
        }
        private async Task WaitModeAsync(bool offline, CancellationToken token)
        {
            DateTime deadline = DateTime.UtcNow.AddSeconds(45);
            while (DateTime.UtcNow < deadline) {
                token.ThrowIfCancellationRequested();
                SteamState state = null;
                try { state = await control.ReadStateAsync(token).ConfigureAwait(false); }
                catch (OperationCanceledException) { throw; }
                catch (Exception) { }
                if (state != null && state.ServicesReady && state.Offline == offline && state.Connected != offline) {
                    Report(offline ? "已确认 Steam 离线且后台连接断开。" : "已确认 Steam 在线且后台连接恢复。");
                    return;
                }
                await Task.Delay(500, token).ConfigureAwait(false);
            }
            throw new InvalidOperationException(offline ? "Steam 离线切换未确认，游戏未启动。" : "Steam 上线连接尚未恢复，请检查网络后使用恢复入口。");
        }

        public async Task StartGameAsync(CancellationToken token)
        {
            CheckNoGames();
            SteamState state = await ReadSteamAsync(token).ConfigureAwait(false);
            if (!state.Offline || state.Connected) throw new InvalidOperationException("Steam 离线状态已改变，已停止启动。");
            if (File.Exists(gameLog)) throw new InvalidOperationException("本次专用游戏日志路径被占用。");
            launchTime = DateTime.UtcNow;
            string command = "-applaunch 1868140 -logFile \"" + gameLog + "\"";
            startRequested = true; // Keep offline while Steam may still create the process.
            using (Process request = Process.Start(new ProcessStartInfo(steamExe, command) {
                UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(steamExe)
            })) { }
            Report("已发送戴夫启动请求；本轮使用独立游戏日志。");
            DateTime deadline = DateTime.UtcNow.AddSeconds(60);
            while (DateTime.UtcNow < deadline) {
                token.ThrowIfCancellationRequested();
                foreach (Process process in Process.GetProcessesByName("DaveTheDiver")) {
                    bool keep = false;
                    try {
                        if (!process.HasExited && process.StartTime.ToUniversalTime() >= launchTime.AddSeconds(-1) &&
                            String.Equals(process.MainModule.FileName, gameExe, StringComparison.OrdinalIgnoreCase)) {
                            game = process; keep = true;
                            Report("已绑定本次戴夫进程 PID=" + game.Id + "。");
                            return;
                        }
                    }
                    finally { if (!keep) process.Dispose(); }
                }
                await Task.Delay(500, token).ConfigureAwait(false);
            }
            throw new InvalidOperationException("游戏启动未确认，保持离线以免延迟启动时提前联网。");
        }

        public bool IsGameRunning()
        {
            if (game != null) return !game.HasExited;
            return startRequested;
        }

        public async Task WaitForGameReadyAsync(CancellationToken token)
        {
            DateTime deadline = DateTime.UtcNow.AddSeconds(config.ReadyTimeoutSeconds);
            DateTime? firstReady = null;
            string lastReason = null;
            while (DateTime.UtcNow < deadline) {
                token.ThrowIfCancellationRequested();
                if (!IsGameRunning()) throw new InvalidOperationException("戴夫在初始化完成前退出。");
                SteamState state = await ReadSteamAsync(token).ConfigureAwait(false);
                if (!state.Offline || state.Connected) throw new InvalidOperationException("初始化过程中 Steam 提前上线，本轮自动流程已停止。");
                if (File.Exists(gameLog)) {
                    var info = new FileInfo(gameLog);
                    if (info.CreationTimeUtc < launchTime.AddSeconds(-2)) throw new InvalidOperationException("游戏日志不属于本次启动。");
                    string text = ReadLimited(gameLog, 8 * 1024 * 1024);
                    ReadinessResult result = DaveReadiness.Analyze(text, config.ExpectedDlcs);
                    if (lastReason != result.Reason) { Report(result.Reason); lastReason = result.Reason; }
                    if (result.Failed) throw new InvalidOperationException("游戏初始化检查失败，保持离线：" + result.Reason);
                    if (result.Ready) {
                        if (!firstReady.HasValue) firstReady = DateTime.UtcNow;
                        game.Refresh();
                        // An observed title sequence is mandatory; this delay only
                        // protects the last transition, never substitutes for evidence.
                        if (DateTime.UtcNow - firstReady.Value >= TimeSpan.FromSeconds(config.TitleSettleSeconds) &&
                            game.MainWindowHandle != IntPtr.Zero && game.Responding) {
                            Report("本轮全部预期 DLC 本地检查、离线票据流程及标题初始化已完成，窗口正常响应。");
                            return;
                        }
                    }
                    else firstReady = null;
                }
                await Task.Delay(500, token).ConfigureAwait(false);
            }
            throw new InvalidOperationException("未等到本轮 DLC 和标题初始化完成，保持 Steam 离线。请查看 sessions 中的日志。");
        }

        private static string ReadLimited(string path, int limit)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
                if (stream.Length > limit) throw new InvalidOperationException("日志或清单超过读取上限，停止自动流程。");
                using (var reader = new StreamReader(stream, Encoding.UTF8, true)) return reader.ReadToEnd();
            }
        }
        public void Report(string message)
        {
            string line = DateTimeOffset.Now.ToString("o") + " " + message;
            events.Add(line);
            File.AppendAllText(Path.Combine(sessionDir, "launcher.log"), line + Environment.NewLine, Encoding.UTF8);
        }
        public void Complete(string outcome)
        {
            completed = true;
            string json = new JavaScriptSerializer().Serialize(new {
                outcome = outcome, session = sessionDir, gamePid = game == null ? 0 : game.Id,
                expectedDlcs = config.ExpectedDlcs, events = events.ToArray()
            });
            File.WriteAllText(Path.Combine(baseDir, probe ? "last-probe.json" : "last-run.json"), json, Encoding.UTF8);
        }
        public void Dispose()
        {
            if (!completed) { try { Complete("stopped"); } catch { } }
            if (game != null) game.Dispose();
        }
    }
}
