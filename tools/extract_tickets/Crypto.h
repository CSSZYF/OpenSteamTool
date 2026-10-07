#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

// ============================================================================
// RAII Secure String (Wiped on destruction with SecureZeroMemory)
// ============================================================================
class SecureString {
public:
    SecureString();
    explicit SecureString(std::string_view str);
    ~SecureString();

    SecureString(const SecureString& other);
    SecureString& operator=(const SecureString& other);
    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    void Assign(std::string_view str);
    void Append(char c);
    void PopBack();
    void Clear();

    [[nodiscard]] const std::string& Data() const noexcept { return m_data; }
    [[nodiscard]] const char* c_str() const noexcept { return m_data.c_str(); }
    [[nodiscard]] size_t Size() const noexcept { return m_data.size(); }
    [[nodiscard]] bool Empty() const noexcept { return m_data.empty(); }

private:
    std::string m_data;
};

// ============================================================================
// Windows CNG (BCrypt) & Crypto Utilities
// ============================================================================

// Reads password from console using _getch() with '*' mask or invisible
[[nodiscard]] SecureString ReadPasswordFromConsole(const char* prompt = "Steam 密码: ");

// Converts hex string to byte buffer
[[nodiscard]] std::vector<uint8_t> HexToBytes(std::string_view hex);

// Encodes binary data to standard Base64 string (no newlines)
[[nodiscard]] std::string Base64Encode(std::span<const uint8_t> data);

// Decodes standard Base64 string to binary data
[[nodiscard]] std::vector<uint8_t> Base64Decode(std::string_view base64);

// Parses expiration unix timestamp from Steam OAuth JWT access token (0 if missing/invalid)
[[nodiscard]] int64_t GetJwtExpiration(std::string_view jwt);

// Encrypts password using Steam's RSA public key (modulus & exponent)
// Returns Base64-encoded encrypted password ready for BeginAuthSessionViaCredentials
[[nodiscard]] std::string EncryptPasswordWithRSA(
    const SecureString& password,
    std::string_view modHex,
    std::string_view expHex);

} // namespace OST::ExtractTickets
