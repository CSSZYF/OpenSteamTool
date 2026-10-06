#include "Log.h"
#include "TuiEngine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace OST::ExtractTickets {

#if EXTRACT_TICKETS_HAS_LOGGING
namespace {
    std::mutex g_logMutex;
    std::ofstream g_logFile;
    bool g_loggingInitialized = false;


    std::string GetTimestampString() {
        const auto now = std::chrono::system_clock::now();
        const auto nowTimeT = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tmNow{};
#if defined(_WIN32)
        localtime_s(&tmNow, &nowTimeT);
#else
        localtime_r(&nowTimeT, &tmNow);
#endif

        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                           tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                           tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
                           static_cast<int>(ms.count()));
    }

    const char* LogLevelToString(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
            default:              return "UNKNOWN";
        }
    }

    std::string RedactMessage(std::string_view msg) {
        if (msg.empty()) return {};
        std::string result{msg};

        // 1. JWT tokens: scan for "eyJ"
        size_t pos = 0;
        while ((pos = result.find("eyJ", pos)) != std::string::npos) {
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(result[pos - 1])) || result[pos - 1] == '_' || result[pos - 1] == '-')) {
                pos += 3;
                continue;
            }
            size_t endPos = pos + 3;
            while (endPos < result.size() &&
                   (std::isalnum(static_cast<unsigned char>(result[endPos])) ||
                    result[endPos] == '_' || result[endPos] == '-' || result[endPos] == '.')) {
                ++endPos;
            }
            size_t len = endPos - pos;
            if (len >= 30) {
                std::string masked = MaskToken(std::string_view(result.data() + pos, len));
                result.replace(pos, len, masked);
                pos += masked.size();
            } else {
                pos += 3;
            }
        }

        // 2. Windows User Path redaction
        if (result.find("Users\\") != std::string::npos || result.find("Users/") != std::string::npos ||
            result.find("users\\") != std::string::npos || result.find("users/") != std::string::npos) {
            result = MaskPath(result);
        }

        return result;
    }
} // namespace
#endif // EXTRACT_TICKETS_HAS_LOGGING

std::string MaskAccount(std::string_view account) {
    if (account.empty()) return "***";
    const size_t len = account.size();
    if (len <= 2) {
        return std::string{account.substr(0, 1)} + "*";
    }
    if (len == 3) {
        return std::string{account.substr(0, 1)} + "*" + std::string{account.substr(2, 1)};
    }
    return std::string{account.substr(0, 2)} + "***" + std::string{account.substr(len - 2)};
}

std::string MaskSteamId(uint64_t steamId) {
    if (steamId == 0) return "0";
    std::string s = std::to_string(steamId);
    if (s.size() >= 17) {
        // e.g. 76561198 01234 5678 -> 76561198****5678
        return s.substr(0, 8) + "****" + s.substr(s.size() - 4);
    }
    if (s.size() > 6) {
        return s.substr(0, 3) + "***" + s.substr(s.size() - 2);
    }
    return "***";
}

std::string MaskToken(std::string_view token, size_t prefixChars, size_t suffixChars) {
    if (token.empty()) return "null";
    if (token.size() <= prefixChars + suffixChars) {
        return "***";
    }
    return std::format("{}...[MASKED_len={}]...{}",
                       token.substr(0, prefixChars),
                       token.size(),
                       token.substr(token.size() - suffixChars));
}

std::string MaskTicketHex(std::span<const uint8_t> ticket, size_t prefixBytes, size_t suffixBytes) {
    if (ticket.empty()) return "null";
    if (ticket.size() <= prefixBytes + suffixBytes) {
        return std::format("[TICKET_{}B]", ticket.size());
    }

    auto toHexStr = [](std::span<const uint8_t> bytes) -> std::string {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            hex += std::format("{:02X}", b);
        }
        return hex;
    };

    const std::string headHex = toHexStr(ticket.subspan(0, prefixBytes));
    const std::string tailHex = toHexStr(ticket.subspan(ticket.size() - suffixBytes));
    return std::format("{}...[TICKET_{}B]...{}", headHex, ticket.size(), tailHex);
}

std::string MaskKeyHex(std::string_view hexKey) {
    if (hexKey.empty()) return "null";
    if (hexKey.size() >= 8) {
        return std::string{hexKey.substr(0, 4)} + "***" + std::string{hexKey.substr(hexKey.size() - 4)};
    }
    return "***";
}

std::string MaskGroupId(std::string_view groupId) {
    if (groupId.empty()) return "***";
    const size_t len = groupId.size();
    if (len <= 4) {
        return std::string{groupId.substr(0, 1)} + "**" + std::string{groupId.substr(len - 1)};
    }
    return std::string{groupId.substr(0, 3)} + "****" + std::string{groupId.substr(len - 2)};
}

std::string MaskGroupId(uint64_t groupId) {
    return MaskGroupId(std::to_string(groupId));
}

std::string MaskEmail(std::string_view email) {
    if (email.empty()) return "***";
    size_t atPos = email.find('@');
    if (atPos == std::string_view::npos) {
        return MaskAccount(email);
    }
    std::string_view local = email.substr(0, atPos);
    std::string_view domain = email.substr(atPos); // includes '@'
    if (local.size() <= 1) {
        return "*" + std::string{domain};
    }
    if (local.size() <= 3) {
        return std::string{local.substr(0, 1)} + "*@" + std::string{email.substr(atPos + 1)};
    }
    return std::string{local.substr(0, 2)} + "***" + std::string{local.substr(local.size() - 1)} + std::string{domain};
}

std::string MaskPicsToken(uint64_t picsToken) {
    if (picsToken == 0) return "0";
    std::string s = std::to_string(picsToken);
    if (s.size() >= 8) {
        return s.substr(0, 4) + "****" + s.substr(s.size() - 4);
    }
    return "***";
}

std::string MaskPath(std::string_view path) {
    if (path.empty()) return {};
    std::string s{path};
    std::string lowerS = s;
    for (char& c : lowerS) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t pos = 0;
    while ((pos = lowerS.find("users", pos)) != std::string::npos) {
        size_t slashPos = pos + 5;
        if (slashPos < s.size() && (s[slashPos] == '\\' || s[slashPos] == '/')) {
            size_t userStart = slashPos + 1;
            size_t nextSlash = s.find_first_of("\\/", userStart);
            if (nextSlash != std::string::npos && nextSlash > userStart) {
                std::string_view userPart(s.data() + userStart, nextSlash - userStart);
                std::string maskedUser = MaskAccount(userPart);
                s.replace(userStart, nextSlash - userStart, maskedUser);
                lowerS.replace(userStart, nextSlash - userStart, maskedUser);
                pos = userStart + maskedUser.size();
                continue;
            } else if (nextSlash == std::string::npos && userStart < s.size()) {
                std::string_view userPart(s.data() + userStart, s.size() - userStart);
                std::string maskedUser = MaskAccount(userPart);
                s.replace(userStart, s.size() - userStart, maskedUser);
                break;
            }
        }
        pos += 5;
    }
    return s;
}

std::string MaskUrl(std::string_view url) {
    if (url.empty()) return {};
    std::string s{url};
    constexpr std::string_view sensitiveKeys[] = {
        "access_token=", "refresh_token=", "password=", "auth_token="
    };

    for (const auto& key : sensitiveKeys) {
        size_t pos = 0;
        while ((pos = s.find(key, pos)) != std::string::npos) {
            size_t valStart = pos + key.size();
            size_t valEnd = s.find_first_of("&# \r\n", valStart);
            if (valEnd == std::string::npos) {
                valEnd = s.size();
            }
            if (valEnd > valStart) {
                std::string_view val(s.data() + valStart, valEnd - valStart);
                std::string masked = MaskToken(val);
                s.replace(valStart, valEnd - valStart, masked);
                pos = valStart + masked.size();
            } else {
                pos = valStart;
            }
        }
    }
    return s;
}

#if EXTRACT_TICKETS_HAS_LOGGING

void InitLogging(const std::string& logFileName) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_loggingInitialized) {
        g_logFile.open(logFileName, std::ios::app);
        g_loggingInitialized = true;
        if (g_logFile.is_open()) {
            g_logFile << "\n======================================================================\n"
                      << "  extract_tickets Debug Session Started at " << GetTimestampString() << "\n"
                      << "======================================================================\n";
            g_logFile.flush();
        }
    }
}

void CloseLogging() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }
    g_loggingInitialized = false;
}

void LogMessage(LogLevel level, std::string_view tag, std::string_view message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_loggingInitialized) {
        g_logFile.open("extract_tickets_debug.log", std::ios::app);
        g_loggingInitialized = true;
    }

    const std::string safeMsg = RedactMessage(message);

    const std::string line = std::format("[{}] [{:<5}] [{}] {}",
                                         GetTimestampString(),
                                         LogLevelToString(level),
                                         tag,
                                         safeMsg);

    if (g_logFile.is_open()) {
        g_logFile << line << "\n";
        g_logFile.flush();
    }

    // Only output to console if TUI is NOT active
    if (!TuiEngine::IsActive()) {
        if (level == LogLevel::Warn) {
            std::cerr << "[WARN] " << safeMsg << "\n";
        } else if (level == LogLevel::Error) {
            std::cerr << "[ERROR] " << safeMsg << "\n";
        }
    }
}

#endif // EXTRACT_TICKETS_HAS_LOGGING

} // namespace OST::ExtractTickets

