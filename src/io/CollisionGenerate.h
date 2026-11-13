#pragma once

#include "core/Math.h"
#include "nfs/Collision.h"

#include <string>
#include <vector>

namespace nfs { class World; struct SolidObject; }

// Collision made from an object's own mesh, for the objects placed in the
// editor (and any of the map's that lack it).
//
// Two shapes of answer, chosen from the mesh:
//
// - Solid (props, buildings, posts, trees): walls with a real height around
//   the outline of the part a car can hit -- the convex hull, in plan, of the
//   vertices up to 2.5 units above the object's foot, so a tree gets its trunk
//   and not its crown -- plus ground on the upward faces higher up, so a car
//   that lands on a roof or a container stays on it.
// - Drivable (ramps, platforms, decks, road pieces): ground on every upward
//   face, no walls. Chosen when an upward face that is not level, or a level
//   one at the foot, reaches down to the ground -- something meant to be
//   driven onto.
//
// Surfaces follow the model's name: posts and lights POST, trees and bushes
// PLANT, fences CHAINLINK, glass GLASS, wood WOOD, everything else SOLID_WALL
// for walls and ROAD for ground.
namespace collision {

enum class GeneratedShape { Solid, Drivable };

struct GenerateResult {
    GeneratedShape shape = GeneratedShape::Solid;
    size_t walls = 0, ground = 0;
    bool   groundCapped = false;   // more upward faces than kMaxGround: the rest left out
    std::wstring note;             // why nothing was made, when nothing was
};

constexpr size_t kMaxGround = 2000;

// Appends the polygons for `model` placed by `transform`. `world` (optional)
// gives the ground under it, which sets the foot of the walls.
GenerateResult GenerateObjectCollision(const nfs::SolidObject& model, const Mat4& transform,
                                       const nfs::World* world,
                                       std::vector<nfs::CollisionPolygon>& out);

} // namespace collision
