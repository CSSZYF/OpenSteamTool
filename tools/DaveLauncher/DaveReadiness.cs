using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;

namespace DaveLauncher
{
    sealed class ReadinessResult
    {
        public bool Ready;
        public bool Failed;
        public string Reason;
    }

    static class DaveReadiness
    {
        private static readonly Regex BuildMarker = new Regex(
            @"^[ \t]*Build Version\b[^\r\n]*", RegexOptions.Multiline);
        private static readonly Regex BuildValue = new Regex(
            @"^Build Version\s*:\s*(\S+)\s*$");
        private static readonly Regex LocalCheck = new Regex(
            @"^\[DLC localcheck\]\s*bundleAppId\s*:\s*(\d+)\s*,\s*isDlcInstalled\s*:\s*(True|False)\s*$");
        private static readonly Regex OfflineResponse = new Regex(
            @"^Res\s*:\s*k_EResultNoConnection\s*$");
        private static readonly Regex TitleLoad = new Regex(
            @"^---hasPrevData:(True|False)---loadFailed:(True|False)\s*$");
        private static readonly Regex ResultFailure = new Regex(
            @"\bRescode\s*[:=]\s*3003\b|\bOnResult\b[^\r\n]*\b3003\b",
            RegexOptions.IgnoreCase);
        private static readonly Regex StackFrame = new Regex(
            @"^(?:[A-Za-z_<][^\r\n]*:[^\r\n]*\(|\(Filename:)");

        private const string TicketFrame = "ItemIntegrityCheck:OnEncryptedAppTicketResponse(";
        private const string TitleFrame = "DR.Title.<Start>d__38:MoveNext(";
        private const string MusicFrame = "SoundManager:PlayMusic(";
        private const string TitleMusic = "BasicLoadAssetAsync(BGM/BGM_Title)";

        // The caller must bind this text to the newly launched process and log file.
        // This parser proves an observed initialization sequence, not a rendered frame.
        public static ReadinessResult Analyze(string text, IEnumerable<uint> expectedDlcs)
        {
            HashSet<uint> expected = new HashSet<uint>();
            if (expectedDlcs != null)
            {
                foreach (uint appId in expectedDlcs)
                {
                    if (appId == 0)
                        return Result(false, true, "Expected DLC AppID must not be zero.");
                    expected.Add(appId);
                }
            }
            if (expected.Count == 0)
                return Result(false, true, "An explicit, nonempty expected DLC list is required.");
            if (String.IsNullOrEmpty(text))
                return Result(false, false, "Waiting for the current game log.");

            MatchCollection builds = BuildMarker.Matches(text);
            if (builds.Count == 0)
                return Result(false, false, "Waiting for a Build Version session marker.");

            // Never combine evidence from launches or versions concatenated in one log.
            Match latestBuild = builds[builds.Count - 1];
            Match versionValue = BuildValue.Match(latestBuild.Value.Trim());
            if (!versionValue.Success)
                return Result(false, false, "The latest Build Version marker is incomplete or unsupported.");
            string version = versionValue.Groups[1].Value;
            string session = text.Substring(latestBuild.Index);
            string[] lines = Regex.Split(session, "\r\n|\n|\r");
            string prefix = "Build " + version + ": ";

            // A later failure invalidates an earlier successful-looking sequence.
            if (session.IndexOf("OnApplicationQuit", StringComparison.Ordinal) >= 0)
                return Result(false, true, prefix + "game shutdown was observed.");
            if (ResultFailure.IsMatch(session))
                return Result(false, true, prefix + "DLC result 3003 was observed.");

            HashSet<uint> found = new HashSet<uint>();
            int lastLocalCheck = -1;
            for (int i = 0; i < lines.Length; i++)
            {
                Match local = LocalCheck.Match(lines[i].Trim());
                if (!local.Success)
                    continue;
                uint appId;
                if (!UInt32.TryParse(local.Groups[1].Value, NumberStyles.None,
                    CultureInfo.InvariantCulture, out appId) || !expected.Contains(appId))
                    continue;
                if (local.Groups[2].Value == "False")
                    return Result(false, true, prefix + "local DLC check failed for " +
                        appId.ToString(CultureInfo.InvariantCulture) + ".");
                found.Add(appId);
                lastLocalCheck = i;
            }
            if (found.Count != expected.Count)
            {
                List<uint> missing = new List<uint>();
                foreach (uint appId in expected)
                    if (!found.Contains(appId))
                        missing.Add(appId);
                missing.Sort();
                List<string> ids = new List<string>();
                foreach (uint appId in missing)
                    ids.Add(appId.ToString(CultureInfo.InvariantCulture));
                return Result(false, false, prefix + "waiting for successful local DLC checks: " +
                    String.Join(", ", ids.ToArray()) + ".");
            }

            int offlineCallback = -1;
            for (int i = lastLocalCheck + 1; i < lines.Length; i++)
            {
                if (!OfflineResponse.IsMatch(lines[i].Trim()))
                    continue;
                int callback = FindFrame(lines, i, TicketFrame);
                if (callback >= 0)
                {
                    offlineCallback = callback;
                    break;
                }
            }
            if (offlineCallback < 0)
                return Result(false, false, prefix +
                    "waiting for the offline encrypted-ticket callback after all local DLC checks.");

            int titleInitialized = -1;
            for (int i = offlineCallback + 1; i < lines.Length; i++)
            {
                Match title = TitleLoad.Match(lines[i].Trim());
                if (!title.Success)
                    continue;
                int titleFrame = FindFrame(lines, i, TitleFrame);
                if (titleFrame < 0)
                    continue;
                if (title.Groups[2].Value == "True")
                    return Result(false, true, prefix + "title data loading failed.");
                titleInitialized = titleFrame;
                break;
            }
            if (titleInitialized < 0)
                return Result(false, false, prefix +
                    "waiting for successful title data initialization after the offline callback.");

            for (int i = titleInitialized + 1; i < lines.Length; i++)
            {
                string line = lines[i].Trim();
                if (!line.StartsWith("[Addressables][Steam] Load Request", StringComparison.Ordinal) ||
                    line.IndexOf(TitleMusic, StringComparison.Ordinal) < 0)
                    continue;
                if (FindFrame(lines, i, MusicFrame) >= 0 && FindFrame(lines, i, TitleFrame) >= 0)
                {
                    return Result(true, false, prefix + "title initialization observed after all " +
                        expected.Count.ToString(CultureInfo.InvariantCulture) +
                        " expected local DLC checks and the offline ticket callback; " +
                        "the title music request is not proof that a menu frame has rendered.");
                }
            }
            return Result(false, false, prefix +
                "waiting for the title music request with matching music and title stack frames.");
        }

        private static int FindFrame(string[] lines, int messageIndex, string frame)
        {
            // Unity appends stack frames to their message. Stop at the entry boundary
            // so an unrelated later stack cannot complete a partially written entry.
            for (int i = messageIndex + 1; i < lines.Length; i++)
            {
                string line = lines[i].Trim();
                if (line.Length == 0 || !StackFrame.IsMatch(line))
                    return -1;
                if (line.IndexOf(frame, StringComparison.Ordinal) >= 0)
                    return i;
            }
            return -1;
        }

        private static ReadinessResult Result(bool ready, bool failed, string reason)
        {
            return new ReadinessResult { Ready = ready, Failed = failed, Reason = reason };
        }
    }
}
