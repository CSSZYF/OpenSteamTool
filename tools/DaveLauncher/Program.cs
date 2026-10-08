using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace DaveLauncher
{
    internal sealed class LauncherConfig
    {
        public uint[] ExpectedDlcs { get; set; }
        public int ReadyTimeoutSeconds { get; set; }
        public int TitleSettleSeconds { get; set; }

        public static LauncherConfig Load(string directory)
        {
            string path = Path.Combine(directory, "DaveLauncher.json");
            var config = new LauncherConfig {
                ExpectedDlcs = new uint[] {2677020, 2841140, 3543180, 4158580, 4394810},
                ReadyTimeoutSeconds = 180,
                TitleSettleSeconds = 10
            };
            if (File.Exists(path)) {
                if (new FileInfo(path).Length > 16384) throw new InvalidOperationException("启动器配置过大。");
                config = new JavaScriptSerializer().Deserialize<LauncherConfig>(File.ReadAllText(path));
            }
            if (config == null || config.ExpectedDlcs == null || config.ExpectedDlcs.Length == 0 ||
                config.ExpectedDlcs.Length > 64 || config.ExpectedDlcs.Any(x => x == 0) ||
                config.ReadyTimeoutSeconds < 30 || config.ReadyTimeoutSeconds > 600 ||
                config.TitleSettleSeconds < 3 || config.TitleSettleSeconds > 30)
                throw new InvalidOperationException("DaveLauncher.json 配置无效。");
            return config;
        }
    }

    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            string directory = AppDomain.CurrentDomain.BaseDirectory;
            string mode = args.Length == 0 ? "--launch" : args[0];
            bool probe = mode == "--status" || mode == "--preview";
            try {
                if (args.Length > 1 || !new[] {"--launch", "--online", "--status", "--preview"}.Contains(mode))
                    throw new InvalidOperationException("支持参数：--launch、--online、--status、--preview。");
                bool created;
                using (var mutex = new Mutex(true, @"Local\OpenSteamTool_DaveNativeLauncher", out created)) {
                    if (!created) throw new InvalidOperationException("戴夫启动流程已经在运行，请勿重复启动。");
                    using (var host = new WindowsLaunchHost(directory, LauncherConfig.Load(directory), probe)) {
                        if (mode == "--preview") host.Preview();
                        else if (mode == "--status") {
                            SteamState state = host.ReadSteamAsync(CancellationToken.None).GetAwaiter().GetResult();
                            host.Report("Steam offline=" + state.Offline + " serverConnected=" + state.Connected + " servicesReady=" + state.ServicesReady);
                        }
                        else if (mode == "--online") {
                            host.GoOnlineAsync(CancellationToken.None).GetAwaiter().GetResult();
                            host.WaitForOnlineAsync(CancellationToken.None).GetAwaiter().GetResult();
                            host.Report("Steam 已恢复在线连接。");
                        }
                        else new AutoLaunchSequence().RunAsync(host, CancellationToken.None).GetAwaiter().GetResult();
                        host.Complete("success");
                    }
                }
                return 0;
            }
            catch (Exception error) {
                // Report only our own diagnostic message, never native UI objects or tickets.
                string message = error is IOException || error is UnauthorizedAccessException
                    ? "读取或写入本地启动日志失败，请检查目录权限。" : error.Message;
                try { File.WriteAllText(Path.Combine(directory, "last-error.txt"), DateTimeOffset.Now.ToString("o") + "\n" + message, Encoding.UTF8); } catch { }
                if (!probe) MessageBox.Show(message + "\n\n流程已停止。需要恢复 Steam 在线时运行“恢复 Steam 在线”。",
                    "戴夫启动器", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return 1;
            }
        }
    }
}
