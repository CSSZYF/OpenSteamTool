using System;

namespace DaveLauncher
{
    // Compile with DaveReadiness.cs and /main:DaveLauncher.DaveReadinessTests.
    // These fixtures never read live game files or start Steam or the game.
    public static class DaveReadinessTests
    {
        public static int Main(string[] args)
        {
            try
            {
                int checks = Run();
                Console.WriteLine(checks + " readiness checks passed.");
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("Readiness test failed: " + error.Message);
                return 1;
            }
        }

        private static int Run()
        {
            uint[] ids = new uint[] { 2677020, 2841140 };
            string build = "Build Version : v1.0.6.2113.steam\n";
            string dlcs =
                "[DLC localcheck] bundleAppId:2677020, isDlcInstalled:True\n" +
                "ItemIntegrityCheck:PreCheckLocalDLC(UInt32)\n\n" +
                "[DLC localcheck] bundleAppId:2841140, isDlcInstalled:True\n" +
                "ItemIntegrityCheck:PreCheckLocalDLC(UInt32)\n\n";
            string offline =
                "Res:k_EResultNoConnection\n" +
                "UnityEngine.DebugLogHandler:Internal_Log(LogType, LogOption, String, Object)\n" +
                "ItemIntegrityCheck:OnEncryptedAppTicketResponse(EncryptedAppTicketResponse_t, Boolean)\n\n";
            string title =
                "---hasPrevData:True---loadFailed:False\n" +
                "UnityEngine.DebugLogHandler:Internal_Log(LogType, LogOption, String, Object)\n" +
                "DR.Title.<Start>d__38:MoveNext()\n\n";
            string music =
                "[Addressables][Steam] Load Request : BasicLoadAssetAsync(BGM/BGM_Title)\n" +
                "UnityEngine.DebugLogHandler:Internal_Log(LogType, LogOption, String, Object)\n" +
                "SoundManager:PlayMusic(SoundPlayInfo, Boolean)\n" +
                "DR.Title.<Start>d__38:MoveNext()\n\n";
            string complete = build + dlcs + offline + title + music;
            int count = 0;

            CheckReady(complete, ids, "complete initialization", ref count);
            CheckWaiting(build + dlcs + offline + title, ids,
                "title music request has not arrived", ref count);
            CheckWaiting(build + offline + dlcs + title + music, ids,
                "offline callback precedes complete local checks", ref count);
            CheckWaiting(build + dlcs + offline + title +
                music.Replace("DR.Title.<Start>d__38:MoveNext()", "Other:MoveNext()"), ids,
                "title music request lacks its title stack", ref count);
            CheckWaiting(complete + "Build Version : v1.0.7.9999.steam\n" + title + music, ids,
                "new build cannot reuse old DLC or offline evidence", ref count);
            CheckFailed(complete + "[Haptic][OnApplicationQuit][GET CONTROLLER HANDLER]:257", ids,
                "shutdown invalidates a complete sequence", ref count);
            CheckFailed(complete + "OnResult: Rescode=3003", ids,
                "result 3003 invalidates a complete sequence", ref count);
            CheckFailed(complete.Replace("2677020, isDlcInstalled:True",
                "2677020, isDlcInstalled:False"), ids,
                "expected local DLC check failed", ref count);
            CheckWaiting(complete.Replace("ItemIntegrityCheck:OnEncryptedAppTicketResponse",
                "Unrelated:OnEncryptedAppTicketResponse"), ids,
                "NoConnection belongs to a different callback", ref count);
            CheckFailed(complete, new uint[0],
                "explicit expected DLC list is required", ref count);
            CheckWaiting(complete.Replace(build, ""), ids,
                "session marker is required", ref count);
            CheckWaiting(build + dlcs + offline + title +
                music.Replace("SoundManager:PlayMusic", "\nSoundManager:PlayMusic"), ids,
                "stack evidence cannot cross an entry boundary", ref count);
            CheckWaiting(build + dlcs + offline + title +
                music.Substring(0, music.IndexOf("DR.Title.<Start>", StringComparison.Ordinal)), ids,
                "partially written music entry is insufficient", ref count);
            CheckReady(complete.Replace("v1.0.6.2113.steam", "v1.0.6.2114.steam"), ids,
                "new build with the same complete structure", ref count);
            string observedMusic =
                "[Addressables][Steam] Load Request : BasicLoadAssetAsync(BGM/BGM_Title)\n" +
                "UnityEngine.DebugLogHandler:Internal_Log(LogType, LogOption, String, Object)\n" +
                "UnityEngine.Logger:Log(LogType, Object)\n" +
                "UnityEngine.Debug:Log(Object)\n" +
                "AddressableAssetsLoader:BasicLoadAssetAsync(Object)\n" +
                "<GetAddressableAudioClip>d__126:MoveNext()\n" +
                "UnityEngine.SetupCoroutine:InvokeMoveNext(IEnumerator, IntPtr)\n" +
                "SoundManager:PlayMusic(SoundPlayInfo, Boolean, Single, Single, Transform, SameClipPlayType, Action`1)\n" +
                "SoundPlayInfoExtension:PlayMusic(SoundPlayInfo, Action`1)\n" +
                "DR.Title.<Start>d__38:MoveNext()\n" +
                "UnityEngine.SetupCoroutine:InvokeMoveNext(IEnumerator, IntPtr)\n\n";
            CheckReady(build + dlcs + offline + title + observedMusic, ids,
                "observed title music stack with compiler-generated async frame", ref count);

            return count;
        }

        private static void CheckReady(string text, uint[] ids, string name, ref int count)
        {
            ReadinessResult result = DaveReadiness.Analyze(text, ids);
            Check(result.Ready && !result.Failed, result, name, ref count);
        }

        private static void CheckWaiting(string text, uint[] ids, string name, ref int count)
        {
            ReadinessResult result = DaveReadiness.Analyze(text, ids);
            Check(!result.Ready && !result.Failed, result, name, ref count);
        }

        private static void CheckFailed(string text, uint[] ids, string name, ref int count)
        {
            ReadinessResult result = DaveReadiness.Analyze(text, ids);
            Check(!result.Ready && result.Failed, result, name, ref count);
        }

        private static void Check(bool condition, ReadinessResult result, string name, ref int count)
        {
            if (!condition || String.IsNullOrEmpty(result.Reason))
                throw new InvalidOperationException(name + ": " + result.Reason);
            count++;
        }
    }
}
