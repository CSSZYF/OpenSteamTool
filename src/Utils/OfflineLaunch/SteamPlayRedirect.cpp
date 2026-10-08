#include "SteamPlayRedirect.h"

#include "dllmain.h"

#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace SteamPlayRedirect {
namespace {

constexpr AppId_t kDaveAppId = 1868140;
constexpr size_t kMaximumInputBytes = 12000;
constexpr size_t kMaximumHelperCommandBytes = 28000;
constexpr size_t kMaximumRetainedLaunches = 64;

struct RedirectArguments {
    std::string executable;
    std::string commandLine;
    std::string workingDirectory;
};

// The original SpawnProcess consumes these pointers after the trap callback
// returns. Its return ABI is intentionally not assumed, so retain a bounded
// collection for this DLL's lifetime instead of freeing it on callback return.
std::mutex g_argumentsMutex;
std::vector<std::shared_ptr<RedirectArguments>> g_arguments;

bool ReadBounded(const char* source, std::string& result) {
    if (!source) return false;
    const size_t length = strnlen_s(source, kMaximumInputBytes + 1);
    if (length > kMaximumInputBytes) return false;
    result.assign(source, length);
    return true;
}

bool StrictWide(std::string_view text, std::wstring& result) {
    result.clear();
    if (text.empty()) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return false;
    result.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), result.data(), count) == count;
}

bool HasOfflineArgument(const std::wstring& commandLine) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(commandLine.c_str(), &count);
    if (!arguments) return false;
    bool found = false;
    // CUser_SpawnProcess receives a complete command line including argv[0].
    for (int index = 1; index < count; ++index) {
        if (_wcsicmp(arguments[index], L"-offline") == 0) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

std::string Base64(std::string_view text) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((text.size() + 2) / 3) * 4);
    for (size_t index = 0; index < text.size(); index += 3) {
        const size_t remaining = text.size() - index;
        const auto first = static_cast<unsigned char>(text[index]);
        const auto second = remaining > 1 ? static_cast<unsigned char>(text[index + 1]) : 0;
        const auto third = remaining > 2 ? static_cast<unsigned char>(text[index + 2]) : 0;
        encoded.push_back(alphabet[first >> 2]);
        encoded.push_back(alphabet[((first & 3) << 4) | (second >> 4)]);
        encoded.push_back(remaining > 1 ? alphabet[((second & 15) << 2) | (third >> 6)] : '=');
        encoded.push_back(remaining > 2 ? alphabet[third & 63] : '=');
    }
    return encoded;
}

void DeclineUnrecognized(const char* reason) {
    // Never intercept an unvalidated or unrelated request based on a substring.
    LOG_MISC_WARN("SteamPlayOffline: cannot validate request ({}); original Steam launch remains unchanged", reason);
}

void BlockRecognized(OSTPlatform::Trap::Context& context, const char* reason) {
    // A question mark cannot be a Windows drive letter, so this path cannot
    // name a runnable file, even if someone creates a similarly named helper.
    // Keep the real SpawnProcess call and let its normal error handling run.
    static constexpr char unavailableExecutable[] =
        "?:\\OpenSteamTool-offline-launch-blocked.exe";
    static constexpr char unavailableCommand[] =
        "\"?:\\OpenSteamTool-offline-launch-blocked.exe\"";
    const uint64_t oldExecutable = context.Argument(2);
    const uint64_t oldCommand = context.Argument(3);
    const bool redirected =
        context.SetRegisterArgument(2, reinterpret_cast<uint64_t>(unavailableExecutable)) &&
        context.SetRegisterArgument(3, reinterpret_cast<uint64_t>(unavailableCommand));
    if (!redirected) {
        context.SetRegisterArgument(2, oldExecutable);
        context.SetRegisterArgument(3, oldCommand);
    }
    LOG_MISC_WARN("SteamPlayOffline: requested offline launch could not be prepared ({}); {}",
                  reason, redirected ? "game creation blocked through Steam's normal launch error" :
                                       "native registers unavailable; launch was not changed");
}

} // namespace

void TryRedirect(OSTPlatform::Trap::Context& context, AppId_t appId,
                 const char* executable, const char* commandLine) {
    if (appId != kDaveAppId) return;
    bool recognized = false;
    try {
        std::string originalExecutable;
        std::string originalCommand;
        std::wstring wideExecutable;
        std::wstring wideCommand;
        if (!ReadBounded(executable, originalExecutable) ||
            !ReadBounded(commandLine, originalCommand) ||
            !StrictWide(originalExecutable, wideExecutable) ||
            !StrictWide(originalCommand, wideCommand)) {
            DeclineUnrecognized("invalid UTF-8 or oversized game launch input");
            return;
        }
        if (!HasOfflineArgument(wideCommand)) return;
        const std::filesystem::path gamePath(wideExecutable);
        if (!gamePath.is_absolute() ||
            _wcsicmp(gamePath.filename().c_str(), L"DaveTheDiver.exe") != 0) {
            return;
        }
        recognized = true;
        // Simultaneous app-id remapping is not a supported offline workflow.
        // The helper checks its inherited Steam environment independently too.
        if (context.Argument(5) == 0) {
            BlockRecognized(context, "missing game identity");
            return;
        }
        const auto* gameId = reinterpret_cast<const CGameID*>(context.Argument(5));
        if (gameId->AppID(true) != kDaveAppId) {
            BlockRecognized(context, "game identity was remapped by another launch mode");
            return;
        }

        const auto* workingDirectory = reinterpret_cast<const char*>(context.Argument(4));
        std::string originalWorkingDirectory;
        std::wstring wideWorkingDirectory;
        std::wstring wideDllDirectory;
        if (!ReadBounded(workingDirectory, originalWorkingDirectory) ||
            originalWorkingDirectory.empty() ||
            !StrictWide(originalWorkingDirectory, wideWorkingDirectory) ||
            !StrictWide(DllDir, wideDllDirectory) || wideDllDirectory.empty() ||
            !std::filesystem::path(wideWorkingDirectory).is_absolute()) {
            BlockRecognized(context, "invalid game or DLL working directory");
            return;
        }
        const auto helperDirectory = std::filesystem::path(wideDllDirectory) / L"dave-launcher";
        const auto helperPath = helperDirectory / L"DaveLauncher.exe";
        std::error_code error;
        if (!helperPath.is_absolute() || !std::filesystem::is_regular_file(helperPath, error) || error) {
            BlockRecognized(context, "dave-launcher/DaveLauncher.exe is not installed beside OpenSteamTool.dll");
            return;
        }

        auto replacement = std::make_shared<RedirectArguments>();
        replacement->executable = OSTPlatform::Encoding::PathToUtf8(helperPath);
        replacement->workingDirectory = OSTPlatform::Encoding::PathToUtf8(helperDirectory);
        replacement->commandLine = "\"" + replacement->executable + "\" --steam-play " +
            std::to_string(kDaveAppId) + " " + std::to_string(GetCurrentProcessId()) + " " +
            Base64(originalExecutable) + " " + Base64(originalCommand) + " " +
            Base64(originalWorkingDirectory);
        if (replacement->executable.empty() || replacement->workingDirectory.empty() ||
            replacement->commandLine.size() > kMaximumHelperCommandBytes) {
            BlockRecognized(context, "helper command exceeds its size limit");
            return;
        }
        {
            std::scoped_lock lock(g_argumentsMutex);
            if (g_arguments.size() >= kMaximumRetainedLaunches) {
                BlockRecognized(context, "launch retention limit reached; restart Steam before using -offline again");
                return;
            }
            g_arguments.push_back(replacement);
        }

        const uint64_t oldExecutable = context.Argument(2);
        const uint64_t oldCommand = context.Argument(3);
        const uint64_t oldWorkingDirectory = context.Argument(4);
        if (!context.SetRegisterArgument(2, reinterpret_cast<uint64_t>(replacement->executable.c_str())) ||
            !context.SetRegisterArgument(3, reinterpret_cast<uint64_t>(replacement->commandLine.c_str())) ||
            !context.SetRegisterArgument(4, reinterpret_cast<uint64_t>(replacement->workingDirectory.c_str()))) {
            context.SetRegisterArgument(2, oldExecutable);
            context.SetRegisterArgument(3, oldCommand);
            context.SetRegisterArgument(4, oldWorkingDirectory);
            BlockRecognized(context, "the native argument registers were unavailable");
            return;
        }
        LOG_MISC_INFO("SteamPlayOffline: redirected app={} to the native offline helper; Steam's original process creation will continue", appId);
    }
    catch (const std::exception&) {
        if (recognized) BlockRecognized(context, "could not prepare the offline helper request");
        else DeclineUnrecognized("could not validate the offline helper request");
    }
    catch (...) {
        if (recognized) BlockRecognized(context, "unexpected offline helper preparation error");
        else DeclineUnrecognized("unexpected offline helper validation error");
    }
}

} // namespace SteamPlayRedirect
