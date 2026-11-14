#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string>
#include <vector>

namespace nfs {

// The world's moving scenery: garage doors, the planes over the airport, tower
// cranes, searchlights, the trams. The layout file (L4Rx.BUN) holds
//   0x00037080  entities: one part of an animated tree each -- its model, its
//               parent, its matrix at rest and the animation that moves it;
//   0x00037090  instances: one placement of a tree in the world, with speed
//               and delays (one door tree is placed at several garages);
//   0x00E34010  EAGL animation banks: little ELF objects with the tracks
//               "<animation>_t", "_q" and "_s" (translation, rotation, scale).
// Each part's matrix is its pose relative to its parent, and the tree's root
// takes the instance's matrix (row vectors, as everywhere in the game).
struct WorldAnimationPart {
    u32  name = 0;           // hash of the part
    u32  model = 0;          // solid hash, 0 for the tree's root
    int  parent = -1;        // index in the tree's parts
    Mat4 rest;               // relative to the parent
    std::string animation;   // empty when it does not move

    // Decoded tracks, one value per frame; empty when the track is absent.
    std::vector<Vec3> translation;
    std::vector<Vec4> rotation;   // x, y, z, w
    std::vector<Vec3> scale;
    int   frames = 0;
    float fps = 30.0f;
};

struct WorldAnimationTree {
    u32 hash = 0;
    std::vector<WorldAnimationPart> parts;   // parents before children
};

struct WorldAnimationPlacement {
    size_t tree = 0;
    int    section = -1;
    Mat4   matrix;
    float  speed = 1.0f;
    float  loopDelay = 0.0f;   // seconds held at the end before it starts again
    u32    key = 0;            // +0x20: what it answers to -- a garage door, its shop's zone hash
};

class WorldAnimations {
public:
    // Reads every animated tree of a region's layout file.
    void Parse(ByteSpan layout);
    void Clear() { trees_.clear(); placements_.clear(); shipped_ = 0; }

    // Placements of the editor's own after the region's -- shown, never
    // written: a preview of what an extension is about to put in the game.
    // Each call replaces the last set.
    void SetExtraPlacements(std::vector<WorldAnimationPlacement> extra);

    const std::vector<WorldAnimationTree>& Trees() const { return trees_; }
    const std::vector<WorldAnimationPlacement>& Placements() const { return placements_; }

    // World matrices of a placement's parts at `seconds` (looping).
    void Pose(const WorldAnimationPlacement& placement, double seconds, std::vector<Mat4>& out) const;

private:
    std::vector<WorldAnimationTree>      trees_;
    std::vector<WorldAnimationPlacement> placements_;
    size_t shipped_ = 0;   // the region's own, before any extra ones
};

} // namespace nfs
