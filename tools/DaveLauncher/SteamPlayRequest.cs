using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DaveLauncher
{
    // The Steam hook forwards the original launch without interpreting its arguments.
    // Executable validation against the installed manifest happens before any mode change.
    internal sealed class SteamPlayRequest
    {
        private const string InvalidRequest = "Steam 开始游戏请求无效，未启动游戏。";
        private const int MaximumTextLength = 32767;
        public int SteamProcessId { get; private set; }
        public string Executable { get; private set; }
        public string WorkingDirectory { get; private set; }
        private string[] arguments;

        [DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr CommandLineToArgvW(string commandLine, out int count);
        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);

        public static SteamPlayRequest Parse(string[] args)
        {
            int steamPid;
            if (args == null || args.Length != 6 || args[0] != "--steam-play" || args[1] != "1868140" ||
                !Int32.TryParse(args[2], NumberStyles.None, CultureInfo.InvariantCulture, out steamPid) || steamPid <= 0)
                throw new InvalidOperationException(InvalidRequest);

            string executable = Decode(args[3], false);
            string command = Decode(args[4], false);
            string workingDirectory = Decode(args[5], true);
            try {
                if (!IsAbsolutePath(executable) ||
                    (workingDirectory.Length != 0 && !IsAbsolutePath(workingDirectory)))
                    throw new InvalidOperationException(InvalidRequest);
                executable = Path.GetFullPath(executable);
                if (workingDirectory.Length != 0) workingDirectory = Path.GetFullPath(workingDirectory);
                string[] parsed = SplitCommandLine(command);
                if (parsed.Length == 0 || !MatchesExecutable(parsed[0], executable))
                    throw new InvalidOperationException(InvalidRequest);
                var kept = new List<string>();
                bool hasOfflineSwitch = false;
                for (int i = 1; i < parsed.Length; i++) {
                    string argument = parsed[i];
                    if (String.Equals(argument, "-offline", StringComparison.OrdinalIgnoreCase)) {
                        hasOfflineSwitch = true;
                        continue;
                    }
                    if (String.Equals(argument, "-logFile", StringComparison.OrdinalIgnoreCase)) {
                        // Unity consumes the following argument as its log file path.
                        if (i + 1 < parsed.Length) i++;
                        continue;
                    }
                    if (argument.StartsWith("-logFile=", StringComparison.OrdinalIgnoreCase)) continue;
                    kept.Add(argument);
                }
                if (!hasOfflineSwitch) throw new InvalidOperationException(InvalidRequest);
                return new SteamPlayRequest {
                    SteamProcessId = steamPid, Executable = executable,
                    WorkingDirectory = workingDirectory, arguments = kept.ToArray()
                };
            }
            catch (ArgumentException) { throw new InvalidOperationException(InvalidRequest); }
            catch (NotSupportedException) { throw new InvalidOperationException(InvalidRequest); }
            catch (PathTooLongException) { throw new InvalidOperationException(InvalidRequest); }
        }

        private static string Decode(string encoded, bool allowEmpty)
        {
            if (encoded == null || encoded.Length > MaximumTextLength * 4)
                throw new InvalidOperationException(InvalidRequest);
            try {
                string value = new UTF8Encoding(false, true).GetString(Convert.FromBase64String(encoded));
                if ((!allowEmpty && value.Length == 0) || value.Length > MaximumTextLength || value.IndexOf('\0') >= 0)
                    throw new InvalidOperationException(InvalidRequest);
                return value;
            }
            catch (FormatException) { throw new InvalidOperationException(InvalidRequest); }
            catch (DecoderFallbackException) { throw new InvalidOperationException(InvalidRequest); }
        }

        private static bool IsAbsolutePath(string path)
        {
            // Rooted drive-relative paths (C:game.exe) and \game.exe are not absolute.
            return path.Length >= 3 && Char.IsLetter(path[0]) && path[1] == ':' &&
                (path[2] == '\\' || path[2] == '/') ||
                path.StartsWith(@"\\", StringComparison.Ordinal) && !path.StartsWith(@"\\?\", StringComparison.Ordinal) &&
                !path.StartsWith(@"\\.\", StringComparison.Ordinal);
        }

        private static bool MatchesExecutable(string argument, string executable)
        {
            if (String.Equals(argument, Path.GetFileName(executable), StringComparison.OrdinalIgnoreCase)) return true;
            return IsAbsolutePath(argument) && String.Equals(Path.GetFullPath(argument), executable, StringComparison.OrdinalIgnoreCase);
        }

        public void ValidateInstalledExecutable(string installedExecutable)
        {
            if (!String.Equals(Executable, Path.GetFullPath(installedExecutable), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Steam 启动目标与戴夫安装清单不一致，未切换离线或启动游戏。");
            if (WorkingDirectory.Length == 0) WorkingDirectory = Path.GetDirectoryName(Executable);
            if (!Directory.Exists(WorkingDirectory))
                throw new InvalidOperationException("Steam 传入的游戏工作目录不存在，未切换离线或启动游戏。");
        }

        public string BuildArguments(string logFile)
        {
            var result = new List<string>(arguments);
            result.Add("-logFile");
            result.Add(logFile);
            return JoinArguments(result);
        }

        internal static string[] SplitCommandLine(string commandLine)
        {
            if (String.IsNullOrWhiteSpace(commandLine)) throw new InvalidOperationException(InvalidRequest);
            int count;
            IntPtr memory = CommandLineToArgvW(commandLine, out count);
            if (memory == IntPtr.Zero) throw new InvalidOperationException(InvalidRequest);
            try {
                string[] result = new string[count];
                for (int i = 0; i < count; i++) result[i] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(memory, i * IntPtr.Size));
                return result;
            }
            finally { LocalFree(memory); }
        }

        internal static string JoinArguments(IEnumerable<string> arguments)
        {
            var result = new StringBuilder();
            foreach (string argument in arguments) {
                if (result.Length != 0) result.Append(' ');
                result.Append(QuoteArgument(argument));
            }
            return result.ToString();
        }

        internal static string QuoteArgument(string argument)
        {
            if (argument == null) throw new ArgumentNullException("argument");
            // Always quote so empty strings, whitespace and trailing backslashes survive.
            var result = new StringBuilder("\"");
            int slashes = 0;
            foreach (char value in argument) {
                if (value == '\\') { slashes++; continue; }
                if (value == '"') result.Append('\\', slashes * 2 + 1);
                else result.Append('\\', slashes);
                result.Append(value);
                slashes = 0;
            }
            result.Append('\\', slashes * 2);
            result.Append('"');
            return result.ToString();
        }
    }
}
