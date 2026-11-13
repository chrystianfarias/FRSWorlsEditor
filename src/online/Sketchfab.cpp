#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "online/Sketchfab.h"

#include "core/Log.h"
#include "net/Http.h"

#include "json.hpp"
#include <dpapi.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <random>

namespace sketchfab {

using nlohmann::json;

namespace {

constexpr const char* kApi = "https://api.sketchfab.com/v3";
constexpr unsigned short kLoginPort = 52890;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

std::vector<std::wstring> AuthHeaders(const Credentials& c) {
    if (!c.Valid()) return {};
    return { std::wstring(L"Authorization: ") + (c.oauth ? L"Bearer " : L"Token ") + Widen(c.token) };
}

std::string Str(const json& node, const char* key) {
    const auto it = node.find(key);
    return it != node.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int Int(const json& node, const char* key) {
    const auto it = node.find(key);
    return it != node.end() && it->is_number() ? it->get<int>() : 0;
}

std::wstring CredentialsPath() {
    wchar_t base[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L"";
    std::wstring folder = std::wstring(base) + L"\\FRSWorldEditor";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\sketchfab.dat";
}

std::wstring ApiError(const net::Response& response) {
    try {
        const json body = json::parse(response.body);
        const std::string detail = Str(body, "detail");
        if (!detail.empty()) return Widen(detail);
    } catch (...) {
    }
    return L"Sketchfab answered " + std::to_wstring(response.status) + L".";
}

} // namespace

bool Search(const Query& query, std::vector<Model>& out, std::string& next, std::wstring& error) {
    out.clear();
    std::string url = query.next;
    if (url.empty()) {
        url = std::string(kApi) + "/search?type=models&downloadable=true&archives_flavours=false&count=24";
        if (!query.text.empty()) url += "&q=" + net::UrlEncode(query.text);
        // The search can only bound faces; vertices are held to the limit
        // below, from what each result reports. Two faces a vertex is the
        // usual ratio of a closed mesh.
        if (query.maxVertices > 0) url += "&max_face_count=" + std::to_string(query.maxVertices * 2);
        if (!query.license.empty()) url += "&license=" + net::UrlEncode(query.license);
        if (!query.sort.empty()) url += "&sort_by=" + net::UrlEncode(query.sort);
    }
    net::Response response;
    if (!net::Get(url, {}, response, error)) return false;
    if (response.status != 200) { error = ApiError(response); return false; }
    try {
        const json body = json::parse(response.body);
        next = body.contains("next") && body["next"].is_string() ? body["next"].get<std::string>() : std::string();
        for (const json& r : body.value("results", json::array())) {
            Model m;
            m.uid = Str(r, "uid");
            m.name = Str(r, "name");
            m.viewerUrl = Str(r, "viewerUrl");
            m.faces = Int(r, "faceCount");
            m.vertices = Int(r, "vertexCount");
            if (r.contains("user") && r["user"].is_object())
                m.author = Str(r["user"], "displayName").empty() ? Str(r["user"], "username") : Str(r["user"], "displayName");
            if (r.contains("license") && r["license"].is_object()) {
                m.license = Str(r["license"], "label");
                m.licenseSlug = Str(r["license"], "slug");
            }
            if (r.contains("archives") && r["archives"].is_object() && r["archives"].contains("glb") &&
                r["archives"]["glb"].is_object()) {
                const json& glb = r["archives"]["glb"];
                m.glbFaces = Int(glb, "faceCount");
                m.glbVertices = Int(glb, "vertexCount");
                m.textures = Int(glb, "textureCount");
                m.textureMax = Int(glb, "textureMaxResolution");
                m.glbBytes = glb.contains("size") && glb["size"].is_number() ? glb["size"].get<size_t>() : 0;
            }
            // The smallest picture at least 200 wide.
            if (r.contains("thumbnails") && r["thumbnails"].contains("images")) {
                int best = 1 << 30;
                for (const json& image : r["thumbnails"]["images"]) {
                    const int width = Int(image, "width");
                    if (width >= 200 && width < best) { best = width; m.thumbnailUrl = Str(image, "url"); }
                }
            }
            // No derivative works: converting it for the game would be one.
            if (m.licenseSlug.find("nd") != std::string::npos) continue;
            const int vertices = m.glbVertices ? m.glbVertices : m.vertices;
            if (query.maxVertices > 0 && vertices > query.maxVertices) continue;
            if (!m.uid.empty()) out.push_back(std::move(m));
        }
    } catch (const std::exception& e) {
        error = L"Sketchfab's answer could not be read: " + Widen(e.what());
        return false;
    }
    return true;
}

bool FetchBytes(const std::string& url, std::string& bytes, std::wstring& error) {
    net::Response response;
    if (!net::Get(url, {}, response, error)) return false;
    if (response.status != 200) { error = L"HTTP " + std::to_wstring(response.status); return false; }
    bytes = std::move(response.body);
    return true;
}

bool LoadCredentials(Credentials& out) {
    out = {};
    const std::wstring path = CredentialsPath();
    FILE* file = nullptr;
    if (path.empty() || ::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    std::string sealed;
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) sealed.append(buffer, n);
    std::fclose(file);
    DATA_BLOB in{ (DWORD)sealed.size(), (BYTE*)sealed.data() }, plain{};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &plain)) return false;
    const std::string text((const char*)plain.pbData, plain.cbData);
    ::LocalFree(plain.pbData);
    try {
        const json j = json::parse(text);
        out.token = Str(j, "token");
        out.oauth = j.value("oauth", false);
        out.user = Str(j, "user");
        out.clientId = Str(j, "clientId");
        out.clientSecret = Str(j, "clientSecret");
    } catch (...) {
        return false;
    }
    return true;
}

bool SaveCredentials(const Credentials& c) {
    const json j = { { "token", c.token }, { "oauth", c.oauth }, { "user", c.user },
                     { "clientId", c.clientId }, { "clientSecret", c.clientSecret } };
    const std::string text = j.dump();
    DATA_BLOB in{ (DWORD)text.size(), (BYTE*)text.data() }, sealed{};
    if (!::CryptProtectData(&in, L"FRS World Editor Sketchfab", nullptr, nullptr, nullptr, 0, &sealed)) return false;
    const std::wstring path = CredentialsPath();
    FILE* file = nullptr;
    bool ok = false;
    if (!path.empty() && ::_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
        ok = std::fwrite(sealed.pbData, 1, sealed.cbData, file) == sealed.cbData;
        std::fclose(file);
    }
    ::LocalFree(sealed.pbData);
    return ok;
}

bool WhoAmI(const Credentials& credentials, std::string& user, std::wstring& error) {
    net::Response response;
    if (!net::Get(std::string(kApi) + "/me", AuthHeaders(credentials), response, error)) return false;
    if (response.status != 200) {
        error = response.status == 401 ? L"Sketchfab did not accept that token." : ApiError(response);
        return false;
    }
    try {
        const json body = json::parse(response.body);
        user = Str(body, "displayName").empty() ? Str(body, "username") : Str(body, "displayName");
    } catch (...) {
        user = "?";
    }
    return true;
}

bool OAuthLogin(Credentials& credentials, std::wstring& error) {
    if (credentials.clientId.empty()) {
        error = L"Logging in through Sketchfab needs this program's client id from Sketchfab.";
        return false;
    }
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = L"No network."; return false; }
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(kLoginPort);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (listener == INVALID_SOCKET || ::bind(listener, (sockaddr*)&address, sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
        if (listener != INVALID_SOCKET) ::closesocket(listener);
        ::WSACleanup();
        error = L"Port 52890 is taken; close what uses it and try again.";
        return false;
    }

    // A random state ties the answer to this request.
    std::mt19937_64 random((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string state = std::to_string(random());
    const std::string url = "https://sketchfab.com/oauth2/authorize/?response_type=code&client_id=" +
                            net::UrlEncode(credentials.clientId) + "&redirect_uri=" + net::UrlEncode(kRedirectUri) +
                            "&state=" + state;
    ::ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    std::string code;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    while (code.empty() && std::chrono::steady_clock::now() < deadline) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval wait{ 1, 0 };
        if (::select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
        SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        char request[8192] = {};
        const int got = ::recv(client, request, sizeof(request) - 1, 0);
        std::string line = got > 0 ? std::string(request, (size_t)got) : std::string();
        line = line.substr(0, line.find("\r\n"));
        const auto param = [&](const char* name) {
            const std::string key = std::string(name) + "=";
            size_t at = line.find("?" + key);
            if (at == std::string::npos) at = line.find("&" + key);
            if (at == std::string::npos) return std::string();
            at += key.size() + 1;
            const size_t end = line.find_first_of("& ", at);
            return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
        };
        const bool ours = line.find("/sketchfab/callback") != std::string::npos && param("state") == state;
        if (ours) code = param("code");
        const std::string page = ours && !code.empty()
            ? "<html><body style='font-family:sans-serif;background:#1e1f22;color:#ddd'><h2>FRS World Editor</h2>"
              "<p>Signed in. You can close this tab and go back to the editor.</p></body></html>"
            : "<html><body style='font-family:sans-serif'><p>Nothing to do here.</p></body></html>";
        const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                                  "Content-Length: " + std::to_string(page.size()) + "\r\n\r\n" + page;
        ::send(client, reply.data(), (int)reply.size(), 0);
        ::closesocket(client);
    }
    ::closesocket(listener);
    ::WSACleanup();
    if (code.empty()) {
        error = L"No answer from Sketchfab's login page within three minutes.";
        return false;
    }

    const std::string form = "grant_type=authorization_code&code=" + net::UrlEncode(code) +
                             "&client_id=" + net::UrlEncode(credentials.clientId) +
                             "&client_secret=" + net::UrlEncode(credentials.clientSecret) +
                             "&redirect_uri=" + net::UrlEncode(kRedirectUri);
    net::Response response;
    if (!net::Request(L"POST", "https://sketchfab.com/oauth2/token/",
                      { L"Content-Type: application/x-www-form-urlencoded" }, form, response, error))
        return false;
    if (response.status != 200) { error = ApiError(response); return false; }
    try {
        const json body = json::parse(response.body);
        credentials.token = Str(body, "access_token");
        credentials.oauth = true;
    } catch (...) {
        error = L"Sketchfab's token answer could not be read.";
        return false;
    }
    if (credentials.token.empty()) { error = L"Sketchfab gave no token."; return false; }
    std::string user;
    if (WhoAmI(credentials, user, error)) credentials.user = user;
    return true;
}

bool DownloadGlb(const Credentials& credentials, const std::string& uid, const std::wstring& path,
                 const std::function<bool(size_t, size_t)>& progress, std::wstring& error) {
    if (!credentials.Valid()) { error = L"Connect a Sketchfab account first."; return false; }
    net::Response response;
    if (!net::Get(std::string(kApi) + "/models/" + uid + "/download", AuthHeaders(credentials), response, error))
        return false;
    if (response.status != 200) {
        error = response.status == 401 ? L"The Sketchfab login has expired: connect again." : ApiError(response);
        return false;
    }
    std::string url;
    try {
        const json body = json::parse(response.body);
        if (body.contains("glb") && body["glb"].is_object()) url = Str(body["glb"], "url");
    } catch (...) {
    }
    if (url.empty()) { error = L"This model has no GLB to download."; return false; }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) { error = L"Cannot write " + path; return false; }
    net::Response data;
    const bool ok = net::Request(L"GET", url, {}, {}, data, error, file, progress);
    std::fclose(file);
    if (!ok || data.status != 200) {
        if (ok) error = L"The GLB download answered " + std::to_wstring(data.status) + L".";
        ::DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

std::string Credit(const Model& model) {
    return model.name + " by " + model.author + " (" + model.license + ") " + model.viewerUrl;
}

} // namespace sketchfab
