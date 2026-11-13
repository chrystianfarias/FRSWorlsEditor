#include "ui/OnlineLibrary.h"

#include "core/Log.h"
#include "io/GlbToObj.h"
#include "ui/ImGuiLayer.h"

#include "imgui.h"

#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

struct LicenseChoice { const char* label; const char* slug; };
const LicenseChoice kLicenses[] = {
    { "Any usable licence", "" }, { "CC0 (public domain)", "cc0" }, { "CC Attribution", "by" },
    { "CC Attribution-ShareAlike", "by-sa" }, { "CC Attribution-NonCommercial", "by-nc" },
    { "CC Attribution-NonCommercial-ShareAlike", "by-nc-sa" },
};
struct SortChoice { const char* label; const char* key; };
const SortChoice kSorts[] = {
    { "Relevance", "" }, { "Most liked", "-likeCount" }, { "Most viewed", "-viewCount" }, { "Newest", "-publishedAt" },
};

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::string Count(int n) {
    char text[32];
    if (n >= 1000000) std::snprintf(text, sizeof(text), "%.1fM", n / 1e6);
    else if (n >= 1000) std::snprintf(text, sizeof(text), "%.1fk", n / 1e3);
    else std::snprintf(text, sizeof(text), "%d", n);
    return text;
}

// A JPEG or PNG in memory, as BGRA, through the system's WIC.
bool Decode(const std::string& bytes, std::vector<unsigned>& bgra, int& width, int& height) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        bgra.assign((size_t)w * h, 0);
        ok = w && h && SUCCEEDED(converter->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)bgra.data()));
        width = (int)w;
        height = (int)h;
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

std::wstring CacheFolder(const std::string& uid) {
    wchar_t base[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L"";
    std::wstring folder = std::wstring(base) + L"\\FRSWorldEditor";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\sketchfab";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\" + std::wstring(uid.begin(), uid.end());
    ::CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

// "Low Poly Street lamp!" -> "LOW_POLY_STREET_LAMP": the new model is FRS_ and this.
std::string ObjectName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out += (char)std::toupper(c);
        else if (!out.empty() && out.back() != '_') out += '_';
        if (out.size() >= 20) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("SKETCHFAB") : out;
}

} // namespace

OnlineLibrary::OnlineLibrary() {
    sketchfab::Credentials stored;
    if (sketchfab::LoadCredentials(stored)) {
        credentials_ = stored;
        std::snprintf(clientId_, sizeof(clientId_), "%s", stored.clientId.c_str());
        std::snprintf(clientSecret_, sizeof(clientSecret_), "%s", stored.clientSecret.c_str());
    }
    worker_ = std::thread([this] { Worker(); });
}

OnlineLibrary::~OnlineLibrary() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    ForgetPictures();
}

void OnlineLibrary::Push(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Pictures wait behind everything a person asked for.
        if (job.kind == Job::Thumbnail) jobs_.push_back(std::move(job));
        else jobs_.push_front(std::move(job));
    }
    wake_.notify_one();
}

void OnlineLibrary::SetStatus(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = text;
}

bool OnlineLibrary::TakeReady(std::wstring& objPath, std::string& credit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (readyObj_.empty()) return false;
    objPath = readyObj_;
    credit = readyCredit_;
    readyObj_.clear();
    return true;
}

void OnlineLibrary::Worker() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::wstring error;
        switch (job.kind) {
            case Job::Search: {
                std::vector<sketchfab::Model> found;
                std::string next;
                const bool ok = sketchfab::Search(job.query, found, next, error);
                std::lock_guard<std::mutex> lock(mutex_);
                searching_ = false;
                if (!ok) { status_ = L"Search failed: " + error; break; }
                if (!job.append) results_.clear();
                results_.insert(results_.end(), found.begin(), found.end());
                next_ = next;
                status_ = std::to_wstring(results_.size()) + L" models" + (next.empty() ? L"" : L" so far");
                break;
            }
            case Job::Thumbnail: {
                std::string bytes;
                Picture picture;
                if (sketchfab::FetchBytes(job.model.thumbnailUrl, bytes, error) &&
                    Decode(bytes, picture.bgra, picture.width, picture.height)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    decoded_[job.model.uid] = std::move(picture);
                }
                break;
            }
            case Job::Download: {
                sketchfab::Credentials credentials;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    credentials = credentials_;
                    downloading_ = job.model.uid;
                    progress_ = 0.0f;
                    status_ = L"Downloading " + std::wstring(job.model.name.begin(), job.model.name.end()) + L"...";
                }
                const std::wstring folder = CacheFolder(job.model.uid);
                const std::wstring glb = folder + L"\\model.glb";
                bool ok = ::GetFileAttributesW(glb.c_str()) != INVALID_FILE_ATTRIBUTES ||
                          sketchfab::DownloadGlb(credentials, job.model.uid, glb,
                                                 [this](size_t got, size_t total) {
                                                     std::lock_guard<std::mutex> lock(mutex_);
                                                     progress_ = total ? (float)got / (float)total : 0.5f;
                                                     return !stop_;
                                                 },
                                                 error);
                GlbConversion conversion;
                const std::string object = ObjectName(job.model.name);
                const std::wstring obj = folder + L"\\" + std::wstring(object.begin(), object.end()) + L".obj";
                if (ok) {
                    ok = ConvertGlbToObj(glb, obj, object, 4.0f, conversion);
                    if (!ok) error = conversion.error;
                    else if (conversion.vertices > 65000) {
                        ok = false;
                        error = L"It has " + std::to_wstring(conversion.vertices) +
                                L" vertices; the game takes at most 65 000 in one model.";
                    }
                }
                std::lock_guard<std::mutex> lock(mutex_);
                progress_ = -1.0f;
                downloading_.clear();
                if (!ok) { status_ = L"Could not bring it in: " + error; break; }
                readyObj_ = obj;
                readyCredit_ = sketchfab::Credit(job.model);
                status_ = L"Placing " + std::wstring(job.model.name.begin(), job.model.name.end()) + L" (" +
                          std::to_wstring(conversion.vertices) + L" vertices, " +
                          std::to_wstring(conversion.textures) + L" textures)";
                if (window_) ::PostMessageW(window_, kPlaceMessage, 0, 0);
                break;
            }
            case Job::Login:
            case Job::CheckToken: {
                sketchfab::Credentials credentials = job.credentials;
                std::string user;
                const bool ok = job.kind == Job::Login ? sketchfab::OAuthLogin(credentials, error)
                                                       : sketchfab::WhoAmI(credentials, user, error);
                if (ok && job.kind == Job::CheckToken) credentials.user = user;
                std::lock_guard<std::mutex> lock(mutex_);
                loggingIn_ = false;
                if (!ok) { loginStatus_ = error; break; }
                credentials_ = credentials;
                sketchfab::SaveCredentials(credentials_);
                loginStatus_ = L"Connected as " + std::wstring(credentials.user.begin(), credentials.user.end());
                LOG_INFO("Sketchfab: connected as %s", credentials.user.c_str());
                break;
            }
        }
    }
    ::CoUninitialize();
}

void OnlineLibrary::StartSearch(bool append) {
    Job job;
    job.kind = Job::Search;
    job.query.text = search_;
    job.query.maxVertices = maxVertices_;
    job.query.license = kLicenses[license_].slug;
    job.query.sort = kSorts[sort_].key;
    job.append = append;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (append) job.query.next = next_;
        searching_ = true;
        status_ = L"Searching...";
        if (!append) selected_ = -1;
    }
    Push(std::move(job));
}

void OnlineLibrary::ForgetPictures() {
    for (auto& [uid, texture] : textures_)
        if (texture) ((IDirect3DTexture9*)texture)->Release();
    textures_.clear();
    requested_.clear();
}

void* OnlineLibrary::Texture(IDirect3DDevice9* device, const std::string& uid) {
    const auto found = textures_.find(uid);
    if (found != textures_.end()) return found->second;
    Picture picture;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto decoded = decoded_.find(uid);
        if (decoded == decoded_.end()) return nullptr;
        picture = std::move(decoded->second);
        decoded_.erase(decoded);
    }
    IDirect3DTexture9* texture = nullptr;
    if (device && SUCCEEDED(device->CreateTexture((UINT)picture.width, (UINT)picture.height, 1, 0, D3DFMT_A8R8G8B8,
                                                  D3DPOOL_MANAGED, &texture, nullptr))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
            for (int y = 0; y < picture.height; ++y)
                std::memcpy((unsigned char*)locked.pBits + (size_t)y * locked.Pitch,
                            picture.bgra.data() + (size_t)y * picture.width, (size_t)picture.width * 4);
            texture->UnlockRect(0);
        }
    }
    textures_[uid] = texture;
    return texture;
}

void OnlineLibrary::DrawConnect() {
    if (openConnect_) { ImGui::OpenPopup("Connect a Sketchfab account"); openConnect_ = false; }
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Connect a Sketchfab account", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    bool busy;
    std::wstring status;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        busy = loggingIn_;
        status = loginStatus_;
    }
    ImGui::TextWrapped("Searching needs no account. Downloading a model does: Sketchfab hands its files only to a "
                       "signed-in person, who then owes the author the credit the licence asks for.");
    ImGui::Spacing();

    ImGui::SeparatorText(ICON_PH_SIGN_IN "  Log in with Sketchfab");
    const bool registered = clientId_[0] != 0;
    ImGui::BeginDisabled(busy || !registered);
    if (ImGui::Button(ICON_PH_GLOBE "  Open Sketchfab's login page")) {
        Job job;
        job.kind = Job::Login;
        job.credentials.clientId = clientId_;
        job.credentials.clientSecret = clientSecret_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loggingIn_ = true;
            loginStatus_ = L"Waiting for the browser...";
        }
        Push(std::move(job));
    }
    ImGui::EndDisabled();
    if (!registered)
        ImGui::TextDisabled("Needs the editor's registration with Sketchfab (Advanced, below).");

    ImGui::SeparatorText(ICON_PH_KEY "  Or use your API token");
    if (ImGui::Button(ICON_PH_ARROW_SQUARE_OUT "  Open my token page"))
        ::ShellExecuteW(nullptr, L"open", L"https://sketchfab.com/settings/password", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    ImGui::TextDisabled("(Settings > Password & API)");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##token", "Paste the API token here", tokenInput_, sizeof(tokenInput_),
                             ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !tokenInput_[0]);
    if (ImGui::Button("Connect", ImVec2(-FLT_MIN, 0))) {
        Job job;
        job.kind = Job::CheckToken;
        job.credentials.token = tokenInput_;
        job.credentials.oauth = false;
        job.credentials.clientId = clientId_;
        job.credentials.clientSecret = clientSecret_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loggingIn_ = true;
            loginStatus_ = L"Checking the token...";
        }
        std::memset(tokenInput_, 0, sizeof(tokenInput_));
        Push(std::move(job));
    }
    ImGui::EndDisabled();

    if (ImGui::CollapsingHeader("Advanced: the editor's registration with Sketchfab")) {
        ImGui::TextWrapped("Sketchfab's login needs a client id and secret it gives to a registered program. "
                           "Register this redirect address with them:");
        ImGui::TextDisabled("%s", sketchfab::kRedirectUri);
        ImGui::InputText("Client id", clientId_, sizeof(clientId_));
        ImGui::InputText("Client secret", clientSecret_, sizeof(clientSecret_), ImGuiInputTextFlags_Password);
        if (ImGui::Button("Keep these")) {
            std::lock_guard<std::mutex> lock(mutex_);
            credentials_.clientId = clientId_;
            credentials_.clientSecret = clientSecret_;
            sketchfab::SaveCredentials(credentials_);
        }
    }

    if (!status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Utf8(status).c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void OnlineLibrary::DrawCard(size_t index, IDirect3DDevice9* device, float width) {
    sketchfab::Model model;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= results_.size()) return;
        model = results_[index];
    }
    if (!requested_[model.uid] && !model.thumbnailUrl.empty()) {
        requested_[model.uid] = true;
        Job job;
        job.kind = Job::Thumbnail;
        job.model = model;
        Push(std::move(job));
    }
    ImGui::PushID((int)index);
    ImGui::BeginGroup();
    const ImVec2 at = ImGui::GetCursorPos();
    const float pictureHeight = width * 9.0f / 16.0f;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::Selectable("##card", selected_ == (int)index, 0, ImVec2(width, pictureHeight + line * 2 + 8)))
        selected_ = (int)index;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nby %s\n%s", model.name.c_str(), model.author.c_str(), model.license.c_str());
    ImGui::SetCursorPos(at);
    if (void* texture = Texture(device, model.uid)) ImGui::Image((ImTextureID)texture, ImVec2(width, pictureHeight));
    else ImGui::Dummy(ImVec2(width, pictureHeight));
    const ImVec2 text = ImGui::GetCursorScreenPos();
    ImGui::PushClipRect(text, ImVec2(text.x + width, text.y + line * 2), true);
    ImGui::TextUnformatted(model.name.c_str());
    const int vertices = model.glbVertices ? model.glbVertices : model.vertices;
    ImGui::TextDisabled("%s verts  %d tex", Count(vertices).c_str(), model.textures);
    ImGui::PopClipRect();
    ImGui::EndGroup();
    ImGui::PopID();
}

void OnlineLibrary::Draw(IDirect3DDevice9* device) {
    sketchfab::Credentials credentials;
    bool searching;
    std::wstring status;
    float progress;
    std::string next;
    size_t count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        credentials = credentials_;
        searching = searching_;
        status = status_;
        progress = progress_;
        next = next_;
        count = results_.size();
    }

    // --- account ---------------------------------------------------------------
    if (credentials.Valid()) {
        ImGui::TextDisabled(ICON_PH_USER_CIRCLE);
        ImGui::SameLine();
        ImGui::Text("%s", credentials.user.empty() ? "Connected" : credentials.user.c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 80);
        if (ImGui::SmallButton("Disconnect")) {
            std::lock_guard<std::mutex> lock(mutex_);
            credentials_.token.clear();
            credentials_.user.clear();
            sketchfab::SaveCredentials(credentials_);
        }
    } else {
        ImGui::TextDisabled(ICON_PH_USER_CIRCLE "  Not connected");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 80);
        if (ImGui::SmallButton("Connect...")) openConnect_ = true;
    }
    DrawConnect();

    // --- search ------------------------------------------------------------------
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##q", ICON_PH_MAGNIFYING_GLASS "  Search Sketchfab", search_,
                                                sizeof(search_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(ICON_PH_ARROW_RIGHT, ImVec2(ImGui::GetFrameHeight(), 0)) || enter) && !searching)
        StartSearch(false);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::DragInt("##max", &maxVertices_, 50.0f, 0, 65000, maxVertices_ ? "max %d verts" : "any size");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vertices of the downloaded model (0: any). The game takes 65 000.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##sort", &sort_, [](void*, int i) { return kSorts[i].label; }, nullptr, (int)std::size(kSorts));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##license", &license_, [](void*, int i) { return kLicenses[i].label; }, nullptr,
                 (int)std::size(kLicenses));
    if (!status.empty()) ImGui::TextDisabled("%s", Utf8(status).c_str());
    if (progress >= 0.0f) ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0));

    // --- the selected model ----------------------------------------------------------
    const float detail = selected_ >= 0 ? ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 2 + 8 : 0.0f;

    // --- results ----------------------------------------------------------------------
    if (ImGui::BeginChild("##results", ImVec2(0, -detail))) {
        const float cell = 150.0f;
        const int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + 8)));
        const float width = ImGui::GetContentRegionAvail().x / columns - 8;
        for (size_t i = 0; i < count; ++i) {
            if (i % (size_t)columns) ImGui::SameLine();
            DrawCard(i, device, width);
        }
        if (!next.empty() && !searching && ImGui::Button("Load more", ImVec2(-FLT_MIN, 0))) StartSearch(true);
        if (count == 0 && !searching && status.empty())
            ImGui::TextDisabled("Search for a model: a bench, a sign, a car...");
    }
    ImGui::EndChild();

    if (selected_ >= 0) {
        sketchfab::Model model;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ((size_t)selected_ < results_.size()) model = results_[(size_t)selected_];
        }
        if (!model.uid.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(model.name.c_str());
            const int vertices = model.glbVertices ? model.glbVertices : model.vertices;
            ImGui::TextDisabled("by %s  -  %s  -  %s vertices, %d textures (%d px)", model.author.c_str(),
                                model.license.c_str(), Count(vertices).c_str(), model.textures, model.textureMax);
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::BeginDisabled(!credentials.Valid() || progress >= 0.0f || vertices > 65000);
            if (ImGui::Button(ICON_PH_DOWNLOAD_SIMPLE "  Place at the view centre", ImVec2(half, 0))) {
                Job job;
                job.kind = Job::Download;
                job.model = model;
                Push(std::move(job));
            }
            ImGui::EndDisabled();
            if (!credentials.Valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Connect a Sketchfab account first (above).");
            ImGui::SameLine();
            if (ImGui::Button(ICON_PH_ARROW_SQUARE_OUT "  Open on Sketchfab", ImVec2(-FLT_MIN, 0)))
                ::ShellExecuteA(nullptr, "open", model.viewerUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}
