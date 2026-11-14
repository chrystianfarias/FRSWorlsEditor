#pragma once

#include "roads/RoadNetwork.h"

#include <functional>
#include <string>
#include <vector>

// Road prefabs, as a truck-sim map editor has them: ready-made pieces of road
// network -- roundabouts, an overpass, an interchange, crossroads -- laid down
// whole at a point and a heading, built of ordinary nodes and roads so every
// part stays editable afterwards. Their open ends join roads already there
// when one ends close by.
namespace roads {

struct PrefabInfo {
    std::string  key;
    std::wstring name;
    std::wstring description;
};

// Every prefab offered, in the order shown.
const std::vector<PrefabInfo>& Prefabs();
const PrefabInfo* FindPrefab(const std::string& key);

// What placing one made.
struct PrefabResult {
    std::vector<u32> nodes;      // every node it added
    std::vector<u32> ends;       // the open ends among them (one road each)
    std::vector<u32> segments;
    size_t joined = 0;           // ends joined to nodes already there
};

// The height of the ground at a point (fallback when there is none).
using PrefabGround = std::function<float(float x, float y, float fallback)>;

// Lays prefab `key` into `network` with its middle at `origin`, turned
// `heading` degrees counter-clockwise from +X; its roads are made like `base`
// (textures, lights, sidewalks), with lanes and the rest as the prefab needs.
// Open ends within `joinReach` of a node already there (and not part of the
// prefab) are joined to it. False for an unknown key.
bool PlacePrefab(Network& network, const std::string& key, const Vec3& origin, float heading,
                 const RoadProfile& base, const PrefabGround& ground, PrefabResult& result, float joinReach = 6.0f);

// A picture of the prefab seen from above: `size` x `size` BGRA pixels, top
// row first -- the roads light on a dark ground, bridges brighter.
bool DrawPrefabPicture(const std::string& key, const RoadProfile& base, int size, std::vector<u32>& bgra);

} // namespace roads
