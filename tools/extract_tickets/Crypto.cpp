#include "Crypto.h"
#include "Log.h"

#include <bcrypt.h>
#include <conio.h>
#include <wincrypt.h>

#include <algorithm>
#include <iostream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace OST::ExtractTickets {

// ============================================================================
// SecureString Implementation
// ============================================================================

SecureString::SecureString() {
    m_data.reserve(128);
}

SecureString::SecureString(std::string_view str) {
    m_data.reserve((std::max)(static_cast<size_t>(128), str.size()));
    m_data.assign(str);
}

SecureString::~SecureString() {
    Clear();
}

SecureString::SecureString(const SecureString& other) {
    m_data.reserve((std::max)(static_cast<size_t>(128), other.m_data.size()));
    m_data.assign(other.m_data);
}

SecureString& SecureString::operator=(const SecureString& other) {
    if (this != &other) {
        Clear();
        m_data.reserve((std::max)(static_cast<size_t>(128), other.m_data.size()));
        m_data.assign(other.m_data);
    }
    return *this;
}

SecureString::SecureString(SecureString&& other) noexcept : m_data(std::move(other.m_data)) {}

SecureString& SecureString::operator=(SecureString&& other) noexcept {
    if (this != &other) {
        Clear();
        m_data = std::move(other.m_data);
    }
    return *this;
}

void SecureString::Assign(std::string_view str) {
    Clear();
    m_data.reserve((std::max)(static_cast<size_t>(128), str.size()));
    m_data.assign(str);
}

void SecureString::Append(char c) {
    m_data.push_back(c);
}

void SecureString::PopBack() {
    if (!m_data.empty()) {
        volatile char* p = &m_data.back();
        *p = 0;
        m_data.pop_back();
    }
}

void SecureString::Clear() {
    if (m_data.capacity() > 0) {
        SecureZeroMemory(m_data.data(), m_data.capacity());
        m_data.clear();
    }
}

// ============================================================================
// Password Console Input with Mask
// ============================================================================

SecureString ReadPasswordFromConsole(const char* prompt) {
    if (prompt && *prompt) {
        std::cout << prompt;
        std::cout.flush();
    }

    SecureString pwd;
    while (true) {
        int ch = _getch();
        if (ch == '\r' || ch == '\n') {
            std::cout << "\n";
            break;
        } else if (ch == '\b' || ch == 127) { // Backspace
            if (!pwd.Empty()) {
                pwd.PopBack();
                std::cout << "\b \b";
                std::cout.flush();
            }
        } else if (ch == 0 || ch == 0xE0) {
            // Extended keys (arrows, function keys) -> consume trailing byte
            _getch();
        } else if (ch == 3) { // Ctrl+C
            pwd.Clear();
            std::cout << "\n[INFO] 用户中断输入。\n";
            exit(0);
        } else if (ch >= 32 && ch <= 126) { // Printable characters
            pwd.Append(static_cast<char>(ch));
            std::cout << '*';
            std::cout.flush();
        }
    }
    return pwd;
}

// ============================================================================
// Hex & Base64 Helpers
// ============================================================================

std::vector<uint8_t> HexToBytes(std::string_view hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto hexCharVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int high = hexCharVal(hex[i]);
        int low = hexCharVal(hex[i + 1]);
        if (high >= 0 && low >= 0) {
            bytes.push_back(static_cast<uint8_t>((high << 4) | low));
        }
    }
    return bytes;
}

std::string Base64Encode(std::span<const uint8_t> data) {
    if (data.empty()) return {};

    DWORD charsNeeded = 0;
    if (!CryptBinaryToStringA(
            data.data(),
            static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &charsNeeded) || charsNeeded == 0) {
        return {};
    }

    std::string result(charsNeeded, '\0');
    if (!CryptBinaryToStringA(
            data.data(),
            static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            result.data(),
            &charsNeeded)) {
        return {};
    }

    // CryptBinaryToStringA includes null-terminator in charsNeeded
    while (!result.empty() && (result.back() == '\0' || result.back() == '\r' || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

std::vector<uint8_t> Base64Decode(std::string_view base64Str) {
    if (base64Str.empty()) return {};

    DWORD bytesNeeded = 0;
    if (!CryptStringToBinaryA(
            base64Str.data(),
            static_cast<DWORD>(base64Str.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &bytesNeeded,
            nullptr,
            nullptr) || bytesNeeded == 0) {
        return {};
    }

    std::vector<uint8_t> buffer(bytesNeeded);
    if (!CryptStringToBinaryA(
            base64Str.data(),
            static_cast<DWORD>(base64Str.size()),
            CRYPT_STRING_BASE64,
            buffer.data(),
            &bytesNeeded,
            nullptr,
            nullptr)) {
        return {};
    }
    buffer.resize(bytesNeeded);
    return buffer;
}

// ============================================================================
// Steam RSA Encryption via Windows CNG
// ============================================================================

std::string EncryptPasswordWithRSA(
    const SecureString& password,
    std::string_view modHex,
    std::string_view expHex) {

    if (password.Empty() || modHex.empty() || expHex.empty()) {
        LOG_ERROR("Crypto", "RSA 加密入参为空 / Empty parameter for RSA encryption");
        return {};
    }

    std::vector<uint8_t> modBytes = HexToBytes(modHex);
    std::vector<uint8_t> expBytes = HexToBytes(expHex);

    if (modBytes.empty() || expBytes.empty()) {
        LOG_ERROR("Crypto", "RSA 公钥 Modulus 或 Exponent 格式无效 / Invalid RSA modulus/exponent format");
        return {};
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status != 0 || !hAlg) {
        LOG_ERROR("Crypto", "BCryptOpenAlgorithmProvider 失败 (status=0x{:08X})", static_cast<uint32_t>(status));
        return {};
    }

    struct AlgGuard {
        BCRYPT_ALG_HANDLE handle;
        ~AlgGuard() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algGuard{hAlg};

    // Construct BCRYPT_RSAKEY_BLOB:
    // [BCRYPT_RSAKEY_BLOB header][PublicExp][Modulus]
    const size_t blobSize = sizeof(BCRYPT_RSAKEY_BLOB) + expBytes.size() + modBytes.size();
    std::vector<uint8_t> keyBlob(blobSize);

    auto* pHeader = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(keyBlob.data());
    pHeader->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    pHeader->BitLength = static_cast<ULONG>(modBytes.size() * 8);
    pHeader->cbPublicExp = static_cast<ULONG>(expBytes.size());
    pHeader->cbModulus = static_cast<ULONG>(modBytes.size());
    pHeader->cbPrime1 = 0;
    pHeader->cbPrime2 = 0;

    uint8_t* pDest = keyBlob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    std::memcpy(pDest, expBytes.data(), expBytes.size());
    pDest += expBytes.size();
    std::memcpy(pDest, modBytes.data(), modBytes.size());

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptImportKeyPair(
        hAlg,
        nullptr,
        BCRYPT_RSAPUBLIC_BLOB,
        &hKey,
        keyBlob.data(),
        static_cast<ULONG>(keyBlob.size()),
        0);

    if (status != 0 || !hKey) {
        LOG_ERROR("Crypto", "BCryptImportKeyPair 失败 (status=0x{:08X})", static_cast<uint32_t>(status));
        return {};
    }

    struct KeyGuard {
        BCRYPT_KEY_HANDLE handle;
        ~KeyGuard() { if (handle) BCryptDestroyKey(handle); }
    } keyGuard{hKey};

    // Encrypt password using PKCS#1 v1.5 padding (standard for Steam web authentication)
    ULONG encryptedSize = 0;
    status = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.c_str())),
        static_cast<ULONG>(password.Size()),
        nullptr,
        nullptr,
        0,
        nullptr,
        0,
        &encryptedSize,
        BCRYPT_PAD_PKCS1);

    if (status != 0 || encryptedSize == 0) {
        LOG_ERROR("Crypto", "BCryptEncrypt 测算大小失败 (status=0x{:08X})", static_cast<uint32_t>(status));
        return {};
    }

    std::vector<uint8_t> encryptedBytes(encryptedSize);
    ULONG actualWritten = 0;
    status = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.c_str())),
        static_cast<ULONG>(password.Size()),
        nullptr,
        nullptr,
        0,
        encryptedBytes.data(),
        encryptedSize,
        &actualWritten,
        BCRYPT_PAD_PKCS1);

    if (status != 0) {
        LOG_ERROR("Crypto", "BCryptEncrypt 加密执行失败 (status=0x{:08X})", static_cast<uint32_t>(status));
        return {};
    }

    encryptedBytes.resize(actualWritten);
    LOG_DEBUG("Crypto", "密码 RSA 加密成功，密文大小: {} 字节", actualWritten);

    return Base64Encode(encryptedBytes);
}

} // namespace OST::ExtractTickets
