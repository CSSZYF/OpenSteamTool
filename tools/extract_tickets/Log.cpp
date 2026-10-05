#include "Log.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace OST::ExtractTickets {

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
} // namespace

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

void InitLogging(const std::string& logFileName) {
#if defined(_DEBUG) || !defined(NDEBUG)
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
#else
    (void)logFileName;
#endif
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

#if defined(_DEBUG) || !defined(NDEBUG)
    if (!g_loggingInitialized) {
        // Auto-init for debug builds
        g_logFile.open("extract_tickets_debug.log", std::ios::app);
        g_loggingInitialized = true;
    }

    const std::string line = std::format("[{}] [{:<5}] [{}] {}",
                                         GetTimestampString(),
                                         LogLevelToString(level),
                                         tag,
                                         message);

    if (g_logFile.is_open()) {
        g_logFile << line << "\n";
        g_logFile.flush();
    }
#else
    // Release mode: only output Warnings and Errors to console
    if (level == LogLevel::Warn) {
        std::cerr << "[WARN] " << message << "\n";
    } else if (level == LogLevel::Error) {
        std::cerr << "[ERROR] " << message << "\n";
    }
#endif
}

} // namespace OST::ExtractTickets
