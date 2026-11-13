#pragma once

#include "online/Sketchfab.h"

#include <windows.h>
#include <d3d9.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The Explorer's Online tab: free models from Sketchfab, searched by name,
// held to a vertex budget and a usable licence, shown with their pictures,
// and placed in the world in one click -- downloaded as GLB, turned into an
// OBJ (io/GlbToObj.h) and brought in as a new object at the view centre, its
// author credited in the models folder's CREDITS.txt.
//
// The network runs on a thread of its own; the panel only draws what it has.
class OnlineLibrary {
public:
    OnlineLibrary();
    ~OnlineLibrary();

    // `window` is told kPlaceMessage when a model is ready to place.
    void SetWindow(HWND window) { window_ = window; }
    static constexpr UINT kPlaceMessage = WM_APP + 0x53;

    void Draw(IDirect3DDevice9* device);

    // The model to place, after kPlaceMessage: its OBJ and the credit line.
    bool TakeReady(std::wstring& objPath, std::string& credit);
    void SetStatus(const std::wstring& text);

    // Drops the pictures (their textures live in the device).
    void ForgetPictures();

    // Starts a search for `text`, as typed and entered.
    void Search(const char* text) {
        std::snprintf(search_, sizeof(search_), "%s", text);
        StartSearch(false);
    }

private:
    struct Job {
        enum Kind { Search, Thumbnail, Download, Login, CheckToken } kind = Search;
        sketchfab::Query query;
        sketchfab::Model model;
        sketchfab::Credentials credentials;
        bool append = false;
    };
    struct Picture { std::vector<unsigned> bgra; int width = 0, height = 0; };

    void Worker();
    void Push(Job job);
    void DrawConnect();
    void DrawCard(size_t index, IDirect3DDevice9* device, float width);
    void* Texture(IDirect3DDevice9* device, const std::string& uid);
    void StartSearch(bool append);

    HWND window_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stop_ = false;

    // --- shared with the worker, under mutex_ --------------------------------
    std::vector<sketchfab::Model> results_;
    std::string next_;
    bool searching_ = false;
    std::wstring status_;
    std::unordered_map<std::string, Picture> decoded_;     // uid -> pixels, not yet a texture
    std::wstring readyObj_;
    std::string readyCredit_;
    float progress_ = -1.0f;                               // a download, 0..1; -1 none
    std::string downloading_;
    sketchfab::Credentials credentials_;
    bool loggingIn_ = false;
    std::wstring loginStatus_;

    // --- the UI's own ------------------------------------------------------------
    std::unordered_map<std::string, void*> textures_;      // uid -> IDirect3DTexture9*
    std::unordered_map<std::string, bool> requested_;
    char search_[128] = {};
    int  maxVertices_ = 5000;
    int  license_ = 0;
    int  sort_ = 0;
    int  selected_ = -1;
    char tokenInput_[128] = {};
    char clientId_[128] = {}, clientSecret_[128] = {};
    bool openConnect_ = false;
};
