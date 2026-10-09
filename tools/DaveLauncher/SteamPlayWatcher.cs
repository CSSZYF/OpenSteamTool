using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace DaveLauncher
{
    // The watcher installs the gate before Steam marks the game as running.
    // It never changes Steam mode itself and exits with the verified client.
    internal sealed class SteamPlayWatcher
    {
        private readonly string directory;
        private readonly NativeSteamControl control = new NativeSteamControl("http://127.0.0.1:8080/");
        private string lastStatus;
        private int warningVisible;
        private string lastErrorEvent;

        public SteamPlayWatcher(string directory) { this.directory = directory; }

        public Task<int> RunAsync(int steamPid)
        {
            // Mutex ownership is thread-affine. The owner waits synchronously on
            // the asynchronous polling task, then releases on this same thread.
            return Task.Run(delegate { return RunOwned(steamPid); });
        }

        private int RunOwned(int steamPid)
        {
            Mutex mutex = null;
            bool ownsMutex = false;
            try {
                if (steamPid <= 0) throw new InvalidOperationException();
                mutex = new Mutex(false, @"Local\OST_DavePlayWatch_" + steamPid.ToString(CultureInfo.InvariantCulture));
                try { ownsMutex = mutex.WaitOne(0); }
                catch (AbandonedMutexException) { ownsMutex = true; }
                if (!ownsMutex) return 0;
                using (Process client = OpenVerifiedClient(steamPid)) {
                    DateTime started = client.StartTime.ToUniversalTime();
                    string script = LoadScript();
                    using (var lifetime = new CancellationTokenSource()) {
                        EventHandler exited = delegate {
                            try { lifetime.Cancel(); }
                            catch (ObjectDisposedException) { }
                        };
                        client.Exited += exited;
                        client.EnableRaisingEvents = true;
                        try {
                            if (client.HasExited) return 0;
                            WriteStatus("watching");
                            PollAsync(client, started, script, lifetime.Token).GetAwaiter().GetResult();
                        }
                        finally { client.Exited -= exited; }
                    }
                }
                WriteStatus("steam-exited");
                return 0;
            }
            catch (OperationCanceledException) { WriteStatus("steam-exited"); return 0; }
            catch (Exception) { WriteStatus("watcher-unavailable"); return 1; }
            finally {
                if (mutex != null) {
                    if (ownsMutex) mutex.ReleaseMutex();
                    mutex.Dispose();
                }
            }
        }

        private static Process OpenVerifiedClient(int steamPid)
        {
            string steamRoot;
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                steamRoot = key == null ? null : key.GetValue("SteamPath") as string;
            if (String.IsNullOrWhiteSpace(steamRoot)) throw new InvalidOperationException();
            string expected = Path.Combine(Path.GetFullPath(steamRoot), "steam.exe");
            Process client = Process.GetProcessById(steamPid);
            try {
                if (client.HasExited || !String.Equals(Path.GetFileName(client.MainModule.FileName), "steam.exe", StringComparison.OrdinalIgnoreCase) ||
                    !String.Equals(Path.GetFullPath(client.MainModule.FileName), expected, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException();
                return client;
            }
            catch { client.Dispose(); throw; }
        }

        private string LoadScript()
        {
            string scriptPath = Path.Combine(directory, "SteamPlayGate.js");
            using (var stream = new FileStream(scriptPath, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                if (stream.Length == 0 || stream.Length > 128 * 1024) throw new InvalidOperationException();
                using (var reader = new StreamReader(stream, new UTF8Encoding(false, true), true))
                    return reader.ReadToEnd();
            }
        }

        private async Task PollAsync(Process client, DateTime started, string script, CancellationToken token)
        {
            while (!token.IsCancellationRequested) {
                if (client.HasExited || client.StartTime.ToUniversalTime() != started) return;
                try {
                    bool noRunningApps = NoRunningApps();
                    Dictionary<string, object> status = await control.InstallPlayGateAsync(script, noRunningApps, token).ConfigureAwait(false);
                    HandleStatus(status);
                }
                catch (OperationCanceledException) { if (token.IsCancellationRequested) return; }
                catch (Exception) { WriteStatus("waiting-for-steam-ui"); }
                await Task.Delay(2000, token).ConfigureAwait(false);
            }
        }

        private static bool NoRunningApps()
        {
            try {
                Process[] games = Process.GetProcessesByName("DaveTheDiver");
                try { if (games.Length != 0) return false; }
                finally { foreach (Process game in games) game.Dispose(); }
                using (RegistryKey apps = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\Apps")) {
                    if (apps == null) return false;
                    foreach (string name in apps.GetSubKeyNames()) {
                        using (RegistryKey app = apps.OpenSubKey(name)) {
                            if (app == null) return false;
                            object running = app.GetValue("Running", 0);
                            if (!(running is int) && !(running is long)) return false;
                            if (Convert.ToInt64(running, CultureInfo.InvariantCulture) != 0) return false;
                        }
                    }
                }
                return true;
            }
            catch (Exception) { return false; }
        }

        private void HandleStatus(Dictionary<string, object> status)
        {
            object installed;
            if (!status.TryGetValue("installed", out installed) || !(installed is bool) || !(bool)installed) {
                WriteStatus("waiting-for-steam-ui");
                return;
            }
            object errorValue;
            string error = status.TryGetValue("error", out errorValue) ? errorValue as string : null;
            if (String.IsNullOrEmpty(error)) {
                object busy;
                WriteStatus(status.TryGetValue("busy", out busy) && busy is bool && (bool)busy ? "preparing-offline" : "ready");
                return;
            }
            WriteStatus(error == "apps_running_or_unknown" ? "apps-running-or-unknown" : "launch-gate-failed");
            object eventValue;
            string errorEvent = status.TryGetValue("eventId", out eventValue) && eventValue is int
                ? ((int)eventValue).ToString(CultureInfo.InvariantCulture) : "gate-error";
            if (errorEvent == lastErrorEvent) return;
            lastErrorEvent = errorEvent;
            ShowGateFailure(error);
        }

        private void ShowGateFailure(string error)
        {
            if (Interlocked.CompareExchange(ref warningVisible, 1, 0) != 0) return;
            string message = error == "apps_running_or_unknown"
                ? "Steam 中仍有应用运行，或暂时无法确认运行状态。\n\n请退出其他 Steam 应用和戴夫，等待几秒后再点击开始游戏。此次未切换离线模式。"
                : "Steam 开始游戏的自动离线准备失败，游戏未由此流程启动。\n\n请关闭此提示后重试。需要恢复 Steam 在线时运行戴夫启动器的 --online。";
            // A notification must not keep the Steam watcher alive after Steam exits.
            var notification = new Thread(delegate() {
                try {
                    MessageBox.Show(message, "戴夫启动器", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                finally { Interlocked.Exchange(ref warningVisible, 0); }
            });
            notification.IsBackground = true;
            notification.SetApartmentState(ApartmentState.STA);
            notification.Start();
        }

        private void WriteStatus(string status)
        {
            if (String.Equals(lastStatus, status, StringComparison.Ordinal)) return;
            lastStatus = status;
            try {
                string logPath = Path.Combine(directory, "play-watch.log");
                if (File.Exists(logPath) && new FileInfo(logPath).Length > 128 * 1024)
                    File.WriteAllText(logPath, "", Encoding.UTF8);
                File.AppendAllText(logPath, DateTimeOffset.Now.ToString("o") + " " + status + Environment.NewLine, Encoding.UTF8);
            }
            catch (Exception) { /* A full log directory cannot stop the launch gate. */ }
        }
    }
}
