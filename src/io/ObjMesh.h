#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A mesh read whole out of a Wavefront OBJ.
//
// The position-only reader that puts vertices back where they were is enough to
// reshape something; building a mesh that has gained or lost geometry needs the
// faces as well, and the material each face belongs to.
//
// OBJ indexes position, texture coordinate and normal separately, and the game
// does not: one vertex there carries all three. So the corners are gathered into
// whole vertices and reused only when every part matches, which is what turns a
// file where a position is shared by faces with different texture coordinates
// into a mesh a GPU can draw.
struct ObjFaceGroup {
    std::string material;    // the usemtl name, e.g. tex_928585E0
    u32 firstIndex = 0;
    u32 indexCount = 0;
};

struct ObjMesh {
    std::string name;

    std::vector<Vec3> positions;
    // Per-vertex colour, white where the file carries none. The game bakes its
    // lighting into these, so a mesh that loses them comes back unlit.
    std::vector<u32>  colors;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u16>  indices;

    std::vector<ObjFaceGroup> groups;

    size_t VertexCount() const { return positions.size(); }
    size_t TriangleCount() const { return indices.size() / 3; }
};

// Reads every object in the file. Faces with more than three corners are split
// into a fan, which is what an OBJ quad means and what the game can draw.
//
// `error` is filled when something in the file cannot be carried over, such as a
// mesh needing more than 65,535 vertices -- the index buffers in the track
// streams are sixteen bit, so that is a wall rather than a preference.
// What image each material draws with, by the material's name.
//
// The two names are not the same thing and cannot be assumed to be. A material
// called `body_upper` can perfectly well be painted with `upper body.png`, and
// in an OBJ exported from a model someone else built, it usually is. The link
// between them lives in the MTL file's `map_Kd` line and nowhere else, so that
// is what has to be read.
//
// The value is the image's base name, without directory or extension, which is
// what the texture is called once it is in the game.
bool ReadObjMaterials(const std::wstring& objPath,
                      std::unordered_map<std::string, std::string>& out,
                      std::unordered_set<std::string>* tgaMaterials = nullptr,
                      // The image file itself, by material, resolved to a full path.
                      std::unordered_map<std::string, std::wstring>* imageFiles = nullptr,
                      // The material's Kd colour, as 0xAARRGGBB in sRGB.
                      std::unordered_map<std::string, u32>* diffuse = nullptr);

bool ReadObjMeshes(const std::wstring& path, std::vector<ObjMesh>& out,
                   std::wstring& error);
