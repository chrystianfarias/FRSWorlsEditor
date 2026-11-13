#include "io/SelectionExport.h"

#include "core/Log.h"
#include "nfs/TextureDecode.h"
#include "nfs/World.h"

#include <windows.h>

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                           nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

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

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{L"."} : path.substr(0, slash);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// A name Blender and a JSON reader both take without argument: no spaces, no
// quotes, nothing that needs escaping on either side.
std::string SafeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(plain ? c : '_');
    }
    return out.empty() ? std::string("object") : out;
}

// Blender's OBJ importer reads Y as up and -Z as forward, so a file written in
// the game's Z-up axes arrives lying on its side. Converting here means the
// import dialog can be left alone; the manifest records the mapping so the way
// back is not a guess.
Vec3 ToBlender(const Vec3& v) { return { v.x, v.z, -v.y }; }

// Vertex normals rebuilt from the triangles, area weighted.
//
// The 24-byte vertex layout carries no normal, and terrain uses it, so the
// parser substitutes a constant one. Exporting that gives Blender a mesh where
// every face points the same way, which shades as one flat tone with no
// lighting at all. The triangles themselves hold the shape, so they are what
// the normals come from.
std::vector<Vec3> BuildNormals(const nfs::SolidObject& model) {
    std::vector<Vec3> normals(model.vertices.size(), Vec3{});

    for (size_t k = 0; k + 2 < model.indices.size(); k += 3) {
        const u16 ia = model.indices[k], ib = model.indices[k + 1], ic = model.indices[k + 2];
        if (ia >= model.vertices.size() || ib >= model.vertices.size() ||
            ic >= model.vertices.size())
            continue;

        const Vec3& a = model.vertices[ia].position;
        const Vec3& b = model.vertices[ib].position;
        const Vec3& c = model.vertices[ic].position;

        // The cross product's length is twice the triangle's area, so leaving it
        // unnormalised weights each face by how much of the surface it is.
        const Vec3 face = Cross(b - a, c - a);
        normals[ia] = normals[ia] + face;
        normals[ib] = normals[ib] + face;
        normals[ic] = normals[ic] + face;
    }

    for (Vec3& normal : normals) {
        if (Length(normal) > 1e-6f) normal = Normalize(normal);
        else                        normal = Vec3{ 0.0f, 0.0f, 1.0f };
    }
    return normals;
}

// Uncompressed 32-bit TGA, bottom-up.
//
// TGA rather than PNG because a PNG needs a deflate stream, and pulling in a
// compressor to save disk space on files that exist to be opened once in
// Blender is a poor trade. Blender reads TGA natively.
bool WriteTga(const std::wstring& path, const std::vector<u32>& bgra, u32 width, u32 height) {
    if (bgra.size() < (size_t)width * height) return false;

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;

    u8 header[18] = {};
    header[2]  = 2;                      // uncompressed true colour
    header[12] = (u8)(width & 0xFF);
    header[13] = (u8)(width >> 8);
    header[14] = (u8)(height & 0xFF);
    header[15] = (u8)(height >> 8);
    header[16] = 32;                     // bits per pixel
    header[17] = 8;                      // eight alpha bits, origin at the bottom
    std::fwrite(header, sizeof(header), 1, file);

    for (u32 y = 0; y < height; ++y) {
        const u32* row = bgra.data() + (size_t)(height - 1 - y) * width;
        std::fwrite(row, 4, width, file);
    }

    std::fclose(file);
    return true;
}

} // namespace

bool ExportSelection(const nfs::World& world,
                     const std::vector<PickResult>& selection,
                     const std::wstring& objPath,
                     const std::wstring& region,
                     ExportSummary& summary) {
    if (selection.empty()) {
        LOG_WARN("nothing is selected, so there is nothing to export");
        return false;
    }

    const std::wstring mtlPath      = ReplaceExtension(objPath, L".mtl");
    const std::wstring manifestPath = ReplaceExtension(objPath, L".json");
    const std::wstring textureDir   = DirectoryOf(objPath) + L"\\textures";
    ::CreateDirectoryW(textureDir.c_str(), nullptr);

    FILE* obj = nullptr;
    if (::_wfopen_s(&obj, objPath.c_str(), L"w") != 0 || !obj) {
        LOG_ERROR("could not open the OBJ for writing");
        return false;
    }

    FILE* manifest = nullptr;
    if (::_wfopen_s(&manifest, manifestPath.c_str(), L"w") != 0 || !manifest) {
        std::fclose(obj);
        LOG_ERROR("could not open the manifest for writing");
        return false;
    }

    std::fprintf(obj, "# FRS World Editor selection export\n");
    std::fprintf(obj, "# region %s, %zu objects selected\n",
                 Narrow(region).c_str(), selection.size());
    std::fprintf(obj, "# world space, moved by the origin in the manifest, Y-up for Blender\n");
    std::fprintf(obj, "# edit in place and export over this file; keep the object names\n");
    std::fprintf(obj, "mtllib %s\n\n", Narrow(FileNameOf(mtlPath)).c_str());

    std::fprintf(manifest, "{\n");
    std::fprintf(manifest, "  \"region\": \"%s\",\n", Narrow(region).c_str());
    std::fprintf(manifest, "  \"space\": \"world\",\n");
    std::fprintf(manifest, "  \"axes\": \"obj is Y-up: obj(x,y,z) = game(x, -z, y)\",\n");

    // Everything is moved by one shared point before it is written.
    //
    // World coordinates put a street several thousand units from zero, and an OBJ
    // has no per-object pivot: Blender puts every imported object's origin at
    // zero, so the handle that moves a lamp post sits a kilometre away from it.
    // Taking the centre of the whole selection out keeps the pieces in their real
    // positions relative to each other -- which is the point of editing a street
    // -- while leaving them within reach of the origin. The manifest carries the
    // point, so putting them back is arithmetic rather than guesswork.
    Aabb selectionBounds;
    for (const PickResult& picked : selection) {
        if (!picked.object) continue;
        for (const nfs::SolidVertex& vertex : picked.object->vertices)
            selectionBounds.Add(picked.transform.TransformPoint(vertex.position));
    }
    const Vec3 origin = selectionBounds.Valid()
                            ? (selectionBounds.min + selectionBounds.max) * 0.5f
                            : Vec3{};

    std::fprintf(manifest, "  \"origin\": [%.4f, %.4f, %.4f],\n",
                 origin.x, origin.y, origin.z);
    std::fprintf(manifest, "  \"objects\": [\n");

    std::unordered_set<u32> usedTextures;
    size_t vertexBase = 1;      // OBJ indices start at one and run across the file
    size_t triangles  = 0;
    size_t written    = 0;

    // How many of the selected placements use each mesh, so the manifest can say
    // what a shape stands for.
    std::unordered_map<u32, size_t> placementCount;
    for (const PickResult& picked : selection)
        if (picked.object) ++placementCount[picked.object->hash];

    // One shape per mesh, however many times it was selected.
    //
    // A building used at both ends of a street is one mesh with two placements,
    // and the game stores it once. Writing it twice invites the two copies to be
    // shaped differently in Blender, which is asking the format for something it
    // cannot hold: whichever came back last would win and change both places.
    // Exporting the mesh once makes that impossible rather than merely detected.
    std::unordered_set<u32> exported;
    size_t collapsed = 0;

    for (const PickResult& picked : selection) {
        const nfs::SolidObject* model = picked.object;
        if (!model || model->vertices.empty() || model->indices.empty()) continue;

        if (!exported.insert(model->hash).second) {
            ++collapsed;
            continue;
        }

        char objectName[224];
        ::_snprintf_s(objectName, _TRUNCATE, "%s__%08X",
                      SafeName(model->name).c_str(), model->hash);

        std::fprintf(obj, "o %s\n", objectName);

        for (const nfs::SolidVertex& vertex : model->vertices) {
            const Vec3 p =
                ToBlender(picked.transform.TransformPoint(vertex.position) - origin);

            // The colour goes on the same line, which is the form OBJ uses for it
            // and what Blender's "Colors" option reads. It is not decoration: the
            // game bakes its night lighting into these, and a mesh that comes back
            // without them loses the light it was lit with.
            const u8 r = (u8)((vertex.color >> 16) & 0xFF);
            const u8 g = (u8)((vertex.color >> 8) & 0xFF);
            const u8 b = (u8)(vertex.color & 0xFF);

            std::fprintf(obj, "v %.4f %.4f %.4f %.4f %.4f %.4f\n", p.x, p.y, p.z,
                         r / 255.0f, g / 255.0f, b / 255.0f);
        }

        // Stored normals where the layout has them, rebuilt ones where it does not.
        const std::vector<Vec3> rebuilt =
            model->hasNormals ? std::vector<Vec3>{} : BuildNormals(*model);

        for (size_t v = 0; v < model->vertices.size(); ++v) {
            const Vec3 source = model->hasNormals ? model->vertices[v].normal : rebuilt[v];
            Vec3 n = picked.transform.TransformDirection(source);
            n = Length(n) > 1e-6f ? Normalize(n) : Vec3{ 0.0f, 0.0f, 1.0f };
            const Vec3 b = ToBlender(n);
            std::fprintf(obj, "vn %.4f %.4f %.4f\n", b.x, b.y, b.z);
        }
        for (const nfs::SolidVertex& vertex : model->vertices) {
            // OBJ runs its vertical texture axis the other way from Direct3D.
            std::fprintf(obj, "vt %.5f %.5f\n", vertex.uv.x, 1.0f - vertex.uv.y);
        }

        for (const nfs::SolidMaterial& material : model->materials) {
            const u32 textureHash =
                material.textureIndex < model->textureHashes.size()
                    ? model->textureHashes[material.textureIndex] : 0;

            if (textureHash) {
                usedTextures.insert(textureHash);
                std::fprintf(obj, "usemtl tex_%08X\n", textureHash);
            } else {
                std::fprintf(obj, "usemtl untextured\n");
            }

            const size_t last = (size_t)material.firstIndex + material.indexCount;
            for (size_t k = material.firstIndex;
                 k + 2 < last && k + 2 < model->indices.size(); k += 3) {
                const size_t a = vertexBase + model->indices[k];
                const size_t b = vertexBase + model->indices[k + 1];
                const size_t c = vertexBase + model->indices[k + 2];
                std::fprintf(obj, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n",
                             a, a, a, b, b, b, c, c, c);
                ++triangles;
            }
        }

        std::fprintf(obj, "\n");

        const Mat4& m = picked.transform;
        std::fprintf(manifest, "%s    {\n", written ? ",\n" : "");
        std::fprintf(manifest, "      \"name\": \"%s\",\n", objectName);
        std::fprintf(manifest, "      \"model\": \"%s\",\n", model->name.c_str());
        std::fprintf(manifest, "      \"hash\": \"%08X\",\n", model->hash);
        std::fprintf(manifest, "      \"section\": \"%s\",\n",
                     picked.sectionName.empty() ? "?" : picked.sectionName.c_str());
        std::fprintf(manifest, "      \"placedBy\": \"%s\",\n",
                     picked.fromInstance ? "instance" : "header");
        std::fprintf(manifest, "      \"vertices\": %zu,\n", model->vertices.size());
        std::fprintf(manifest, "      \"triangles\": %u,\n", model->TriangleCount());

        // The transform below is the placement this shape was written from. When
        // the mesh is used more than once, editing it changes every one of them,
        // because there is only the one mesh to change.
        std::fprintf(manifest, "      \"placementsInSelection\": %zu,\n",
                     placementCount[model->hash]);
        std::fprintf(manifest,
                     "      \"transform\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, "
                     "%.6f, %.6f, %.6f, %.6f, %.6f, %.6f]\n",
                     m.m[0][0], m.m[0][1], m.m[0][2],
                     m.m[1][0], m.m[1][1], m.m[1][2],
                     m.m[2][0], m.m[2][1], m.m[2][2],
                     m.m[3][0], m.m[3][1], m.m[3][2]);
        std::fprintf(manifest, "    }");

        vertexBase += model->vertices.size();
        ++written;
    }

    std::fprintf(manifest, "\n  ]\n}\n");
    std::fclose(manifest);
    std::fclose(obj);

    // --- materials and their textures ---------------------------------------
    FILE* mtl = nullptr;
    if (::_wfopen_s(&mtl, mtlPath.c_str(), L"w") == 0 && mtl) {
        std::fprintf(mtl, "# FRS World Editor selection export\n\n");
        std::fprintf(mtl, "newmtl untextured\nKd 0.60 0.60 0.60\n\n");

        for (u32 hash : usedTextures) {
            const nfs::Texture* texture = world.FindTexture(hash);
            std::fprintf(mtl, "newmtl tex_%08X\n", hash);
            std::fprintf(mtl, "Kd 1.00 1.00 1.00\n");
            if (!texture) {
                std::fprintf(mtl, "# texture %08X is not in this region\n\n", hash);
                continue;
            }

            std::vector<u32> pixels;
            u32 width = 0, height = 0;
            if (!nfs::DecodeTexture(*texture, 0, pixels, width, height)) {
                std::fprintf(mtl, "# %s could not be decoded\n\n", texture->name.c_str());
                continue;
            }

            const std::string file = SafeName(texture->name) + ".tga";
            if (WriteTga(textureDir + L"\\" + Widen(file), pixels, width, height))
                std::fprintf(mtl, "map_Kd textures/%s\n", file.c_str());
            std::fprintf(mtl, "\n");
        }
        std::fclose(mtl);
    }

    summary.objects      = written;
    summary.collapsed    = collapsed;
    summary.triangles    = triangles;
    summary.textures     = usedTextures.size();
    summary.objPath      = objPath;
    summary.manifestPath = manifestPath;

    LOG_INFO("exported %zu meshes, %zu triangles, %zu textures "
             "(%zu repeat placements of a mesh already written)",
             written, triangles, usedTextures.size(), collapsed);
    return written > 0;
}
