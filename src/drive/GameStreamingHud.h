#pragma once

// The test drive's "Game streaming" panel: what SPEED2.EXE would have loaded, drawn and collided with at the
// car's position (drive/world/GameStreaming, docs/STREAMING-STUDY.md), and the option to draw only that.
// It reports; the drive's collision still comes from every section (bb::BuildTrackWorld).

#include "drive/world/GameStreaming.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nfs { class World; }
class WorldRenderer;

namespace bb {

class Speed2Image;

class GameStreamingHud {
public:
    /// Builds the model of the open region as the editor will write it. False with `error` when the region has
    /// no streaming data; the panel then says so.
    bool Begin(nfs::World& world, const std::wstring& gameFolder, const Speed2Image* exe, std::wstring& error);
    /// Forgets the drive and removes the draw filter.
    void End(WorldRenderer* renderer);

    /// One step: car position and velocity (game space, m/s), camera position. collisionTree / collisionBoundary:
    /// what the test drive's own coordinate is bound to (everything loaded), for comparison; 0 when none.
    void Update(float carX, float carY, float velX, float velY, float camX, float camY, float dt,
                int collisionTree, int collisionBoundary);

    /// The ImGui panel (its own window at the right of the view). Installs or removes the renderer's draw filter
    /// when the checkbox changes.
    void Draw(float x, float y, float width, float height, WorldRenderer* renderer);

    bool Ready() const { return model_ != nullptr; }
    const StreamingState& State() const { return state_; }

private:
    void Warn(const std::string& text, float seconds = 6.0f);
    void InstallFilter(WorldRenderer* renderer);

    std::unique_ptr<GameStreamingModel> model_;
    GameStreamingModel::Tracker tracker_;
    StreamingState state_;
    std::string error_;
    float clock_ = 0.0f;

    int lastLoading_ = -1, lastDrawing_ = -1, lastCollision_ = -1;
    std::unordered_set<int> loaded_;               // the kept set, as the pool holds it
    std::unordered_map<int, float> loadedAt_;      // section -> clock when it entered the kept set
    int driveTree_ = 0, driveBoundary_ = 0;
    float carX_ = 0.0f, carY_ = 0.0f;

    struct Message { std::string text; float until; };
    std::vector<Message> messages_;
    int gaps_ = 0;

    static bool drawOnlyLoaded_;                   // kept between drives
    bool filterInstalled_ = false;
    WorldRenderer* filterOn_ = nullptr;
};

} // namespace bb
