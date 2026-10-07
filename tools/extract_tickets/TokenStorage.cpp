#include "TokenStorage.h"
#include "Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

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

    std::string SerializeAccounts(const std::vector<CachedAccount>& accounts) {
        std::ostringstream oss;
        for (const auto& acc : accounts) {
            oss << acc.accountName << '\t'
                << acc.steamId << '\t'
                << acc.lastLoginTime << '\t'
                << acc.refreshToken << '\t'
                << acc.accessToken << '\n';
        }
        return oss.str();
    }

    std::vector<CachedAccount> DeserializeAccounts(std::string_view plaintext) {
        std::vector<CachedAccount> result;
        std::istringstream iss(std::string{plaintext});
        std::string line;

        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::istringstream lineStream(line);
            CachedAccount acc;
            std::string steamIdStr;
            std::string timeStr;

            if (std::getline(lineStream, acc.accountName, '\t') &&
                std::getline(lineStream, steamIdStr, '\t') &&
                std::getline(lineStream, timeStr, '\t')) {
                
                if (std::getline(lineStream, acc.refreshToken, '\t')) {
                    // Has 5th field: accessToken
                    std::getline(lineStream, acc.accessToken);
                } else {
                    // 4th field without trailing tab
                    acc.refreshToken = "";
                }

                if (acc.refreshToken.empty()) {
                    // Try parsing 4th field without tab delimiter
                    std::istringstream fallbackStream(line);
                    std::string dummy1, dummy2, dummy3;
                    std::getline(fallbackStream, dummy1, '\t');
                    std::getline(fallbackStream, dummy2, '\t');
                    std::getline(fallbackStream, dummy3, '\t');
                    std::getline(fallbackStream, acc.refreshToken);
                }

                uint64_t sId = 0;
                const auto [p1, ec1] = std::from_chars(steamIdStr.data(), steamIdStr.data() + steamIdStr.size(), sId);
                int64_t loginTime = 0;
                const auto [p2, ec2] = std::from_chars(timeStr.data(), timeStr.data() + timeStr.size(), loginTime);

                if (ec1 == std::errc{} && p1 == steamIdStr.data() + steamIdStr.size() &&
                    ec2 == std::errc{} && p2 == timeStr.data() + timeStr.size()) {
                    acc.steamId = sId;
                    acc.lastLoginTime = loginTime;
                    if (!acc.accountName.empty() && !acc.refreshToken.empty()) {
                        result.push_back(std::move(acc));
                    }
                } else {
                    LOG_WARN("TokenStorage", "跳过解析异常的账号条目");
                }
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
                std::vector<uint8_t> zeros(1024, 0);
                size_t remaining = fileSize;
                while (remaining > 0) {
                    size_t toWrite = (std::min)(remaining, zeros.size());
                    ofs.write(reinterpret_cast<const char*>(zeros.data()), toWrite);
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
    wchar_t localAppData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)) || localAppData[0] == L'\0') {
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return L"accounts.enc";
        }
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

    if (accounts.empty()) {
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

    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        LOG_ERROR("TokenStorage", "无法创建凭据目标文件: {}", MaskPath(filePath.string()));
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

    LocalFree(outBlob.pbData);
    LOG_DEBUG("TokenStorage", "成功持久化 {} 个账号到 DPAPI 存储", accounts.size());
    return true;
}

bool TokenStorage::UpsertAccount(const CachedAccount& account) {
    auto accounts = LoadAccounts();
    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.accountName == account.accountName;
    });

    if (it != accounts.end()) {
        *it = account;
    } else {
        accounts.push_back(account);
    }

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
    return true;
}

} // namespace OST::ExtractTickets
