// Standalone Win32 tests. Compile this file instead of SteamPlayRedirect.cpp,
// link OSTPlatform/Encoding.cpp, shell32 and crypt32, and define
// OST_STEAMPLAY_REDIRECT_TEST. No Steam or game process is started.
#ifndef OST_STEAMPLAY_REDIRECT_TEST
#error Define OST_STEAMPLAY_REDIRECT_TEST for this standalone test executable.
#endif

#include "SteamPlayRedirect.cpp"

#include <wincrypt.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace OSTPlatform::Trap {
uint64_t Context::Argument(int index) const {
    if (!nativeContext_ || index < 1 || index > 5) return 0;
    return (*static_cast<std::array<uint64_t, 6>*>(nativeContext_))[index];
}
bool Context::SetRegisterArgument(int index, uint64_t value) {
    if (!nativeContext_ || index < 1 || index > 4) return false;
    (*static_cast<std::array<uint64_t, 6>*>(nativeContext_))[index] = value;
    return true;
}
} // namespace OSTPlatform::Trap

namespace {
int checks = 0;
void Check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    ++checks;
}

std::string Decode(const std::wstring& encoded) {
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()),
            CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr))
        throw std::runtime_error("Windows Base64 size decode failed");
    std::string text(bytes, '\0');
    if (!CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()),
            CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(text.data()), &bytes, nullptr, nullptr))
        throw std::runtime_error("Windows Base64 decode failed");
    text.resize(bytes);
    return text;
}

struct Launch {
    std::string executable = "D:\\Games\\DaveTheDiver.exe";
    std::string command = "\"D:\\Games\\DaveTheDiver.exe\" -offline";
    std::string workingDirectory = "D:\\Games";
    CGameID gameId{};
    std::array<uint64_t, 6> registers{};
    OSTPlatform::Trap::Context context{&registers};
    Launch() { gameId.SetAppID(1868140); Reset(); }
    void Reset() {
        registers[2] = reinterpret_cast<uint64_t>(executable.c_str());
        registers[3] = reinterpret_cast<uint64_t>(command.c_str());
        registers[4] = reinterpret_cast<uint64_t>(workingDirectory.c_str());
        registers[5] = reinterpret_cast<uint64_t>(&gameId);
    }
    void Redirect(AppId_t app = 1868140) {
        Reset();
        SteamPlayRedirect::TryRedirect(context, app, executable.c_str(), command.c_str());
    }
    const char* Exe() const { return reinterpret_cast<const char*>(registers[2]); }
    const char* Command() const { return reinterpret_cast<const char*>(registers[3]); }
    bool Unchanged() const {
        return Exe() == executable.c_str() && Command() == command.c_str() &&
               registers[4] == reinterpret_cast<uint64_t>(workingDirectory.c_str());
    }
    bool Blocked() const { return std::string_view(Exe()).starts_with("?:\\"); }
};

void TestPureHelpers() {
    using namespace SteamPlayRedirect;
    Check(!HasOfflineArgument(L"\"D:\\-offline\\DaveTheDiver.exe\""), "argv zero is not a flag");
    Check(!HasOfflineArgument(L"DaveTheDiver.exe --offline"), "double-dash is distinct");
    Check(!HasOfflineArgument(L"DaveTheDiver.exe -offline=no"), "equals-suffix is not a flag");
    Check(!HasOfflineArgument(L"DaveTheDiver.exe \"notes -offline\""), "quoted argument substring is not a flag");
    Check(!HasOfflineArgument(L"DaveTheDiver.exe -offline-extra"), "hyphen-suffix is not a flag");
    Check(!HasOfflineArgument(L"DaveTheDiver.exe -logFile -offline"), "log path token is not an offline switch");
    Check(!HasOfflineArgument(L"DaveTheDiver.exe -logFile=-offline"), "equals log path is not an offline switch");
    Check(HasOfflineArgument(L"DaveTheDiver.exe -logFile -offline -offline"), "real offline switch following log path recognized");
    Check(HasOfflineArgument(L"DaveTheDiver.exe -offline"), "exact flag recognized");
    Check(HasOfflineArgument(L"DaveTheDiver.exe \"-OFFLINE\""), "quoted case-insensitive flag recognized");
    Check(HasOfflineArgument(L"DaveTheDiver.exe\t-offline\t-windowed"), "tab separated flag recognized");
    std::wstring converted;
    Check(!StrictWide(std::string("\xc3\x28", 2), converted), "invalid UTF-8 rejected");
    Check(StrictWide("", converted) && converted.empty(), "empty UTF-8 accepted");
    const std::pair<const char*, const char*> vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}
    };
    for (const auto& vector : vectors) Check(Base64(vector.first) == vector.second, "RFC 4648 Base64 vector");
    const std::string original = "\"D:\\游戏 目录\\DaveTheDiver.exe\" -offline -name \"a \\\"quote\\\"\"";
    const std::string encoded = Base64(original);
    Check(StrictWide(encoded, converted), "encoded payload is valid UTF-8");
    Check(Decode(converted) == original, "Windows independently decodes exact UTF-8 bytes and quoting");
}

void TestRedirects(const std::filesystem::path& fixture) {
    std::filesystem::create_directories(fixture / "dave-launcher");
    const auto helper = fixture / "dave-launcher" / "DaveLauncher.exe";
    { std::ofstream marker(helper); marker << "test fixture; not executable"; }
    const std::string utf8Fixture = OSTPlatform::Encoding::PathToUtf8(fixture);
    Check(utf8Fixture.size() < kRuntimePathCapacity, "fixture fits DLL directory storage");
    std::memcpy(DllDir, utf8Fixture.c_str(), utf8Fixture.size() + 1);

    Launch normal;
    normal.command = "\"D:\\Games\\DaveTheDiver.exe\" -windowed";
    normal.Redirect();
    Check(normal.Unchanged(), "normal game launch stays untouched");
    normal.command = "\"D:\\Games\\DaveTheDiver.exe\" -offline=no";
    normal.Redirect();
    Check(normal.Unchanged(), "similar flag stays untouched");
    normal.command = "\"D:\\Games\\DaveTheDiver.exe\" -logFile -offline";
    normal.Redirect();
    Check(normal.Unchanged(), "log file named offline cannot intercept original Play");
    normal.command = "\"D:\\Games\\DaveTheDiver.exe\" -offline";
    normal.Redirect(42);
    Check(normal.Unchanged(), "unrelated app stays untouched");
    normal.executable = "D:\\Games\\OtherGame.exe";
    normal.Redirect();
    Check(normal.Unchanged(), "unrelated executable stays untouched");

    Launch first;
    first.command += " -name \"a \\\"quote\\\"\" -windowed";
    first.Redirect();
    Check(std::string(first.Exe()) == OSTPlatform::Encoding::PathToUtf8(helper), "supported request redirects to helper");
    const char* retainedPointer = first.Command();
    const std::string retainedCommand(retainedPointer);
    std::wstring wide;
    Check(SteamPlayRedirect::StrictWide(retainedCommand, wide), "helper request uses valid UTF-8");
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(wide.c_str(), &count);
    Check(arguments != nullptr && count == 7, "helper receives complete fixed argument contract");
    Check(std::wstring(arguments[1]) == L"--steam-play" && std::wstring(arguments[2]) == L"1868140", "helper receives app and mode");
    Check(std::stoul(arguments[3]) == GetCurrentProcessId(), "helper receives Steam caller PID");
    Check(Decode(arguments[4]) == first.executable, "original executable survives request transport");
    Check(Decode(arguments[5]) == first.command, "entire command with quoted arguments survives request transport");
    Check(Decode(arguments[6]) == first.workingDirectory, "original working directory survives request transport");
    LocalFree(arguments);

    Launch second;
    second.Redirect();
    Check(std::string(retainedPointer) == retainedCommand, "later launch cannot invalidate earlier command pointer");
    Check(!second.context.SetRegisterArgument(5, 123), "stack argument cannot be rewritten");
    Check(second.registers[5] == reinterpret_cast<uint64_t>(&second.gameId), "failed stack mutation preserves identity");

    Launch oversized;
    oversized.command += " " + std::string(11800, 'a');
    oversized.workingDirectory = "D:\\" + std::string(11000, 'a');
    oversized.Redirect();
    Check(oversized.Blocked(), "validated request with oversized helper output cannot launch game online");

    Launch remapped;
    remapped.gameId.SetAppID(480);
    remapped.Redirect();
    Check(remapped.Blocked(), "conflicting app identity cannot launch game online");

    Launch nonApp;
    nonApp.gameId.m_gameID.m_nType = CGameID::k_EGameIDTypeShortcut;
    nonApp.Redirect();
    Check(nonApp.Blocked(), "non-app game identity cannot launch game online");

    std::filesystem::remove(helper);
    Launch missing;
    missing.Redirect();
    Check(missing.Blocked(), "missing helper cannot launch game online");
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string blockedCommand(missing.Command());
    const BOOL created = CreateProcessA(missing.Exe(), blockedCommand.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &startup, &process);
    const DWORD creationError = GetLastError();
    std::cout << "Blocked CreateProcess result=" << created << " error=" << creationError << '\n';
    Check(created == FALSE && (creationError == ERROR_INVALID_NAME ||
        creationError == ERROR_PATH_NOT_FOUND || creationError == ERROR_FILE_NOT_FOUND),
        "Windows rejects blocked executable path without starting a process");

    { std::ofstream marker(helper); marker << "test fixture; not executable"; }
    while (SteamPlayRedirect::g_arguments.size() < SteamPlayRedirect::kMaximumRetainedLaunches) {
        Launch retained;
        retained.Redirect();
        Check(!retained.Blocked() && !retained.Unchanged(), "bounded retention accepts supported launch");
    }
    Launch full;
    full.Redirect();
    Check(full.Blocked(), "full retention pool cannot launch game online");
    Check(std::string(retainedPointer) == retainedCommand, "old pointer remains valid at retention limit");

    std::filesystem::remove(helper);
    std::filesystem::remove(fixture / "dave-launcher");
    std::filesystem::remove(fixture);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Pass a workspace artifact directory for test fixtures.");
        TestPureHelpers();
        const auto fixture = std::filesystem::absolute(OSTPlatform::Encoding::PathFromUtf8(argv[1])) /
            ("steamplay-native-fixture-" + std::to_string(GetCurrentProcessId()));
        TestRedirects(fixture);
        std::cout << "Passed " << checks << " native Steam Play redirect checks.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
