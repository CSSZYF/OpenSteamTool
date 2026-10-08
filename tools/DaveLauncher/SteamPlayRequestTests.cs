using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;

namespace DaveLauncher
{
    internal static class SteamPlayRequestTests
    {
        private const string Game = @"D:\Steam Library\steamapps\common\DAVE THE DIVER\DaveTheDiver.exe";
        private const string Log = @"D:\diagnostics\with space\Dave.Player.log";

        private static string Encode(string text) { return Convert.ToBase64String(Encoding.UTF8.GetBytes(text)); }
        private static string[] Request(string command)
        {
            return new[] { "--steam-play", "1868140", "812", Encode(Game), Encode(command), Encode(@"D:\Steam Library") };
        }

        private static SteamPlayRequest Parse(params string[] args)
        {
            var all = new List<string> { Game };
            all.AddRange(args);
            return SteamPlayRequest.Parse(Request(SteamPlayRequest.JoinArguments(all)));
        }

        private static string[] LaunchArguments(SteamPlayRequest request)
        {
            var parsed = new List<string>(SteamPlayRequest.SplitCommandLine("game.exe " + request.BuildArguments(Log)));
            parsed.RemoveAt(0);
            return parsed.ToArray();
        }

        private static void Equal(string[] expected, string[] actual)
        {
            if (expected.Length != actual.Length) throw new Exception("Argument count changed.");
            for (int i = 0; i < expected.Length; i++)
                if (expected[i] != actual[i]) throw new Exception("Argument contents changed at " + i + ".");
        }

        private static void Reject(Action action)
        {
            try { action(); }
            catch (InvalidOperationException error) {
                if (error.Message.Contains(Game) || error.Message.Contains("sensitive-value"))
                    throw new Exception("Invalid input leaked into diagnostic.");
                return;
            }
            throw new Exception("Expected a rejected request.");
        }

        private static void MinimalLaunch()
        {
            SteamPlayRequest parsed = Parse("-offline");
            if (parsed.SteamProcessId != 812 || parsed.Executable != Game) throw new Exception("Identity changed.");
            Equal(new[] { "-logFile", Log }, LaunchArguments(parsed));
        }

        private static void PreserveArgumentContents()
        {
            string[] values = { "", "two words", "中文 路径", "a\"b", "trailing\\", "C:\\some path\\", "-offline=no", "prefix-offline", "line\twithtab" };
            var args = new List<string>(values); args.Insert(2, "-offline");
            var expected = new List<string>(values); expected.Add("-logFile"); expected.Add(Log);
            Equal(expected.ToArray(), LaunchArguments(Parse(args.ToArray())));
        }

        private static void ReplaceLogOptions()
        {
            Equal(new[] { "-screen-fullscreen", "0", "-logFile", Log },
                LaunchArguments(Parse("-logFile", "old log", "-offline", "-screen-fullscreen", "0", "-LOGFILE=other log")));
        }

        private static void RemoveOnlyExactOfflineTokens()
        {
            Equal(new[] { "--offline", "-offline=false", "before-offline", "-logFile", Log },
                LaunchArguments(Parse("-OFFLINE", "--offline", "-offline", "-offline=false", "before-offline")));
            Reject(delegate { Parse("-offline=false"); });
        }

        private static void LogValueIsNotAnOfflineSwitch()
        {
            Reject(delegate { Parse("-logFile", "-offline"); });
        }

        private static void RejectInvalidIdentity()
        {
            string[] args = Request(SteamPlayRequest.QuoteArgument(Game) + " -offline");
            args[1] = "480";
            Reject(delegate { SteamPlayRequest.Parse(args); });
            args[1] = "1868140";
            foreach (string value in new[] { "0", "-1", "+812", " 812", "99999999999999", "sensitive-value" }) {
                args[2] = value;
                Reject(delegate { SteamPlayRequest.Parse(args); });
            }
            Reject(delegate { SteamPlayRequest.Parse(new[] { "--steam-play" }); });
        }

        private static void RejectCorruptEncoding()
        {
            foreach (int index in new[] { 3, 4, 5 }) {
                string[] args = Request(SteamPlayRequest.QuoteArgument(Game) + " -offline");
                args[index] = "bad:sensitive-value";
                Reject(delegate { SteamPlayRequest.Parse(args); });
                args[index] = Convert.ToBase64String(new byte[] { 0xc3, 0x28 });
                Reject(delegate { SteamPlayRequest.Parse(args); });
                args[index] = Encode("sensitive-value\0tail");
                Reject(delegate { SteamPlayRequest.Parse(args); });
            }
        }

        private static void RejectEmptyCommandAndExe()
        {
            foreach (int index in new[] { 3, 4 }) {
                string[] args = Request(SteamPlayRequest.QuoteArgument(Game) + " -offline");
                args[index] = "";
                Reject(delegate { SteamPlayRequest.Parse(args); });
            }
        }

        private static void RejectDifferentExecutable()
        {
            Reject(delegate { SteamPlayRequest.Parse(Request("other.exe -offline")); });
            Reject(delegate { SteamPlayRequest.Parse(Request("C:\\elsewhere\\DaveTheDiver.exe -offline")); });
            Reject(delegate { SteamPlayRequest.Parse(Request("..\\DaveTheDiver.exe -offline")); });
            Reject(delegate { Parse("-offline").ValidateInstalledExecutable(@"C:\another\DaveTheDiver.exe"); });
        }

        private static void AllowMatchingBasename()
        {
            Equal(new[] { "-logFile", Log }, LaunchArguments(SteamPlayRequest.Parse(Request("davethediver.EXE -offline"))));
        }

        private static void RejectRelativePaths()
        {
            foreach (string path in new[] { "DaveTheDiver.exe", @"D:DaveTheDiver.exe", @"\DaveTheDiver.exe", @"\\?\D:\DaveTheDiver.exe" }) {
                string[] args = Request(SteamPlayRequest.QuoteArgument(Game) + " -offline");
                args[3] = Encode(path);
                Reject(delegate { SteamPlayRequest.Parse(args); });
                args[3] = Encode(Game); args[5] = Encode(path);
                Reject(delegate { SteamPlayRequest.Parse(args); });
            }
        }

        private static void EmptyWorkingDirectoryUsesInstalledDirectory()
        {
            string executable = Path.Combine(Path.GetTempPath(), "DaveTheDiver.exe");
            string[] args = Request(SteamPlayRequest.QuoteArgument(executable) + " -offline");
            args[3] = Encode(executable); args[5] = "";
            SteamPlayRequest parsed = SteamPlayRequest.Parse(args);
            parsed.ValidateInstalledExecutable(executable);
            if (parsed.WorkingDirectory != Path.GetDirectoryName(executable)) throw new Exception("Missing working-directory fallback.");
        }

        private static void QuotingRoundTrips()
        {
            var values = new List<string> { "program.exe" };
            for (int slashCount = 0; slashCount < 7; slashCount++) {
                string slashes = new string('\\', slashCount);
                values.Add(slashes); values.Add("a " + slashes); values.Add("a" + slashes + "\"b");
            }
            values.Add("汉字 \"quoted\" "); values.Add("");
            Equal(values.ToArray(), SteamPlayRequest.SplitCommandLine(SteamPlayRequest.JoinArguments(values)));
        }

        private static void RejectOversizedInput()
        {
            string[] args = Request(SteamPlayRequest.QuoteArgument(Game) + " -offline");
            args[4] = Encode(new string('x', 32768));
            Reject(delegate { SteamPlayRequest.Parse(args); });
            args[4] = new string('A', 32767 * 4 + 1);
            Reject(delegate { SteamPlayRequest.Parse(args); });
        }

        private static void RecoveryCanAcquireReleasedNamedMutex()
        {
            string name = @"Local\OpenSteamTool_LauncherMutexTest_" + Guid.NewGuid().ToString("N");
            using (var original = new Mutex(false, name)) {
                if (!original.WaitOne(0)) throw new Exception("Cannot own fresh launch mutex.");
                original.ReleaseMutex();
                // The launcher remains alive with this handle during the game.
                // --online must acquire it even though the named object exists.
                bool acquired = false;
                Exception failure = null;
                var recovery = new Thread(delegate() {
                    try {
                        using (var existing = new Mutex(false, name)) {
                            acquired = existing.WaitOne(0);
                            if (acquired) existing.ReleaseMutex();
                        }
                    }
                    catch (Exception error) { failure = error; }
                });
                recovery.Start();
                if (!recovery.Join(5000)) throw new Exception("Recovery lock test did not finish.");
                if (failure != null) throw failure;
                if (!acquired) throw new Exception("Released launch mutex blocked recovery.");
            }
        }

        public static int Main()
        {
            Action[] tests = { MinimalLaunch, PreserveArgumentContents, ReplaceLogOptions, RemoveOnlyExactOfflineTokens,
                LogValueIsNotAnOfflineSwitch, RejectInvalidIdentity, RejectCorruptEncoding, RejectEmptyCommandAndExe,
                RejectDifferentExecutable, AllowMatchingBasename, RejectRelativePaths, EmptyWorkingDirectoryUsesInstalledDirectory,
                QuotingRoundTrips, RejectOversizedInput, RecoveryCanAcquireReleasedNamedMutex };
            foreach (Action test in tests) {
                try { test(); Console.WriteLine("PASS " + test.Method.Name); }
                catch (Exception error) { Console.Error.WriteLine("FAIL " + test.Method.Name + ": " + error.Message); return 1; }
            }
            Console.WriteLine("SteamPlayRequest: " + tests.Length + " tests passed.");
            return 0;
        }
    }
}
