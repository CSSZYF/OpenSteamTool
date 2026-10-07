#include "TokenStorage.h"
#include "Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>

#pragma comment(lib, "crypt32.lib")

namespace OST::ExtractTickets {

namespace {
    constexpr uint32_t kHeaderMagic = 0x4B54534F; // "OSTK"
    constexpr uint16_t kHeaderVersion = 1;

    // App-specific entropy for DPAPI to prevent generic DPAPI dumping tools
    constexpr uint8_t kAppEntropy[] = {
        0x4F, 0x53, 0x54, 0x2D, 0x53, 0x65, 0x63, 0x75,
        0x72, 0x65, 0x2D, 0x54, 0x6F, 0x6B, 0x65, 0x6E,
        0x2D, 0x45, 0x6E, 0x74, 0x72, 0x6F, 0x70, 0x79,
        0x2D, 0x4B, 0x65, 0x79, 0x32, 0x30, 0x32, 0x36
    };

    std::vector<std::string>& GetQuarantineLines() {
        static std::vector<std::string> s_quarantine;
        return s_quarantine;
    }

    std::string SerializeAccounts(const std::vector<CachedAccount>& accounts) {
        std::string result;
        result.reserve(accounts.size() * 256);
        for (const auto& acc : accounts) {
            std::format_to(std::back_inserter(result), "{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                           acc.accountName, acc.steamId, acc.lastLoginTime,
                           acc.refreshToken, acc.accessToken,
                           acc.isInvalid ? 1 : 0,
                           TokenStorage::SanitizeAlias(acc.alias));
        }
        for (const auto& qLine : GetQuarantineLines()) {
            if (!qLine.empty()) {
                result += qLine;
                result += '\n';
            }
        }
        return result;
    }

    std::vector<CachedAccount> DeserializeAccounts(std::string_view plaintext) {
        std::vector<CachedAccount> result;
        GetQuarantineLines().clear();
        std::istringstream iss(std::string{plaintext});
        std::string line;

        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::istringstream lineStream(line);
            std::vector<std::string> fields;
            std::string token;
            while (std::getline(lineStream, token, '\t')) {
                fields.push_back(token);
            }

            if (fields.size() >= 4) {
                CachedAccount acc;
                acc.accountName = fields[0];
                const std::string& steamIdStr = fields[1];
                const std::string& timeStr = fields[2];
                acc.refreshToken = fields[3];

                if (fields.size() >= 5) {
                    acc.accessToken = fields[4];
                }
                if (fields.size() >= 6) {
                    acc.isInvalid = (fields[5] == "1" || fields[5] == "true");
                }
                if (fields.size() >= 7) {
                    acc.alias = TokenStorage::SanitizeAlias(fields[6]);
                }

                uint64_t sId = 0;
                const auto [p1, ec1] = std::from_chars(steamIdStr.data(), steamIdStr.data() + steamIdStr.size(), sId);
                int64_t loginTime = 0;
                const auto [p2, ec2] = std::from_chars(timeStr.data(), timeStr.data() + timeStr.size(), loginTime);

                if (ec1 == std::errc{} && p1 == steamIdStr.data() + steamIdStr.size() &&
                    ec2 == std::errc{} && p2 == timeStr.data() + timeStr.size() &&
                    !acc.accountName.empty() && !acc.refreshToken.empty()) {
                    acc.steamId = sId;
                    acc.lastLoginTime = loginTime;
                    result.push_back(std::move(acc));
                } else {
                    LOG_WARN("TokenStorage", "暂存解析异常的凭据条目至隔离区，避免覆盖丢失");
                    GetQuarantineLines().push_back(line);
                }
            } else {
                LOG_WARN("TokenStorage", "跳过格式严重缺损的凭据条目 (列数={})", fields.size());
                GetQuarantineLines().push_back(line);
            }
        }
        return result;
    }

    void SecureWipeFile(const std::filesystem::path& p) {
        std::error_code ec;
        auto fileSize = std::filesystem::file_size(p, ec);
        if (!ec && fileSize > 0) {
            std::ofstream ofs(p, std::ios::binary | std::ios::in | std::ios::out);
            if (ofs.is_open()) {
                constexpr std::array<char, 1024> zeros{};
                size_t remaining = fileSize;
                while (remaining > 0) {
                    size_t toWrite = (std::min)(remaining, zeros.size());
                    ofs.write(zeros.data(), toWrite);
                    remaining -= toWrite;
                }
                ofs.flush();
                ofs.close();
            }
        }
        std::filesystem::remove(p, ec);
    }
} // namespace

std::filesystem::path TokenStorage::GetStorageFilePath() {
    wchar_t localAppData[32768]{0};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (len == 0 || len >= std::size(localAppData)) {
        PWSTR pPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pPath)) && pPath) {
            std::filesystem::path dir = std::filesystem::path(pPath) / L"OpenSteamTool" / L"credentials";
            CoTaskMemFree(pPath);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir / L"accounts.enc";
        }
        return L"accounts.enc";
    }

    std::filesystem::path dir = std::filesystem::path(localAppData) / L"OpenSteamTool" / L"credentials";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"accounts.enc";
}

std::vector<CachedAccount> TokenStorage::LoadAccounts() {
    const auto filePath = GetStorageFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        return {};
    }

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        LOG_WARN("TokenStorage", "无法打开凭据缓存文件: {}", MaskPath(filePath.string()));
        return {};
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t reserved = 0;
    uint32_t payloadSize = 0;

    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    ifs.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
    ifs.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));

    if (!ifs || magic != kHeaderMagic || version != kHeaderVersion || payloadSize == 0 || payloadSize > 10 * 1024 * 1024) {
        LOG_WARN("TokenStorage", "凭据文件头部校验失败或文件已损坏");
        return {};
    }

    std::vector<uint8_t> encryptedBlob(payloadSize);
    ifs.read(reinterpret_cast<char*>(encryptedBlob.data()), payloadSize);
    if (ifs.gcount() != static_cast<std::streamsize>(payloadSize)) {
        LOG_WARN("TokenStorage", "凭据密文读取不完整");
        return {};
    }

    DATA_BLOB inBlob;
    inBlob.pbData = encryptedBlob.data();
    inBlob.cbData = static_cast<DWORD>(encryptedBlob.size());

    DATA_BLOB entropyBlob;
    entropyBlob.pbData = const_cast<BYTE*>(kAppEntropy);
    entropyBlob.cbData = sizeof(kAppEntropy);

    DATA_BLOB outBlob = {0, nullptr};
    if (!CryptUnprotectData(&inBlob, nullptr, &entropyBlob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        LOG_ERROR("TokenStorage", "DPAPI 解密失败 (GetLastError={})", GetLastError());
        return {};
    }

    std::string plaintext(reinterpret_cast<char*>(outBlob.pbData), outBlob.cbData);
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);

    auto accounts = DeserializeAccounts(plaintext);
    SecureZeroMemory(plaintext.data(), plaintext.size());

    LOG_DEBUG("TokenStorage", "成功加载 {} 个缓存账号", accounts.size());
    return accounts;
}

bool TokenStorage::SaveAccounts(const std::vector<CachedAccount>& accounts) {
    const auto filePath = GetStorageFilePath();

    if (accounts.empty() && GetQuarantineLines().empty()) {
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec)) {
            SecureWipeFile(filePath);
        }
        return true;
    }

    std::string plaintext = SerializeAccounts(accounts);

    DATA_BLOB inBlob;
    inBlob.pbData = reinterpret_cast<BYTE*>(plaintext.data());
    inBlob.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB entropyBlob;
    entropyBlob.pbData = const_cast<BYTE*>(kAppEntropy);
    entropyBlob.cbData = sizeof(kAppEntropy);

    DATA_BLOB outBlob = {0, nullptr};
    if (!CryptProtectData(&inBlob, L"OST_Steam_Tokens", &entropyBlob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        LOG_ERROR("TokenStorage", "DPAPI 加密保存失败 (GetLastError={})", GetLastError());
        SecureZeroMemory(plaintext.data(), plaintext.size());
        return false;
    }

    SecureZeroMemory(plaintext.data(), plaintext.size());

    const auto tempPath = std::filesystem::path(filePath.wstring() + L".tmp");

    {
        std::ofstream ofs(tempPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            LOG_ERROR("TokenStorage", "无法创建临时凭据文件: {}", MaskPath(tempPath.string()));
            LocalFree(outBlob.pbData);
            return false;
        }

        const uint32_t magic = kHeaderMagic;
        const uint16_t version = kHeaderVersion;
        const uint16_t reserved = 0;
        const uint32_t payloadSize = outBlob.cbData;

        ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
        ofs.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
        ofs.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
        ofs.write(reinterpret_cast<const char*>(outBlob.pbData), payloadSize);
        ofs.flush();
        ofs.close();
    }

    LocalFree(outBlob.pbData);

    if (!ReplaceFileW(filePath.c_str(), tempPath.c_str(), nullptr, 0, nullptr, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            if (!MoveFileExW(tempPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                LOG_ERROR("TokenStorage", "首次建立凭据文件失败 (MoveFileExW GetLastError={})", GetLastError());
                std::error_code ec;
                std::filesystem::remove(tempPath, ec);
                return false;
            }
        } else {
            LOG_ERROR("TokenStorage", "原子替换凭据文件失败 (ReplaceFileW GetLastError={})", err);
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    LOG_DEBUG("TokenStorage", "成功原子持久化 {} 个账号到 DPAPI 存储", accounts.size());
    return true;
}

bool TokenStorage::UpsertAccount(const CachedAccount& account) {
    auto accounts = LoadAccounts();
    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return (account.steamId != 0 && a.steamId == account.steamId) ||
               (!account.accountName.empty() && a.accountName == account.accountName);
    });

    if (it != accounts.end()) {
        it->accountName = account.accountName;
        if (account.steamId != 0) it->steamId = account.steamId;
        if (!account.refreshToken.empty()) it->refreshToken = account.refreshToken;
        if (!account.accessToken.empty()) it->accessToken = account.accessToken;
        if (account.lastLoginTime > 0) it->lastLoginTime = account.lastLoginTime;
        // Merge policy for alias:
        // Ordinary logins pass empty alias, so NEVER overwrite an existing user-configured alias!
        if (!account.alias.empty()) {
            it->alias = SanitizeAlias(account.alias);
        }
        it->isInvalid = account.isInvalid;
    } else {
        CachedAccount sanitized = account;
        sanitized.alias = SanitizeAlias(account.alias);
        accounts.push_back(sanitized);
    }

    return SaveAccounts(accounts);
}

bool TokenStorage::UpdateAccountAlias(uint64_t steamId, std::string_view newAlias) {
    auto accounts = LoadAccounts();
    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.steamId == steamId;
    });

    if (it == accounts.end()) {
        return false;
    }

    it->alias = SanitizeAlias(newAlias);
    return SaveAccounts(accounts);
}

bool TokenStorage::MarkAccountInvalid(uint64_t steamId, bool isInvalid) {
    auto accounts = LoadAccounts();
    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.steamId == steamId;
    });

    if (it == accounts.end()) {
        return false;
    }

    if (it->isInvalid == isInvalid) {
        return true;
    }

    it->isInvalid = isInvalid;
    return SaveAccounts(accounts);
}

std::string TokenStorage::SanitizeAlias(std::string_view raw) {
    if (raw.empty()) return {};

    std::string clean;
    clean.reserve(raw.size());
    for (char c : raw) {
        if (static_cast<unsigned char>(c) < 32 || c == 127 || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        clean.push_back(c);
    }

    size_t start = 0;
    while (start < clean.size() && clean[start] == ' ') {
        ++start;
    }
    size_t end = clean.size();
    while (end > start && clean[end - 1] == ' ') {
        --end;
    }
    if (start >= end) {
        return {};
    }

    std::string trimmed = clean.substr(start, end - start);

    if (trimmed.size() > 64) {
        trimmed.resize(64);
        while (!trimmed.empty()) {
            unsigned char last = static_cast<unsigned char>(trimmed.back());
            if ((last & 0x80) == 0) {
                break;
            }
            if ((last & 0xC0) == 0xC0) {
                trimmed.pop_back();
                break;
            }
            size_t seqLen = 0;
            size_t idx = trimmed.size() - 1;
            while (idx > 0 && (static_cast<unsigned char>(trimmed[idx]) & 0xC0) == 0x80) {
                --idx;
                ++seqLen;
            }
            unsigned char lead = static_cast<unsigned char>(trimmed[idx]);
            size_t needed = 0;
            if ((lead & 0xE0) == 0xC0) needed = 1;
            else if ((lead & 0xF0) == 0xE0) needed = 2;
            else if ((lead & 0xF8) == 0xF0) needed = 3;
            if (seqLen >= needed) {
                break;
            } else {
                trimmed.erase(idx);
                break;
            }
        }
    }

    return trimmed;
}

bool TokenStorage::DeleteAccount(uint64_t steamId) {
    if (steamId == 0) return false;
    auto accounts = LoadAccounts();
    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.steamId == steamId;
    });

    if (it == accounts.end()) {
        return false;
    }

    if (!it->refreshToken.empty()) {
        SecureZeroMemory(it->refreshToken.data(), it->refreshToken.size());
    }
    if (!it->accessToken.empty()) {
        SecureZeroMemory(it->accessToken.data(), it->accessToken.size());
    }
    accounts.erase(it);

    LOG_INFO("TokenStorage", "已安全删除 SteamID {} 的本地凭据缓存", steamId);
    return SaveAccounts(accounts);
}

bool TokenStorage::DeleteAccount(std::string_view accountName) {
    auto accounts = LoadAccounts();
    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.accountName == accountName;
    });

    if (it == accounts.end()) {
        return false;
    }

    // Zero out memory of tokens before removal
    if (!it->refreshToken.empty()) {
        SecureZeroMemory(it->refreshToken.data(), it->refreshToken.size());
    }
    if (!it->accessToken.empty()) {
        SecureZeroMemory(it->accessToken.data(), it->accessToken.size());
    }
    accounts.erase(it);

    LOG_INFO("TokenStorage", "已安全删除账号 {} 的本地凭据缓存", MaskAccount(accountName));
    return SaveAccounts(accounts);
}

bool TokenStorage::WipeAll() {
    const auto filePath = GetStorageFilePath();
    std::error_code ec;
    if (std::filesystem::exists(filePath, ec)) {
        SecureWipeFile(filePath);
        LOG_INFO("TokenStorage", "已彻底粉碎并清理全部本地凭据缓存文件");
    }
    const auto tempPath = std::filesystem::path(filePath.wstring() + L".tmp");
    if (std::filesystem::exists(tempPath, ec)) {
        SecureWipeFile(tempPath);
    }
    GetQuarantineLines().clear();
    return true;
}

} // namespace OST::ExtractTickets
