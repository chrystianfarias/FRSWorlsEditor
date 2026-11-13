#pragma once

// TEMPORARY: minimap building preview.
//
// Everything for the minimap buildings lives in src/minimap. The rest of the
// editor reaches it only through blocks marked "MINIMAP PREVIEW (temporary)"
// in WorldRenderer.h/.cpp (draw the preview, hide the originals),
// MainWindow.h/.cpp (menu commands), Resource.h, App.rc and CMakeLists.txt.
// Removing the feature means deleting this folder and those marked blocks.
//
// The minimap draws buildings from a small JSON description instead of the
// game meshes. Each building is one of:
//
//   { "p": [x, y, x, y, ...], "z": base, "h": height }          a prism
//   { "parts": [ { "p": [...], "z": .., "h": .., "roof": {..} } ] }
//   { "mesh": { "v": [x, y, z, ...], "f": [[a, b, c], ...] } }
//
// The footprint "p" is a flat list of world X/Y pairs, counter-clockwise.
// A roof is { "type": "flat" | "pyramid" | "gable" | "dome", "h": rise },
// and a gable also has "dir": the ridge direction in degrees from +X.

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/SolidList.h"

#include <string>
#include <vector>

namespace minimap {

enum class RoofType : u8 { Flat, Pyramid, Gable, Dome };

struct Roof {
    RoofType type = RoofType::Flat;
    float    height = 0.0f;
    float    direction = 0.0f;   // gable ridge, degrees from +X
};

// One prism: a footprint raised from `base` by `height`, with a roof on top.
struct Part {
    std::vector<Vec2> footprint;   // counter-clockwise, convex
    float base = 0.0f;
    float height = 0.0f;
    Roof  roof;
};

// How a building was reduced, for colouring the preview and for the log.
enum class BuildingKind : u8 {
    Prism,     // one part, flat roof
    Parts,     // stacked parts or a pitched roof
    Box,       // facade only: the model's bounding box
    Mesh,      // did not fit prisms: a decimated mesh
};

struct Building {
    BuildingKind kind = BuildingKind::Prism;
    std::vector<Part> parts;                 // Prism, Parts, Box
    std::vector<Vec3> meshVertices;          // Mesh
    std::vector<u32>  meshTriangles;         // Mesh, three per face
};

// One building model where the world places it.
struct PlacedModel {
    const nfs::SolidObject* model = nullptr;
    Mat4 transform;
};

// Whether a model is one of the buildings the minimap redraws.
bool IsMinimapBuilding(const nfs::SolidObject& model);

// Reduces one placed building to the minimap description.
bool BuildBuilding(const PlacedModel& placed, Building& out);

struct GenerateSummary {
    size_t prisms = 0, parts = 0, boxes = 0, meshes = 0, skipped = 0;
    size_t Total() const { return prisms + parts + boxes + meshes; }
};

std::vector<Building> BuildAll(const std::vector<PlacedModel>& placed, GenerateSummary& summary);

bool WriteJson(const std::vector<Building>& buildings, const std::wstring& path);

// What the preview draws: flat-shaded triangles and outline lines, the look
// the minimap would give them.
struct PreviewVertex { float x, y, z; u32 colour; };

void BuildPreview(const std::vector<Building>& buildings,
                  std::vector<PreviewVertex>& triangles,
                  std::vector<PreviewVertex>& lines);

} // namespace minimap
