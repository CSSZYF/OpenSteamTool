using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace DaveLauncher
{
    public static class LaunchSequenceTests
    {
        private sealed class FakeHost : ILaunchHost
        {
            public readonly List<string> Calls = new List<string>();
            public readonly List<string> Reports = new List<string>();
            public readonly Dictionary<string, Exception> Failures = new Dictionary<string, Exception>();
            public SteamState InitialState = new SteamState { ServicesReady = true, Connected = true };
            public bool GameRunning;
            public bool ExitBeforeReadyFailure;
            public bool ThrowWhenCheckingGame;
            public bool FailReports;
            public Action<string> OnCall;
            public readonly List<CancellationToken> OnlineTokens = new List<CancellationToken>();

            private Task Call(string name, CancellationToken token)
            {
                Calls.Add(name);
                if (OnCall != null) OnCall(name);
                if (name == "GoOnline" || name == "WaitOnline") OnlineTokens.Add(token);
                if (name == "WaitReady" && ExitBeforeReadyFailure) GameRunning = false;
                Exception error;
                if (Failures.TryGetValue(name, out error))
                {
                    TaskCompletionSource<bool> failed = new TaskCompletionSource<bool>();
                    failed.SetException(error);
                    return failed.Task;
                }
                if (name == "StartGame") GameRunning = true;
                return Task.FromResult(true);
            }

            public Task PrepareAsync(CancellationToken token) { return Call("Prepare", token); }
            public async Task<SteamState> ReadSteamAsync(CancellationToken token)
            {
                await Call("ReadSteam", token);
                return InitialState;
            }
            public Task GoOfflineAsync(CancellationToken token) { return Call("GoOffline", token); }
            public Task WaitForOfflineAsync(CancellationToken token) { return Call("WaitOffline", token); }
            public Task StartGameAsync(CancellationToken token) { return Call("StartGame", token); }
            public Task WaitForGameReadyAsync(CancellationToken token) { return Call("WaitReady", token); }
            public Task GoOnlineAsync(CancellationToken token) { return Call("GoOnline", token); }
            public Task WaitForOnlineAsync(CancellationToken token) { return Call("WaitOnline", token); }
            public bool IsGameRunning()
            {
                Calls.Add("IsGameRunning");
                if (ThrowWhenCheckingGame) throw new InvalidOperationException("process query failed");
                return GameRunning;
            }
            public void Report(string message)
            {
                if (FailReports) throw new InvalidOperationException("report failed");
                Reports.Add(message);
            }
        }

        private static void Assert(bool condition, string message)
        {
            if (!condition) throw new Exception(message);
        }

        private static void Sequence(FakeHost host, string expected)
        {
            string actual = string.Join(",", host.Calls);
            Assert(actual == expected, "Expected " + expected + "; got " + actual);
        }

        private static Exception Run(FakeHost host, CancellationToken token)
        {
            try { new AutoLaunchSequence().RunAsync(host, token).GetAwaiter().GetResult(); }
            catch (Exception error) { return error; }
            return null;
        }

        private static Exception Run(FakeHost host) { return Run(host, CancellationToken.None); }

        private static void NoSuccess(FakeHost host)
        {
            Assert(!host.Reports.Exists(delegate(string text) { return text.Contains("游戏初始化已确认完成"); }),
                   "Failure must not report successful launch");
        }

        private static void HappyPath()
        {
            FakeHost host = new FakeHost();
            Assert(Run(host) == null, "Expected successful launch");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,WaitReady,GoOnline,WaitOnline");
            Assert(host.Reports.Count == 1, "Expected one completion report");
        }

        private static void AlreadyOffline()
        {
            FakeHost host = new FakeHost();
            host.InitialState.Offline = true;
            host.InitialState.Connected = false;
            Assert(Run(host) == null, "Initially offline launch should succeed");
            Sequence(host, "Prepare,ReadSteam,WaitOffline,StartGame,WaitReady,GoOnline,WaitOnline");
        }

        private static void PrepareAndReadFailureDoNotChangeState()
        {
            foreach (string stage in new[] { "Prepare", "ReadSteam" })
            {
                FakeHost host = new FakeHost();
                Exception original = new InvalidOperationException(stage);
                host.Failures.Add(stage, original);
                Assert(object.ReferenceEquals(Run(host), original), "Original exception must survive");
                Sequence(host, stage == "Prepare" ? "Prepare" : "Prepare,ReadSteam");
                NoSuccess(host);
            }
        }

        private static void ServicesMustBeReady()
        {
            FakeHost host = new FakeHost();
            host.InitialState.ServicesReady = false;
            Assert(Run(host) is InvalidOperationException, "Services must be ready");
            Sequence(host, "Prepare,ReadSteam");
            host = new FakeHost();
            host.InitialState = null;
            Assert(Run(host) is InvalidOperationException, "Missing state must be refused");
            Sequence(host, "Prepare,ReadSteam");
        }

        private static void OfflineFailureRestoresWithoutStarting()
        {
            foreach (string stage in new[] { "GoOffline", "WaitOffline" })
            {
                FakeHost host = new FakeHost();
                Exception original = new TimeoutException(stage);
                host.Failures.Add(stage, original);
                Assert(object.ReferenceEquals(Run(host), original), "Offline error must survive recovery");
                Sequence(host, "Prepare,ReadSteam,GoOffline," +
                    (stage == "WaitOffline" ? "WaitOffline," : "") + "IsGameRunning,GoOnline,WaitOnline");
                Assert(host.OnlineTokens.TrueForAll(delegate(CancellationToken t) { return !t.CanBeCanceled; }),
                       "Recovery must use an independent token");
                NoSuccess(host);
            }
        }

        private static void StartFailureBeforeProcessRestores()
        {
            FakeHost host = new FakeHost();
            Exception original = new InvalidOperationException("start failed");
            host.Failures.Add("StartGame", original);
            Assert(object.ReferenceEquals(Run(host), original), "Start error must survive");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,IsGameRunning,GoOnline,WaitOnline");
            NoSuccess(host);
        }

        private static void StartFailureAfterProcessAppearsDoesNotRestore()
        {
            FakeHost host = new FakeHost();
            host.Failures.Add("StartGame", new InvalidOperationException("reply lost"));
            host.OnCall = delegate(string name) { if (name == "StartGame") host.GameRunning = true; };
            Assert(Run(host) != null, "Expected start failure");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,IsGameRunning");
            NoSuccess(host);
        }

        private static void ReadyTimeoutWithActiveGameDoesNotRestore()
        {
            FakeHost host = new FakeHost();
            Exception original = new TimeoutException("ready timeout");
            host.Failures.Add("WaitReady", original);
            Assert(object.ReferenceEquals(Run(host), original), "Readiness error must survive");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,WaitReady,IsGameRunning");
            Assert(host.GameRunning, "Test must represent an active game");
            NoSuccess(host);
        }

        private static void ReadyFailureAfterExitRestores()
        {
            FakeHost host = new FakeHost();
            host.ExitBeforeReadyFailure = true;
            host.Failures.Add("WaitReady", new InvalidOperationException("game exited"));
            Assert(Run(host) != null, "Expected readiness failure");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,WaitReady,IsGameRunning,GoOnline,WaitOnline");
            NoSuccess(host);
        }

        private static void InitiallyOfflineFailureKeepsOriginalMode()
        {
            FakeHost host = new FakeHost();
            host.InitialState.Offline = true;
            host.Failures.Add("StartGame", new InvalidOperationException("start failed"));
            Assert(Run(host) != null, "Expected start failure");
            Sequence(host, "Prepare,ReadSteam,WaitOffline,StartGame");
            NoSuccess(host);
        }

        private static void OnlineWaitFailureCannotReportSuccess()
        {
            FakeHost host = new FakeHost();
            Exception original = new TimeoutException("still not connected");
            host.Failures.Add("WaitOnline", original);
            Assert(object.ReferenceEquals(Run(host), original), "Online wait error must survive");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,WaitReady,GoOnline,WaitOnline,IsGameRunning");
            NoSuccess(host);
        }

        private static void RecoveryFailurePreservesOriginal()
        {
            FakeHost host = new FakeHost();
            Exception original = new InvalidOperationException("first failure");
            host.Failures.Add("StartGame", original);
            host.Failures.Add("GoOnline", new TimeoutException("recovery failed"));
            Assert(object.ReferenceEquals(Run(host), original), "Recovery must not replace original error");
            Assert(host.Reports.Exists(delegate(string text) { return text.Contains("自动恢复在线失败"); }),
                   "Recovery failure must be reported");
            NoSuccess(host);
        }

        private static void UnknownGameStateDoesNotRestore()
        {
            FakeHost host = new FakeHost();
            Exception original = new TimeoutException("ready timeout");
            host.Failures.Add("WaitReady", original);
            host.ThrowWhenCheckingGame = true;
            Assert(object.ReferenceEquals(Run(host), original), "Process query must not replace error");
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,StartGame,WaitReady,IsGameRunning");
            NoSuccess(host);
        }

        private static void CancellationBeforePrepareDoesNothing()
        {
            FakeHost host = new FakeHost();
            using (CancellationTokenSource cancel = new CancellationTokenSource())
            {
                cancel.Cancel();
                Assert(Run(host, cancel.Token) is OperationCanceledException, "Expected cancellation");
            }
            Sequence(host, "");
        }

        private static void CancellationAfterOfflineRecoversWithIndependentToken()
        {
            FakeHost host = new FakeHost();
            using (CancellationTokenSource cancel = new CancellationTokenSource())
            {
                host.OnCall = delegate(string name) { if (name == "WaitOffline") cancel.Cancel(); };
                Assert(Run(host, cancel.Token) is OperationCanceledException, "Expected cancellation");
            }
            Sequence(host, "Prepare,ReadSteam,GoOffline,WaitOffline,IsGameRunning,GoOnline,WaitOnline");
            Assert(host.OnlineTokens.TrueForAll(delegate(CancellationToken t) { return !t.CanBeCanceled; }),
                   "Recovery cannot inherit cancelled token");
            NoSuccess(host);
        }

        private static void ReportingFailureDoesNotReplaceResult()
        {
            FakeHost host = new FakeHost();
            host.FailReports = true;
            Assert(Run(host) == null, "Successful launch must survive failed logging");
            host = new FakeHost();
            host.FailReports = true;
            Exception original = new InvalidOperationException("start failed");
            host.Failures.Add("StartGame", original);
            Assert(object.ReferenceEquals(Run(host), original), "Logging must not replace original error");
        }

        public static int Main()
        {
            Action[] tests =
            {
                HappyPath, AlreadyOffline, PrepareAndReadFailureDoNotChangeState,
                ServicesMustBeReady, OfflineFailureRestoresWithoutStarting,
                StartFailureBeforeProcessRestores, StartFailureAfterProcessAppearsDoesNotRestore,
                ReadyTimeoutWithActiveGameDoesNotRestore, ReadyFailureAfterExitRestores,
                InitiallyOfflineFailureKeepsOriginalMode, OnlineWaitFailureCannotReportSuccess,
                RecoveryFailurePreservesOriginal, UnknownGameStateDoesNotRestore,
                CancellationBeforePrepareDoesNothing, CancellationAfterOfflineRecoversWithIndependentToken,
                ReportingFailureDoesNotReplaceResult
            };
            int failed = 0;
            foreach (Action test in tests)
            {
                try { test(); Console.WriteLine("PASS " + test.Method.Name); }
                catch (Exception error)
                {
                    ++failed;
                    Console.WriteLine("FAIL " + test.Method.Name + ": " + error.Message);
                }
            }
            Console.WriteLine("Launch sequence: " + (tests.Length - failed) + "/" + tests.Length + " passed.");
            return failed == 0 ? 0 : 1;
        }
    }
}
