#include "net/Http.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <memory>

namespace net {

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) ::WinHttpCloseHandle(h); }
};

} // namespace

std::string UrlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

bool Request(const std::wstring& method, const std::string& url, const std::vector<std::wstring>& headers,
             const std::string& body, Response& out, std::wstring& error, FILE* sink,
             const std::function<bool(size_t, size_t)>& progress) {
    out = {};
    const std::wstring wide = Widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4095;
    wchar_t extra[8192] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 8191;
    if (!::WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        error = L"Not a URL: " + wide;
        return false;
    }
    const std::wstring target = std::wstring(path) + extra;
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    // A generic name: nothing about the person or their project goes out.
    Handle session{ ::WinHttpOpen(L"FRSWorldEditor", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h) { error = L"WinHTTP could not start."; return false; }
    ::WinHttpSetTimeouts(session.h, 10000, 15000, 30000, 60000);
    Handle connection{ ::WinHttpConnect(session.h, host, parts.nPort, 0) };
    if (!connection.h) { error = L"Could not reach " + std::wstring(host); return false; }
    Handle request{ ::WinHttpOpenRequest(connection.h, method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) };
    if (!request.h) { error = L"Could not make the request."; return false; }

    std::wstring allHeaders;
    for (const std::wstring& line : headers) allHeaders += line + L"\r\n";
    if (!::WinHttpSendRequest(request.h, allHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : allHeaders.c_str(),
                              allHeaders.empty() ? 0 : (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                              (DWORD)body.size(), (DWORD)body.size(), 0) ||
        !::WinHttpReceiveResponse(request.h, nullptr)) {
        error = L"No answer from " + std::wstring(host) + L" (error " + std::to_wstring(::GetLastError()) + L").";
        return false;
    }

    DWORD status = 0, size = sizeof(status);
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;
    DWORD length = 0;
    size = sizeof(length);
    size_t total = 0;
    if (::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX))
        total = length;

    size_t got = 0;
    std::unique_ptr<char[]> chunk(new char[65536]);
    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.h, &available)) { error = L"The download broke off."; return false; }
        if (!available) break;
        while (available) {
            DWORD read = 0;
            const DWORD want = available < 65536 ? available : 65536;
            if (!::WinHttpReadData(request.h, chunk.get(), want, &read) || !read) { error = L"The download broke off."; return false; }
            if (sink) std::fwrite(chunk.get(), 1, read, sink);
            else out.body.append(chunk.get(), read);
            got += read;
            available -= read;
            if (progress && !progress(got, total)) { error = L"Cancelled."; return false; }
        }
    }
    return true;
}

} // namespace net
