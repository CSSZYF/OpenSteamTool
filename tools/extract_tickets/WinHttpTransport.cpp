#include "WinHttpTransport.h"
#include "Log.h"
#include "Utils.h"

#include <chrono>
#include <iostream>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace OST::ExtractTickets {

namespace {

    struct ParsedUrl {
        std::wstring host;
        std::wstring path;
        INTERNET_PORT port{INTERNET_DEFAULT_HTTPS_PORT};
        bool isHttps{true};
    };

    bool ParseUrl(std::string_view urlStr, ParsedUrl& outParsed) {
        std::string normalizedUrl(urlStr);
        bool isWss = false;
        bool isWs = false;
        if (normalizedUrl.starts_with("wss://")) {
            isWss = true;
            normalizedUrl.replace(0, 6, "https://");
        } else if (normalizedUrl.starts_with("ws://")) {
            isWs = true;
            normalizedUrl.replace(0, 5, "http://");
        }

        std::wstring wideUrl = Utf8ToWide(normalizedUrl);
        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
        urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &urlComp)) {
            LOG_WARN("WinHttp", "WinHttpCrackUrl 失败: {} (GetLastError={})", MaskUrl(urlStr), GetLastError());
            return false;
        }

        outParsed.host.assign(urlComp.lpszHostName, urlComp.dwHostNameLength);
        outParsed.path.assign(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (urlComp.dwExtraInfoLength > 0) {
            outParsed.path.append(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
        }
        if (outParsed.path.empty()) {
            outParsed.path = L"/";
        }

        outParsed.port = urlComp.nPort;
        if (isWss) {
            outParsed.isHttps = true;
            if (outParsed.port == 0 || outParsed.port == INTERNET_DEFAULT_HTTPS_PORT) {
                outParsed.port = INTERNET_DEFAULT_HTTPS_PORT;
            }
        } else if (isWs) {
            outParsed.isHttps = false;
            if (outParsed.port == 0 || outParsed.port == INTERNET_DEFAULT_HTTP_PORT) {
                outParsed.port = INTERNET_DEFAULT_HTTP_PORT;
            }
        } else {
            outParsed.isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
        }
        return true;
    }

    struct ScopedHInternet {
        HINTERNET handle{nullptr};
        ScopedHInternet() = default;
        explicit ScopedHInternet(HINTERNET h) noexcept : handle(h) {}
        ~ScopedHInternet() noexcept {
            if (handle) WinHttpCloseHandle(handle);
        }
        ScopedHInternet(const ScopedHInternet&) = delete;
        ScopedHInternet& operator=(const ScopedHInternet&) = delete;
        ScopedHInternet(ScopedHInternet&& o) noexcept : handle(o.handle) { o.handle = nullptr; }
        ScopedHInternet& operator=(ScopedHInternet&& o) noexcept {
            if (this != &o) {
                if (handle) WinHttpCloseHandle(handle);
                handle = o.handle;
                o.handle = nullptr;
            }
            return *this;
        }
        operator HINTERNET() const noexcept { return handle; }
        void Reset(HINTERNET h = nullptr) noexcept {
            if (handle) WinHttpCloseHandle(handle);
            handle = h;
        }
    };
} // namespace

// ============================================================================
// WinHttpTransport (HTTPS Client)
// ============================================================================

WinHttpTransport::WinHttpTransport() {
    m_hSession = WinHttpOpen(
        L"OpenSteamTool-ExtractTickets/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!m_hSession) {
        m_hSession = WinHttpOpen(
            L"OpenSteamTool-ExtractTickets/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }

    if (!m_hSession) {
        LOG_ERROR("WinHttp", "WinHttpOpen 失败 (GetLastError={})", GetLastError());
    } else {
        // Enforce TLS 1.2 and TLS 1.3
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(m_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        // Timeouts: resolve, connect, send, receive (all 15s)
        WinHttpSetTimeouts(m_hSession, 15000, 15000, 15000, 15000);
    }
}

WinHttpTransport::~WinHttpTransport() {
    InvalidateConnection();
    if (m_hSession) {
        WinHttpCloseHandle(m_hSession);
        m_hSession = nullptr;
    }
}

void WinHttpTransport::SetTimeouts(int resolveMs, int connectMs, int sendMs, int receiveMs) {
    if (m_hSession) {
        WinHttpSetTimeouts(m_hSession, resolveMs, connectMs, sendMs, receiveMs);
    }
}

HINTERNET WinHttpTransport::GetOrCreateConnection(const std::wstring& host, INTERNET_PORT port) {
    if (m_hCachedConnect && m_cachedConnectHost == host && m_cachedConnectPort == port) {
        return m_hCachedConnect;
    }
    InvalidateConnection();
    m_hCachedConnect = WinHttpConnect(m_hSession, host.c_str(), port, 0);
    if (m_hCachedConnect) {
        m_cachedConnectHost = host;
        m_cachedConnectPort = port;
    }
    return m_hCachedConnect;
}

void WinHttpTransport::InvalidateConnection() {
    if (m_hCachedConnect) {
        WinHttpCloseHandle(m_hCachedConnect);
        m_hCachedConnect = nullptr;
    }
    m_cachedConnectHost.clear();
    m_cachedConnectPort = 0;
}

HttpResponse WinHttpTransport::Get(
    std::string_view url,
    const std::vector<std::string>& extraHeaders,
    DWORD timeoutMs) {
    
    HttpResponse resp;
    if (!m_hSession) {
        resp.errorMessage = "WinHTTP 会话未就绪";
        return resp;
    }

    ParsedUrl parsed;
    if (!ParseUrl(url, parsed)) {
        resp.errorMessage = "无效的 URL 地址";
        return resp;
    }

    HINTERNET hConnect = GetOrCreateConnection(parsed.host, parsed.port);
    if (!hConnect) {
        resp.errorMessage = std::format("无法连接主机 {} (GetLastError={})", WideToUtf8(parsed.host), GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    DWORD flags = parsed.isHttps ? WINHTTP_FLAG_SECURE : 0;
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    auto executeRequest = [&](HINTERNET conn) -> ScopedHInternet {
        ScopedHInternet req{WinHttpOpenRequest(
            conn,
            L"GET",
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags)};
        if (!req) return req;

        if (timeoutMs > 0) {
            WinHttpSetTimeouts(req, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        }

        if (parsed.isHttps) {
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        }

        for (const auto& hdr : extraHeaders) {
            std::wstring wHdr = Utf8ToWide(hdr);
            WinHttpAddRequestHeaders(req, wHdr.c_str(), static_cast<DWORD>(wHdr.size()), WINHTTP_ADDREQ_FLAG_ADD);
        }

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            req.Reset();
        }
        return req;
    };

    auto startT = std::chrono::steady_clock::now();
    ScopedHInternet hRequest = executeRequest(hConnect);

    // If request failed on a cached connection (e.g. server closed idle keep-alive socket), retry once on fresh connection
    if (!hRequest) {
        InvalidateConnection();
        hConnect = GetOrCreateConnection(parsed.host, parsed.port);
        if (hConnect) {
            hRequest = executeRequest(hConnect);
        }
    }

    if (!hRequest) {
        resp.errorMessage = std::format("发送 GET 请求失败 (GetLastError={})", GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        resp.errorMessage = std::format("接收响应头失败 (GetLastError={})", GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    resp.statusCode = static_cast<int>(statusCode);

    // Fast memory pre-allocation if server returns Content-Length
    DWORD contentLength = 0;
    DWORD clSize = sizeof(contentLength);
    if (WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &contentLength,
            &clSize,
            WINHTTP_NO_HEADER_INDEX) && contentLength > 0) {
        resp.body.reserve(contentLength);
    }

    std::vector<char> buffer(131072); // 128 KB buffer for high-throughput streaming
    while (true) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) || bytesRead == 0) {
            break;
        }
        resp.body.append(buffer.data(), bytesRead);
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startT).count();
    LOG_DEBUG("WinHttp", "GET 完成 (HTTP {}, 大小: {} 字节, 耗时: {}ms)", resp.statusCode, resp.body.size(), elapsedMs);

    return resp;
}

HttpResponse WinHttpTransport::Post(
    std::string_view url,
    std::string_view postData,
    std::string_view contentType,
    const std::vector<std::string>& extraHeaders,
    DWORD timeoutMs) {

    HttpResponse resp;
    if (!m_hSession) {
        resp.errorMessage = "WinHTTP 会话未就绪";
        return resp;
    }

    ParsedUrl parsed;
    if (!ParseUrl(url, parsed)) {
        resp.errorMessage = "无效的 URL 地址";
        return resp;
    }

    HINTERNET hConnect = GetOrCreateConnection(parsed.host, parsed.port);
    if (!hConnect) {
        resp.errorMessage = std::format("无法连接主机 {} (GetLastError={})", WideToUtf8(parsed.host), GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    DWORD flags = parsed.isHttps ? WINHTTP_FLAG_SECURE : 0;
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    DWORD postLen = static_cast<DWORD>(postData.size());
    void* pData = postLen > 0 ? const_cast<char*>(postData.data()) : WINHTTP_NO_REQUEST_DATA;

    auto executeRequest = [&](HINTERNET conn) -> ScopedHInternet {
        ScopedHInternet req{WinHttpOpenRequest(
            conn,
            L"POST",
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags)};
        if (!req) return req;

        if (timeoutMs > 0) {
            WinHttpSetTimeouts(req, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        }

        if (parsed.isHttps) {
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        }

        if (!contentType.empty()) {
            std::string ct = "Content-Type: " + std::string{contentType};
            std::wstring wCt = Utf8ToWide(ct);
            WinHttpAddRequestHeaders(req, wCt.c_str(), static_cast<DWORD>(wCt.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        for (const auto& hdr : extraHeaders) {
            std::wstring wHdr = Utf8ToWide(hdr);
            WinHttpAddRequestHeaders(req, wHdr.c_str(), static_cast<DWORD>(wHdr.size()), WINHTTP_ADDREQ_FLAG_ADD);
        }

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pData, postLen, postLen, 0)) {
            req.Reset();
        }
        return req;
    };

    auto startT = std::chrono::steady_clock::now();
    ScopedHInternet hRequest = executeRequest(hConnect);

    if (!hRequest) {
        InvalidateConnection();
        hConnect = GetOrCreateConnection(parsed.host, parsed.port);
        if (hConnect) {
            hRequest = executeRequest(hConnect);
        }
    }

    if (!hRequest) {
        resp.errorMessage = std::format("创建或发送 POST 请求失败 (GetLastError={})", GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        resp.errorMessage = std::format("接收响应头失败 (GetLastError={})", GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    resp.statusCode = static_cast<int>(statusCode);

    DWORD contentLength = 0;
    DWORD clSize = sizeof(contentLength);
    if (WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &contentLength,
            &clSize,
            WINHTTP_NO_HEADER_INDEX) && contentLength > 0) {
        resp.body.reserve(contentLength);
    }

    std::vector<char> buffer(131072);
    while (true) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) || bytesRead == 0) {
            break;
        }
        resp.body.append(buffer.data(), bytesRead);
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startT).count();
    LOG_DEBUG("WinHttp", "POST 完成 (HTTP {}, 大小: {} 字节, 耗时: {}ms)", resp.statusCode, resp.body.size(), elapsedMs);

    return resp;
}

// ============================================================================
// WebSocketClient Implementation
// ============================================================================

WebSocketClient::WebSocketClient() {
    m_hSession = WinHttpOpen(
        L"OpenSteamTool-WebSocket/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!m_hSession) {
        m_hSession = WinHttpOpen(
            L"OpenSteamTool-WebSocket/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }

    if (m_hSession) {
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(m_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));
        WinHttpSetTimeouts(m_hSession, 5000, 5000, 5000, 5000);
    }
}

WebSocketClient::~WebSocketClient() {
    Close();
    if (m_hSession) {
        WinHttpCloseHandle(m_hSession);
        m_hSession = nullptr;
    }
}

void WebSocketClient::Close() {
    if (m_hWebSocket) {
        WinHttpWebSocketClose(m_hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(m_hWebSocket);
        m_hWebSocket = nullptr;
    }
    if (m_hRequest) {
        WinHttpCloseHandle(m_hRequest);
        m_hRequest = nullptr;
    }
    if (m_hConnect) {
        WinHttpCloseHandle(m_hConnect);
        m_hConnect = nullptr;
    }
}

bool WebSocketClient::Connect(std::string_view wssUrl, DWORD timeoutMs) {
    Close();
    if (!m_hSession) {
        LOG_ERROR("WebSocket", "WinHTTP 会话无效");
        return false;
    }

    ParsedUrl parsed;
    if (!ParseUrl(wssUrl, parsed)) {
        LOG_ERROR("WebSocket", "无效的 WebSocket URL: {}", wssUrl);
        return false;
    }

    LOG_DEBUG("WebSocket", "正在建立连接 -> {}:{} (Path: {}, SSL: {})",
              WideToUtf8(parsed.host), parsed.port, WideToUtf8(parsed.path), parsed.isHttps);

    m_hConnect = WinHttpConnect(m_hSession, parsed.host.c_str(), parsed.port, 0);
    if (!m_hConnect) {
        LOG_ERROR("WebSocket", "无法连接 WebSocket 主机: {} (GetLastError={})", WideToUtf8(parsed.host), GetLastError());
        return false;
    }

    DWORD flags = parsed.isHttps ? WINHTTP_FLAG_SECURE : 0;
    m_hRequest = WinHttpOpenRequest(
        m_hConnect,
        L"GET",
        parsed.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);

    if (!m_hRequest) {
        LOG_ERROR("WebSocket", "创建 WebSocket 握手请求失败 (GetLastError={})", GetLastError());
        Close();
        return false;
    }

    // Request WebSocket Upgrade
    if (!WinHttpSetOption(m_hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        LOG_ERROR("WebSocket", "请求升级 WebSocket 选项失败 (GetLastError={})", GetLastError());
        Close();
        return false;
    }

    if (parsed.isHttps) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(m_hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    WinHttpSetTimeouts(m_hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    auto startT = std::chrono::steady_clock::now();
    if (!WinHttpSendRequest(m_hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
        LOG_ERROR("WebSocket", "发送 WebSocket 升级握手请求失败 (GetLastError={})", GetLastError());
        Close();
        return false;
    }

    if (!WinHttpReceiveResponse(m_hRequest, nullptr)) {
        LOG_ERROR("WebSocket", "接收 WebSocket 握手响应失败 (GetLastError={})", GetLastError());
        Close();
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        m_hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 101) {
        LOG_ERROR("WebSocket", "WebSocket 握手被拒绝 (HTTP 状态码: {})", statusCode);
        Close();
        return false;
    }

    m_hWebSocket = WinHttpWebSocketCompleteUpgrade(m_hRequest, 0);
    WinHttpCloseHandle(m_hRequest);
    m_hRequest = nullptr;

    if (!m_hWebSocket) {
        LOG_ERROR("WebSocket", "WinHttpWebSocketCompleteUpgrade 失败 (GetLastError={})", GetLastError());
        Close();
        return false;
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startT).count();
    LOG_DEBUG("WebSocket", "已成功建立安全 WebSocket 连接: {} (耗时: {}ms)", wssUrl, elapsedMs);
    return true;
}

bool WebSocketClient::Send(std::span<const uint8_t> data, bool isBinary) {
    if (!m_hWebSocket) return false;

    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType = isBinary ?
        WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE :
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;

    DWORD error = WinHttpWebSocketSend(
        m_hWebSocket,
        bufType,
        const_cast<void*>(reinterpret_cast<const void*>(data.data())),
        static_cast<DWORD>(data.size()));

    if (error != ERROR_SUCCESS) {
        LOG_ERROR("WebSocket", "WinHttpWebSocketSend 失败 (error={})", error);
        Close();
        return false;
    }
    return true;
}

bool WebSocketClient::Receive(std::vector<uint8_t>& outData, bool& isBinary, DWORD timeoutMs) {
    if (!m_hWebSocket) return false;

    if (timeoutMs > 0) {
        DWORD t = timeoutMs;
        WinHttpSetOption(m_hWebSocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    }

    outData.clear();
    std::vector<uint8_t> chunk(65536);

    while (true) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;

        DWORD error = WinHttpWebSocketReceive(
            m_hWebSocket,
            chunk.data(),
            static_cast<DWORD>(chunk.size()),
            &bytesRead,
            &bufType);

        if (error != ERROR_SUCCESS) {
            LOG_ERROR("WebSocket", "WinHttpWebSocketReceive 错误 (error={})", error);
            if (error != ERROR_WINHTTP_TIMEOUT) {
                Close();
            }
            return false;
        }

        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            LOG_WARN("WebSocket", "收到对端 WebSocket 关闭帧");
            Close();
            return false;
        }

        if (bytesRead > 0) {
            outData.insert(outData.end(), chunk.begin(), chunk.begin() + bytesRead);
        }

        if (bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            isBinary = true;
            break;
        } else if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            isBinary = false;
            break;
        }
        // If buffer type is fragment (..._FRAGMENT_BUFFER_TYPE), continue reading
    }

    return true;
}

} // namespace OST::ExtractTickets
