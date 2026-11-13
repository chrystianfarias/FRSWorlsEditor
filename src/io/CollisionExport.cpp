#include "io/CollisionExport.h"

#include "core/Log.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace {

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                           nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size,
                          nullptr, nullptr);
    return out;
}

std::wstring ReplaceExtension(const std::wstring& path, const wchar_t* extension) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        return path.substr(0, dot) + extension;
    return path + extension;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The same axis change as the mesh export: game Z-up to Blender's Y-up.
Vec3 ToBlender(const Vec3& v) { return { v.x, v.z, -v.y }; }

std::string MaterialName(const nfs::CollisionPolygon& polygon) {
    char name[48];
    ::_snprintf_s(name, _TRUNCATE, "%s_%02X_%02X_%02X_%02X",
                  polygon.barrier ? "barrier" : "ground",
                  polygon.attributes[0], polygon.attributes[1],
                  polygon.attributes[2], polygon.attributes[3]);
    return name;
}

// The corners a face is written with: the stored ones for ground, the wall
// quad for a barrier.
int FaceCorners(const nfs::CollisionPack& pack, const nfs::CollisionPolygon& polygon,
                Vec3 out[4]) {
    if (polygon.barrier) {
        pack.BarrierQuad(polygon, out);
        return 4;
    }
    for (int c = 0; c < 4; ++c) out[c] = polygon.corners[c];
    return polygon.quad ? 4 : 3;
}

struct PackKey {
    u32 section = 0;
    u32 pack    = 0;
    bool operator<(const PackKey& o) const {
        return section != o.section ? section < o.section : pack < o.pack;
    }
};

} // namespace

bool ExportCollision(const nfs::World& world,
                     const std::vector<CollisionPick>& selection,
                     const std::wstring& objPath,
                     const std::wstring& region,
                     CollisionExportSummary& summary) {
    if (selection.empty()) {
        LOG_WARN("no collision is selected, so there is nothing to export");
        return false;
    }

    // Grouped by pack, each pack's polygons in the order the file has them, so
    // the faces of an object follow the pack and duplicates fall away.
    std::map<PackKey, std::set<u32>> groups;
    const auto& loaded = world.Loaded();
    for (const CollisionPick& pick : selection) {
        if (pick.section >= loaded.size()) continue;
        const auto& packs = loaded[pick.section]->collision;
        if (pick.pack >= packs.size() || pick.polygon >= packs[pick.pack].polygons.size())
            continue;
        groups[{ pick.section, pick.pack }].insert(pick.polygon);
    }
    if (groups.empty()) {
        LOG_WARN("the collision selection no longer matches the loaded region");
        return false;
    }

    // One shared origin, for the reason the mesh export gives: world
    // coordinates put the street kilometres from Blender's origin.
    Aabb bounds;
    for (const auto& [key, indices] : groups) {
        const nfs::CollisionPack& pack = loaded[key.section]->collision[key.pack];
        for (u32 index : indices) {
            Vec3 corners[4];
            const int count = FaceCorners(pack, pack.polygons[index], corners);
            for (int c = 0; c < count; ++c) bounds.Add(corners[c]);
        }
    }
    const Vec3 origin = bounds.Valid() ? (bounds.min + bounds.max) * 0.5f : Vec3{};

    const std::wstring mtlPath      = ReplaceExtension(objPath, L".mtl");
    const std::wstring manifestPath = ReplaceExtension(objPath, L".json");

    FILE* obj = nullptr;
    if (::_wfopen_s(&obj, objPath.c_str(), L"w") != 0 || !obj) {
        LOG_ERROR("could not open the collision OBJ for writing");
        return false;
    }
    FILE* manifest = nullptr;
    if (::_wfopen_s(&manifest, manifestPath.c_str(), L"w") != 0 || !manifest) {
        std::fclose(obj);
        LOG_ERROR("could not open the collision manifest for writing");
        return false;
    }

    std::fprintf(obj, "# FRS World Editor collision export\n");
    std::fprintf(obj, "# region %s, %zu packs\n", Narrow(region).c_str(), groups.size());
    std::fprintf(obj, "# world space, moved by the origin in the manifest, Y-up for Blender\n");
    std::fprintf(obj, "# one face per polygon; the material name holds its attribute bytes\n");
    std::fprintf(obj, "# barriers are walls: only their bottom edge is read back\n");
    std::fprintf(obj, "mtllib %s\n\n", Narrow(FileNameOf(mtlPath)).c_str());

    std::fprintf(manifest, "{\n");
    std::fprintf(manifest, "  \"kind\": \"collision\",\n");
    std::fprintf(manifest, "  \"region\": \"%s\",\n", Narrow(region).c_str());
    std::fprintf(manifest, "  \"space\": \"world\",\n");
    std::fprintf(manifest, "  \"axes\": \"obj is Y-up: obj(x,y,z) = game(x, -z, y)\",\n");
    std::fprintf(manifest, "  \"origin\": [%.4f, %.4f, %.4f],\n", origin.x, origin.y, origin.z);
    std::fprintf(manifest, "  \"barrierHeight\": { \"below\": %.2f, \"above\": %.2f },\n",
                 nfs::CollisionPack::kBarrierBelow, nfs::CollisionPack::kBarrierAbove);
    std::fprintf(manifest, "  \"objects\": [\n");

    std::set<std::string> materials;
    size_t vertexBase = 1, polygons = 0, barriers = 0, written = 0;

    for (const auto& [key, indices] : groups) {
        const nfs::LoadedSection& section = *loaded[key.section];
        const nfs::CollisionPack& pack = section.collision[key.pack];
        const std::string sectionName = section.info ? section.info->name : "unknown";

        char objectName[64];
        if (key.pack == 0)
            ::_snprintf_s(objectName, _TRUNCATE, "collision_%s", sectionName.c_str());
        else
            ::_snprintf_s(objectName, _TRUNCATE, "collision_%s_%u", sectionName.c_str(), key.pack);

        std::fprintf(obj, "o %s\n", objectName);

        // Corners shared between neighbouring polygons become one vertex, so the
        // ground arrives in Blender as a connected surface that can be edited as
        // one. The positions are fixed point, so equal corners are exactly equal.
        std::map<std::tuple<long, long, long>, size_t> welded;
        std::vector<Vec3> vertices;
        std::vector<std::vector<size_t>> faces;
        std::vector<std::string> faceMaterials;

        for (u32 index : indices) {
            const nfs::CollisionPolygon& polygon = pack.polygons[index];
            Vec3 corners[4];
            const int count = FaceCorners(pack, polygon, corners);

            std::vector<size_t> face;
            for (int c = 0; c < count; ++c) {
                const Vec3& p = corners[c];
                const auto k = std::make_tuple(std::lround(p.x * 8.0f), std::lround(p.y * 8.0f),
                                               std::lround(p.z * 256.0f));
                auto [it, inserted] = welded.emplace(k, vertices.size());
                if (inserted) vertices.push_back(p);
                face.push_back(it->second);
            }
            faces.push_back(std::move(face));
            faceMaterials.push_back(MaterialName(polygon));
            materials.insert(faceMaterials.back());
            ++polygons;
            barriers += polygon.barrier;
        }

        for (const Vec3& v : vertices) {
            const Vec3 b = ToBlender(v - origin);
            std::fprintf(obj, "v %.4f %.4f %.4f\n", b.x, b.y, b.z);
        }

        std::string current;
        for (size_t f = 0; f < faces.size(); ++f) {
            if (faceMaterials[f] != current) {
                current = faceMaterials[f];
                std::fprintf(obj, "usemtl %s\n", current.c_str());
            }
            std::fprintf(obj, "f");
            for (size_t v : faces[f]) std::fprintf(obj, " %zu", vertexBase + v);
            std::fprintf(obj, "\n");
        }
        std::fprintf(obj, "\n");
        vertexBase += vertices.size();

        std::fprintf(manifest, "%s    {\n", written ? ",\n" : "");
        std::fprintf(manifest, "      \"name\": \"%s\",\n", objectName);
        std::fprintf(manifest, "      \"section\": \"%s\",\n", sectionName.c_str());
        std::fprintf(manifest, "      \"number\": %u,\n", pack.sectionNumber);
        std::fprintf(manifest, "      \"pack\": %u,\n", key.pack);
        std::fprintf(manifest, "      \"packPolygons\": %zu,\n", pack.polygons.size());
        std::fprintf(manifest, "      \"packFingerprint\": \"%08X\",\n",
                     nfs::CollisionFingerprint(pack));
        std::fprintf(manifest, "      \"polygons\": [");
        size_t n = 0;
        for (u32 index : indices) {
            const char* separator = n == 0 ? "\n        " : (n % 20 ? ", " : ",\n        ");
            std::fprintf(manifest, "%s%u", separator, index);
            ++n;
        }
        std::fprintf(manifest, "\n      ]\n    }");
        ++written;
    }

    std::fprintf(manifest, "\n  ]\n}\n");
    std::fclose(manifest);
    std::fclose(obj);

    // Colours only, so the kinds are told apart in Blender's solid view.
    FILE* mtl = nullptr;
    if (::_wfopen_s(&mtl, mtlPath.c_str(), L"w") == 0 && mtl) {
        std::fprintf(mtl, "# FRS World Editor collision export\n\n");
        for (const std::string& name : materials) {
            float r = 1.0f, g = 0.2f, b = 0.2f;
            if (name.rfind("ground", 0) == 0) {
                const unsigned surface = std::stoul(name.substr(10, 2), nullptr, 16);
                const float hue = std::fmod(0.15f + (float)surface * 0.618034f, 1.0f) * 0.8f + 0.1f;
                auto channel = [&](float offset) {
                    float t = std::fabs(std::fmod(hue * 6.0f + offset, 6.0f) - 3.0f) - 1.0f;
                    return 0.35f + 0.65f * std::clamp(t, 0.0f, 1.0f);
                };
                r = channel(0.0f); g = channel(4.0f); b = channel(2.0f);
            }
            std::fprintf(mtl, "newmtl %s\nKd %.3f %.3f %.3f\n\n", name.c_str(), r, g, b);
        }
        std::fclose(mtl);
    }

    summary.sections     = written;
    summary.polygons     = polygons;
    summary.barriers     = barriers;
    summary.objPath      = objPath;
    summary.manifestPath = manifestPath;

    LOG_INFO("exported collision: %zu packs, %zu polygons of which %zu barriers",
             written, polygons, barriers);
    return written > 0;
}
