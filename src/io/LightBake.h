#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <vector>

namespace nfs { class World; struct SolidObject; }

// Night lighting baked into an object's vertex colours, the way the game's
// own meshes carry it.
//
// The game draws a world mesh as texture x vertex colour x 2 (Direct3D's
// MODULATE2X): a colour of 128 leaves the texture as it is, less darkens it,
// more brightens it, and the colours hold all of the night -- the dark of the
// street, the pools of the lamps. A new object (a model from Blender or
// Sketchfab) arrives white, which at 2x glows at night; baking gives it the
// light of where it stands:
//
//   - ambient: the darker vertex colours of the map's own meshes around it
//     (a low percentile), so it matches the street's own darkness;
//   - lamps: every street light of the map within reach, as a point light at
//     its head, its colour and strength measured from the pool the game baked
//     on the ground under it -- so the new light matches the old;
//   - per vertex: ambient + each lamp x falloff over distance x how much the
//     surface faces it.
namespace lightbake {

struct Lamp {
    Vec3 position;            // the head
    Vec3 base;                // where the post stands
};

struct Settings {
    float ambientScale = 1.0f;   // x the sampled ambient
    float lampScale = 1.0f;      // x the measured lamp strength
    float lampRadius = 24.0f;    // a lamp's reach, in units (metres)
    float searchRadius = 30.0f;  // how far round the object ambient is sampled
};

// What the map around a point says about its light: the ambient colour and
// the lamps, with the lamp colour measured from their pools. Colours are in
// the game's scale, 1.0 = a vertex colour of 128.
struct Environment {
    Vec3 ambient{ 0.45f, 0.45f, 0.5f };
    Vec3 lampColour{ 0.9f, 0.7f, 0.45f };   // at the lamp's own head height above the ground
    float lampHeight = 7.0f;
    std::vector<Lamp> lamps;
    size_t ambientSamples = 0, poolSamples = 0;
};

// Reads the environment round `area` (lamps from `area` grown by the lamp radius).
Environment Measure(const nfs::World& world, const Aabb& area, const Settings& settings);

// The baked colour of every vertex of `model` placed by `transform`, as
// D3DCOLOR (alpha kept from the model).
std::vector<u32> Bake(const nfs::SolidObject& model, const Mat4& transform, const Environment& environment,
                      const Settings& settings);

// A model name that lights the street: street lights, lamps.
bool IsLamp(const char* modelName);

} // namespace lightbake
