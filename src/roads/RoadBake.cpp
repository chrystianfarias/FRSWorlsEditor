#include "roads/RoadBake.h"

#include "core/Log.h"

#include <cmath>
#include <cstdio>

namespace roads {

bool IsBakedRoadModel(const std::string& name) {
    return name.compare(0, std::char_traits<char>::length(kBakedModelPrefix), kBakedModelPrefix) == 0;
}

namespace {

// The game is Z up, the OBJ Y up: obj(x, y, z) = game(x, -z, y).
Vec3 ToBlender(const Vec3& v) { return { v.x, v.z, -v.y }; }

} // namespace

bool WriteRoadObj(const std::vector<MeshPiece>& pieces, const std::wstring& objPath, Vec3& origin,
                  std::wstring& error) {
    Aabb all;
    for (const MeshPiece& piece : pieces) all.Add(piece.Bounds());
    if (!all.Valid()) {
        error = L"there are no roads to bake";
        return false;
    }
    // Whole units, so the numbers in the file stay readable.
    origin = { std::floor(all.Center().x), std::floor(all.Center().y), std::floor(all.min.z) };

    FILE* file = nullptr;
    if (::_wfopen_s(&file, objPath.c_str(), L"wb") != 0 || !file) {
        error = L"could not write " + objPath;
        return false;
    }
    std::fprintf(file, "# FRS World Editor road bake: one object per piece, materials name game textures\n");

    size_t base = 1;
    for (const MeshPiece& piece : pieces) {
        if (piece.VertexCount() == 0) continue;
        std::fprintf(file, "o %s\n", piece.name.c_str());
        for (const MeshGroup& g : piece.groups)
            for (const MeshVertex& v : g.vertices) {
                const Vec3 p = ToBlender(v.position - origin);
                const float r = ((v.color >> 16) & 0xFF) / 255.0f;
                const float gr = ((v.color >> 8) & 0xFF) / 255.0f;
                const float b = (v.color & 0xFF) / 255.0f;
                std::fprintf(file, "v %.4f %.4f %.4f %.4f %.4f %.4f\n", p.x, p.y, p.z, r, gr, b);
            }
        for (const MeshGroup& g : piece.groups)
            for (const MeshVertex& v : g.vertices) {
                const Vec3 n = ToBlender(v.normal);
                std::fprintf(file, "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
            }
        // OBJ runs its vertical texture axis the other way from Direct3D.
        for (const MeshGroup& g : piece.groups)
            for (const MeshVertex& v : g.vertices) std::fprintf(file, "vt %.5f %.5f\n", v.uv.x, 1.0f - v.uv.y);

        size_t first = base;
        for (const MeshGroup& g : piece.groups) {
            if (!g.indices.empty()) {
                // A material named after a game texture is that texture.
                std::fprintf(file, "usemtl %s\n", g.texture.c_str());
                for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
                    const size_t a = first + g.indices[k], b = first + g.indices[k + 1], c = first + g.indices[k + 2];
                    std::fprintf(file, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
                }
            }
            first += g.vertices.size();
        }
        base = first;
        std::fprintf(file, "\n");
    }
    std::fclose(file);
    LOG_INFO("roads: %zu pieces written to %S around (%.1f, %.1f, %.1f)", pieces.size(), objPath.c_str(),
             origin.x, origin.y, origin.z);
    return true;
}

} // namespace roads
