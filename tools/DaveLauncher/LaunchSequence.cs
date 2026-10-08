using System;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Threading.Tasks;

namespace DaveLauncher
{
    public interface ILaunchHost
    {
        Task PrepareAsync(CancellationToken cancellationToken);
        Task<SteamState> ReadSteamAsync(CancellationToken cancellationToken);
        Task GoOfflineAsync(CancellationToken cancellationToken);
        Task WaitForOfflineAsync(CancellationToken cancellationToken);
        Task StartGameAsync(CancellationToken cancellationToken);
        Task WaitForGameReadyAsync(CancellationToken cancellationToken);
        Task GoOnlineAsync(CancellationToken cancellationToken);
        Task WaitForOnlineAsync(CancellationToken cancellationToken);
        bool IsGameRunning();
        void Report(string message);
    }

    public sealed class AutoLaunchSequence
    {
        public async Task RunAsync(ILaunchHost host, CancellationToken cancellationToken)
        {
            if (host == null) throw new ArgumentNullException("host");

            bool originallyOnline = false;
            bool offlineAttempted = false;
            bool gameRequested = false;
            ExceptionDispatchInfo failure = null;
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                await host.PrepareAsync(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                SteamState state = await host.ReadSteamAsync(cancellationToken).ConfigureAwait(false);
                if (state == null || !state.ServicesReady)
                    throw new InvalidOperationException("Steam 服务尚未就绪，未切换模式或启动游戏。");

                originallyOnline = !state.Offline;
                cancellationToken.ThrowIfCancellationRequested();
                if (originallyOnline)
                {
                    // The native call can change state and then fail to reply.
                    offlineAttempted = true;
                    await host.GoOfflineAsync(cancellationToken).ConfigureAwait(false);
                }
                cancellationToken.ThrowIfCancellationRequested();
                await host.WaitForOfflineAsync(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                gameRequested = true;
                await host.StartGameAsync(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                await host.WaitForGameReadyAsync(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                await host.GoOnlineAsync(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                await host.WaitForOnlineAsync(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                SafeReport(host, "游戏初始化已确认完成，Steam 已恢复在线连接。");
            }
            catch (Exception error)
            {
                failure = ExceptionDispatchInfo.Capture(error);
            }

            if (failure == null) return;

            // C# 5 does not permit await in catch. Recovery runs here so the
            // original exception and its stack survive any recovery failure.
            if (originallyOnline && offlineAttempted)
            {
                bool running = true;
                try
                {
                    running = host.IsGameRunning();
                }
                catch (Exception)
                {
                    SafeReport(host, "无法确认游戏是否仍在运行，已停止自动切换模式。需要恢复时可运行 --online。");
                }

                if (!running)
                {
                    try
                    {
                        // Host implementations must impose their own timeout;
                        // user cancellation must not cancel this recovery.
                        await host.GoOnlineAsync(CancellationToken.None).ConfigureAwait(false);
                        await host.WaitForOnlineAsync(CancellationToken.None).ConfigureAwait(false);
                        SafeReport(host, "游戏未在运行，Steam 已恢复原来的在线状态。");
                    }
                    catch (Exception)
                    {
                        SafeReport(host, "自动恢复在线失败；原始启动错误已保留，需要恢复时可运行 --online。");
                    }
                }
                else if (gameRequested)
                {
                    SafeReport(host, "游戏仍在运行或运行状态无法确认，已停止自动切换模式。需要恢复时可运行 --online。");
                }
            }

            failure.Throw();
        }

        private static void SafeReport(ILaunchHost host, string message)
        {
            try { host.Report(message); }
            catch (Exception) { /* Reporting cannot hide the operation result. */ }
        }
    }
}
