#include "drive/GameStreamingHud.h"

#include "core/Log.h"
#include "nfs/CityZones.h"
#include "nfs/FreeRoam.h"
#include "nfs/World.h"
#include "render/WorldRenderer.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace bb {

namespace {

const ImVec4 kRed(1.0f, 0.38f, 0.32f, 1.0f);
const ImVec4 kAmber(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 kGreen(0.45f, 0.9f, 0.5f, 1.0f);
const ImVec4 kBlue(0.3f, 0.69f, 0.9f, 1.0f);
constexpr double kMiB = 1048576.0;

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? (char)c : '?';
    return s;
}

std::string Name(int number) { return StreamSectionName(number); }

// What the renderer's filter reads; shared so a filter left installed never points at a dead panel.
struct DrawSets {
    std::vector<char> sections;
    std::unordered_set<uint32_t> models;
};

std::shared_ptr<DrawSets>& Sets()
{
    static std::shared_ptr<DrawSets> sets = std::make_shared<DrawSets>();
    return sets;
}

std::vector<nfs::CityZone> g_cityZones;

} // namespace

bool GameStreamingHud::drawOnlyLoaded_ = false;

bool GameStreamingHud::Begin(nfs::World& world, const std::wstring& gameFolder, const Speed2Image* exe,
                             std::wstring& error)
{
    model_.reset();
    error_.clear();
    messages_.clear();
    gaps_ = 0;
    clock_ = 0.0f;
    lastLoading_ = lastDrawing_ = lastCollision_ = -1;
    loaded_.clear();
    loadedAt_.clear();
    state_ = StreamingState{};
    model_ = GameStreamingModel::FromWorld(world, gameFolder, exe, error);
    if (!model_) {
        error_ = Narrow(error);
        return false;
    }
    tracker_ = model_->NewTracker();
    std::vector<nfs::FreeRoamZone> freeRoam;
    g_cityZones.clear();
    if (nfs::ReadFreeRoamZones(gameFolder, world.Region(), freeRoam)) g_cityZones = nfs::ReadCityZones(freeRoam);
    LOG_INFO("game streaming: %zu sections, %zu zone lists, %zu CC groups, %zu prediction zones, %zu barriers, "
             "pool %.1f MiB",
             model_->Sections().size(), model_->Lists().size(), model_->Groups().size(),
             model_->PredictionZones().size(), model_->Barriers().size(), model_->PoolBytes() / kMiB);
    return true;
}

void GameStreamingHud::End(WorldRenderer* renderer)
{
    if (renderer && filterInstalled_) renderer->SetGameDrawFilter({}, {});
    filterInstalled_ = false;
    filterOn_ = nullptr;
    model_.reset();
    tracker_ = GameStreamingModel::Tracker{};
}

void GameStreamingHud::Warn(const std::string& text, float seconds)
{
    messages_.push_back({ text, clock_ + seconds });
    if (messages_.size() > 6) messages_.erase(messages_.begin());
}

void GameStreamingHud::Update(float carX, float carY, float velX, float velY, float camX, float camY, float dt,
                              int collisionTree, int collisionBoundary)
{
    if (!model_) return;
    clock_ += dt;
    driveTree_ = collisionTree;
    driveBoundary_ = collisionBoundary;
    carX_ = carX;
    carY_ = carY;

    const LoadingZoneResult loading = model_->LoadingZone(*tracker_.Loading, carX, carY, velX, velY);
    const int drawing = GameStreamingModel::DrawingZone(*tracker_.Drawing, camX, camY);
    const int collision = GameStreamingModel::CollisionZone(*tracker_.Collision, tracker_.Coordinate, carX, carY);
    state_.Loading = loading;
    state_.DrawingZone = drawing;
    state_.CollisionZone = collision;

    // Crossing into a collision zone: was its pack among what the pool held a step ago? Without a prediction
    // the loading zone switches on this same step, so the pack only starts its read now: the game has no
    // ground or walls here until it is read and activated.
    if (collision != lastCollision_ && lastCollision_ >= 0 && collision > 0) {
        std::vector<int16_t> before(loaded_.begin(), loaded_.end());
        const CollisionPackStatus held = model_->CollisionPackFor(collision, before);
        char text[200];
        if (held.Providers.empty()) {
            ++gaps_;
            std::snprintf(text, sizeof text,
                          "Crossing gap: %s's pack was not preloaded by %s - no collision in the game until it is "
                          "read and activated",
                          Name(collision).c_str(), lastCollision_ > 0 ? Name(lastCollision_).c_str() : "zone 0");
            Warn(text, 8.0f);
            LOG_WARN("game streaming: %s", text);
        } else {
            const auto at = loadedAt_.find(held.Providers.back().Section);
            const float since = at != loadedAt_.end() ? clock_ - at->second : 0.0f;
            if (since < 30.0f) {
                std::snprintf(text, sizeof text, "Entered %s: its pack %s was loading for %.1f s (prediction)",
                              Name(collision).c_str(), Name(held.Providers.back().Section).c_str(), since);
                Warn(text, 4.0f);
            }
        }
    }

    const bool loadingChanged = loading.Zone != lastLoading_;
    if (loadingChanged && lastLoading_ > 0 && loading.Zone > 0) {
        const int geometry = loading.Zone + model_->Base();
        if (!loaded_.count(geometry)) {
            char text[160];
            std::snprintf(text, sizeof text, "%s's section %s was not preloaded by %s: its read starts now",
                          Name(loading.Zone).c_str(), Name(geometry).c_str(), Name(lastLoading_).c_str());
            Warn(text, 5.0f);
        }
    }
    if (loadingChanged) {
        state_.Wanted = model_->WantedSet(loading.Zone, 0, &state_.Group);
        // Sections the pool already holds count at their permanent size [I], new ones at their full size.
        state_.Fit = model_->PoolFit(state_.Wanted, SizeModel::Full, &loaded_);
        std::unordered_set<int> kept(state_.Fit.Kept.begin(), state_.Fit.Kept.end());
        for (auto it = loadedAt_.begin(); it != loadedAt_.end();)
            it = kept.count(it->first) ? std::next(it) : loadedAt_.erase(it);
        for (int n : kept) loadedAt_.emplace(n, clock_);
        loaded_ = std::move(kept);
        Sets()->models = model_->LoadedModels(state_.Fit.Kept);
        LOG_INFO("game streaming: loading zone %s%s, %zu wanted (%.1f MiB), %zu dropped",
                 Name(loading.Zone).c_str(), loading.Predicted ? " (predicted)" : "", state_.Wanted.size(),
                 state_.Fit.WantedBytes / kMiB, state_.Fit.Dropped.size());
        if (loading.Zone == 0) Warn("Outside every outline: the game unloads the whole city here", 8.0f);
    }
    if (loadingChanged || drawing != lastDrawing_) {
        state_.Drawn = model_->DrawSet(drawing, state_.Fit.Kept);
        std::vector<char>& marks = Sets()->sections;
        marks.assign(2800, 0);
        for (int16_t n : state_.Drawn)
            if (n >= 0 && n < (int)marks.size()) marks[n] = 1;
    }
    if (loadingChanged || collision != lastCollision_)
        state_.Collision = model_->CollisionPackFor(collision, state_.Fit.Kept);

    lastLoading_ = loading.Zone;
    lastDrawing_ = drawing;
    lastCollision_ = collision;
}

void GameStreamingHud::InstallFilter(WorldRenderer* renderer)
{
    if (!renderer) return;
    const bool want = drawOnlyLoaded_ && model_ != nullptr;
    if (want == filterInstalled_ && renderer == filterOn_) return;
    if (want) {
        std::shared_ptr<DrawSets> sets = Sets();
        renderer->SetGameDrawFilter(
            [sets](u32 number) { return number < sets->sections.size() && sets->sections[number] != 0; },
            [sets](u32 hash) { return sets->models.count(hash) != 0; });
    } else {
        renderer->SetGameDrawFilter({}, {});
    }
    filterInstalled_ = want;
    filterOn_ = renderer;
}

void GameStreamingHud::Draw(float x, float y, float width, float height, WorldRenderer* renderer)
{
    (void)height;
    ImGui::SetNextWindowPos(ImVec2(x + width - 12.0f, y + 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##gamestreaming", nullptr, flags)) {
        ImGui::TextColored(kBlue, "Game streaming");
        ImGui::SameLine();
        ImGui::TextDisabled("(SPEED2.EXE rules)");
        if (!model_) {
            ImGui::TextColored(kAmber, "%s", error_.empty() ? "Not available." : error_.c_str());
            ImGui::End();
            return;
        }
        const LoadingZoneResult& l = state_.Loading;
        const char* none = "none (0)";
        ImGui::Text("Loading zone   %s", l.Zone > 0 ? Name(l.Zone).c_str() : none);
        if (l.Predicted) {
            ImGui::SameLine();
            ImGui::TextColored(kGreen, "predicted (car in %s)", Name(l.ZoneAtCar).c_str());
        } else if (l.BarrierCancelled) {
            ImGui::SameLine();
            ImGui::TextColored(kAmber, "prediction cancelled by a barrier");
        } else if (l.PredictionZone >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("in prediction zone %d", l.PredictionZone);
        }
        ImGui::Text("Drawing zone   %s", state_.DrawingZone > 0 ? Name(state_.DrawingZone).c_str() : none);
        ImGui::SameLine();
        ImGui::TextDisabled("(camera)");
        ImGui::Text("Collision zone %s", state_.CollisionZone > 0 ? Name(state_.CollisionZone).c_str() : none);
        const CollisionPackStatus& c = state_.Collision;
        if (state_.CollisionZone > 0) {
            ImGui::SameLine();
            if (c.Providers.empty()) ImGui::TextColored(kRed, "no pack loaded");
            else ImGui::TextDisabled("pack %s", Name(c.Providers.back().Section).c_str());
        }
        ImGui::TextDisabled("test drive (all loaded): tree %s, outline %s",
                            driveTree_ > 0 ? Name(driveTree_).c_str() : "none",
                            driveBoundary_ > 0 ? Name(driveBoundary_).c_str() : "none");
        // The neighbourhood is only a name for orientation; it plays no part in streaming.
        const int city = nfs::ZoneAt(g_cityZones, carX_, carY_);
        if (city >= 0) ImGui::TextDisabled("in %s", Narrow(g_cityZones[city].name).c_str());
        if (state_.Group) ImGui::TextDisabled("CC group %s (%zu zones)", state_.Group->Name.c_str(),
                                              state_.Group->Zones.size());

        const PoolFitResult& fit = state_.Fit;
        const float used = fit.PoolBytes ? (float)((double)fit.KeptBytes / (double)fit.PoolBytes) : 0.0f;
        char overlay[96];
        std::snprintf(overlay, sizeof overlay, "%.1f / %.1f MiB", fit.KeptBytes / kMiB, fit.PoolBytes / kMiB);
        ImGui::Text("Wanted %zu sections", state_.Wanted.size());
        if (!fit.Dropped.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kRed, "%zu dropped", fit.Dropped.size());
        }
        ImGui::ProgressBar(std::min(used, 1.0f), ImVec2(260.0f, 0.0f), overlay);
        ImGui::TextDisabled("full %.1f MiB, permanent %.1f MiB; drawn %zu",
                            model_->TotalSize(state_.Wanted, SizeModel::Full) / kMiB,
                            model_->TotalSize(state_.Wanted, SizeModel::Permanent) / kMiB, state_.Drawn.size());

        // Standing warnings.
        if (l.Zone == 0) ImGui::TextColored(kRed, "Outside every outline - the game unloads everything here");
        else if (!model_->ListOf(l.Zone)) ImGui::TextColored(kAmber, "Zone %s has no section list", Name(l.Zone).c_str());
        if (state_.DrawingZone == 0)
            ImGui::TextColored(kAmber, "Camera outside every outline: only sections n%%100 < 10 are drawn");
        if (state_.CollisionZone == 0)
            ImGui::TextColored(kRed, "No collision zone: no ground or walls in the game");
        else if (c.Missing())
            ImGui::TextColored(kRed, "Collision pack for %s missing from the loaded set", Name(state_.CollisionZone).c_str());
        else if (c.Conflict()) {
            std::string list;
            for (const CollisionPackRef& p : c.Providers) list += " " + Name(p.Section) + "(" + Name(p.Header) + ")";
            ImGui::TextColored(kRed, "Collision conflict on %s:%s - last loaded wins", Name(state_.CollisionZone).c_str(),
                               list.c_str());
        }
        if (!c.Aliases.empty()) {
            std::string list;
            for (const CollisionPackRef& p : c.Aliases) list += " " + Name(p.Section) + "(" + Name(p.Header) + ")";
            ImGui::TextColored(kAmber, "Packs with aliasing headers loaded:%s", list.c_str());
        }
        if (gaps_ > 0) ImGui::TextColored(kAmber, "Crossing gaps this drive: %d", gaps_);
        for (const Message& m : messages_)
            if (m.until > clock_) ImGui::TextColored(kAmber, "%s", m.text.c_str());

        ImGui::Checkbox("Draw only what the game has loaded", &drawOnlyLoaded_);
        InstallFilter(renderer);

        if (ImGui::TreeNode("##sections", "Sections (%zu)", state_.Wanted.size())) {
            const std::unordered_set<int16_t> dropped(fit.Dropped.begin(), fit.Dropped.end());
            const std::unordered_set<int16_t> drawn(state_.Drawn.begin(), state_.Drawn.end());
            int column = 0;
            for (int16_t n : state_.Wanted) {
                const StreamSection* s = model_->Section(n);
                char text[48];
                std::snprintf(text, sizeof text, "%-4s %5.1f", Name(n).c_str(), s ? s->FullSize / kMiB : 0.0);
                if (column++ % 3) ImGui::SameLine(0.0f, 14.0f);
                if (dropped.count(n)) ImGui::TextColored(kRed, "%s", text);
                else if (drawn.count(n)) ImGui::TextColored(kGreen, "%s", text);
                else ImGui::TextDisabled("%s", text);
            }
            ImGui::TextDisabled("MiB at full size; green drawn, grey loaded only, red dropped");
            ImGui::TreePop();
        }
    }
    ImGui::End();
}

} // namespace bb
