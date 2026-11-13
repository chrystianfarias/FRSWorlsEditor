#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// HTTPS through the system's WinHTTP: no library to ship, and the same
// certificates and proxy settings as the rest of Windows. Blocking -- call it
// from a worker thread, never from the UI.
namespace net {

struct Response {
    int status = 0;            // HTTP status, 0 when the request never got an answer
    std::string body;          // empty when written to a file instead
};

// `headers` are full lines ("Authorization: Token abc"). With `sink` the body
// goes to that file rather than into the response. `progress` is told the
// bytes so far and the total (0 when unknown); returning false stops it.
bool Request(const std::wstring& method, const std::string& url, const std::vector<std::wstring>& headers,
             const std::string& body, Response& out, std::wstring& error, FILE* sink = nullptr,
             const std::function<bool(size_t, size_t)>& progress = {});

inline bool Get(const std::string& url, const std::vector<std::wstring>& headers, Response& out,
                std::wstring& error) {
    return Request(L"GET", url, headers, {}, out, error);
}

// Percent-encoding for a query value.
std::string UrlEncode(const std::string& text);

} // namespace net
