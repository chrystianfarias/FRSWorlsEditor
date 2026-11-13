#pragma once

#include <functional>
#include <string>
#include <vector>

// Sketchfab (sketchfab.com) through its public Data API v3: searching the
// downloadable models, the signed-in account, and fetching a model's GLB.
// Every call blocks -- run them on a worker thread.
//
// Searching needs no account. Downloading does: either the person's own API
// token (sketchfab.com > Settings > Password & API), or a login through
// Sketchfab's OAuth page, which needs this program registered with Sketchfab
// (a client id and secret they hand out on request; set them under Online >
// Connect > Advanced). Credentials are kept in the user's profile, encrypted
// for that Windows user (DPAPI), never beside the game or in the repository.
namespace sketchfab {

struct Model {
    std::string uid, name, author, viewerUrl, thumbnailUrl;
    std::string license, licenseSlug;     // "CC Attribution", "by"
    int faces = 0, vertices = 0;          // of the model as made
    int glbFaces = 0, glbVertices = 0;    // of the GLB download -- what would be imported
    int textures = 0, textureMax = 0;
    size_t glbBytes = 0;
};

struct Query {
    std::string text;
    int maxVertices = 0;          // 0: no limit (applied here, after the search)
    std::string license;          // a Sketchfab licence slug, empty for any usable one
    std::string sort;             // "", "-likeCount", "-viewCount", "-publishedAt"
    std::string next;             // the cursor URL of the next page, from a previous search
};

// One page of downloadable models. `next` gets the cursor of the page after.
bool Search(const Query& query, std::vector<Model>& out, std::string& next, std::wstring& error);

// The JPEG of a model's thumbnail, as bytes.
bool FetchBytes(const std::string& url, std::string& bytes, std::wstring& error);

struct Credentials {
    std::string token;            // an API token or an OAuth access token
    bool oauth = false;           // Bearer (OAuth) rather than Token (API token)
    std::string user;             // the display name it belongs to
    std::string clientId, clientSecret;   // the registration, for the OAuth login
    bool Valid() const { return !token.empty(); }
};

bool LoadCredentials(Credentials& out);
bool SaveCredentials(const Credentials& credentials);

// Asks Sketchfab whose token it is. False when it is not accepted.
bool WhoAmI(const Credentials& credentials, std::string& user, std::wstring& error);

// Opens Sketchfab's login page in the browser and waits (up to three minutes)
// for it to come back to http://127.0.0.1:52890/sketchfab/callback, then
// trades the code for a token. Needs the registration in `credentials`.
bool OAuthLogin(Credentials& credentials, std::wstring& error);
constexpr const char* kRedirectUri = "http://127.0.0.1:52890/sketchfab/callback";

// Downloads the model's GLB to `path`. `progress` gets bytes so far and the
// total; false from it cancels.
bool DownloadGlb(const Credentials& credentials, const std::string& uid, const std::wstring& path,
                 const std::function<bool(size_t, size_t)>& progress, std::wstring& error);

// What a credits line says: "Name by Author (CC Attribution) https://...".
std::string Credit(const Model& model);

} // namespace sketchfab
