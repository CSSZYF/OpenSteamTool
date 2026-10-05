#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

struct HttpResponse {
    int statusCode{0};
    std::string body;
    std::string errorMessage;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return statusCode >= 200 && statusCode < 300;
    }
};

class WinHttpTransport {
public:
    WinHttpTransport();
    ~WinHttpTransport();

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    [[nodiscard]] HttpResponse Get(
        std::string_view url,
        const std::vector<std::string>& extraHeaders = {});

    [[nodiscard]] HttpResponse Post(
        std::string_view url,
        std::string_view postData,
        std::string_view contentType = "application/x-www-form-urlencoded",
        const std::vector<std::string>& extraHeaders = {});

private:
    HINTERNET m_hSession{nullptr};
};

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Connects to a WebSocket endpoint (e.g. wss://cm.steampowered.com/cmsocket/)
    [[nodiscard]] bool Connect(std::string_view wssUrl, DWORD timeoutMs = 10000);

    // Sends a message frame (binary or text)
    bool Send(std::span<const uint8_t> data, bool isBinary = true);

    // Receives a complete message frame
    [[nodiscard]] bool Receive(std::vector<uint8_t>& outData, bool& isBinary, DWORD timeoutMs = 15000);

    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return m_hWebSocket != nullptr; }

private:
    HINTERNET m_hSession{nullptr};
    HINTERNET m_hConnect{nullptr};
    HINTERNET m_hRequest{nullptr};
    HINTERNET m_hWebSocket{nullptr};
};

} // namespace OST::ExtractTickets
