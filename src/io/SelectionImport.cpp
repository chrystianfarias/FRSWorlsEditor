#include "io/SelectionImport.h"
#include "io/TextureImport.h"

#include "core/Log.h"
#include "io/MeshOverlay.h"
#include "io/ObjMesh.h"
#include "io/ModelCopy.h"
#include "io/SectionEdits.h"
#include "io/SectionRebuild.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// What the manifest says about one exported shape.
struct ManifestEntry {
    u32    hash = 0;
    size_t vertices = 0;
    Mat4   transform = Mat4::Identity();
    std::string model;
};

std::string ReadWholeFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{L"."} : path.substr(0, slash);
}

std::wstring ReplaceExtension(const std::wstring& path, const wchar_t* extension) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        return path.substr(0, dot) + extension;
    return path + extension;
}

// The manifest is written by this editor and read back by it, so a scanner that
// understands the handful of shapes it uses is enough. It still has to fail
// rather than invent: an entry that does not parse is one object left alone, not
// one written to the wrong place.
bool FindString(const std::string& text, size_t begin, size_t end, const char* key,
                std::string& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted, begin);
    if (at == std::string::npos || at >= end) return false;

    const size_t colon = text.find(':', at + quoted.size());
    if (colon == std::string::npos || colon >= end) return false;
    const size_t open = text.find('"', colon);
    if (open == std::string::npos || open >= end) return false;
    const size_t close = text.find('"', open + 1);
    if (close == std::string::npos || close >= end) return false;

    value = text.substr(open + 1, close - open - 1);
    return true;
}

bool FindNumber(const std::string& text, size_t begin, size_t end, const char* key,
                double& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted, begin);
    if (at == std::string::npos || at >= end) return false;
    const size_t colon = text.find(':', at + quoted.size());
    if (colon == std::string::npos || colon >= end) return false;

    const char* first = text.c_str() + colon + 1;
    char* last = nullptr;
    const double parsed = std::strtod(first, &last);
    if (last == first) return false;
    value = parsed;
    return true;
}

bool FindNumbers(const std::string& text, size_t begin, size_t end, const char* key,
                 std::vector<double>& values) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted, begin);
    if (at == std::string::npos || at >= end) return false;
    const size_t open = text.find('[', at + quoted.size());
    if (open == std::string::npos || open >= end) return false;
    const size_t close = text.find(']', open);
    if (close == std::string::npos || close >= end) return false;

    values.clear();
    const char* cursor = text.c_str() + open + 1;
    const char* stop   = text.c_str() + close;
    while (cursor < stop) {
        char* last = nullptr;
        const double parsed = std::strtod(cursor, &last);
        if (last == cursor) { ++cursor; continue; }
        values.push_back(parsed);
        cursor = last;
    }
    return !values.empty();
}

bool ReadManifest(const std::wstring& path,
                  std::unordered_map<std::string, ManifestEntry>& entries, Vec3& origin) {
    const std::string text = ReadWholeFile(path);
    if (text.empty()) return false;

    std::vector<double> originValues;
    if (FindNumbers(text, 0, text.size(), "origin", originValues) && originValues.size() >= 3)
        origin = Vec3{ (float)originValues[0], (float)originValues[1], (float)originValues[2] };

    const size_t listStart = text.find("\"objects\"");
    if (listStart == std::string::npos) return false;

    // Each object is one { } block in the list; walking brace to brace keeps the
    // fields of one entry from being read as another's.
    size_t cursor = text.find('[', listStart);
    if (cursor == std::string::npos) return false;

    while (true) {
        const size_t open = text.find('{', cursor);
        if (open == std::string::npos) break;

        int depth = 0;
        size_t close = open;
        for (; close < text.size(); ++close) {
            if (text[close] == '{') ++depth;
            else if (text[close] == '}' && --depth == 0) break;
        }
        if (close >= text.size()) break;

        std::string name, hashText;
        std::vector<double> matrix;
        double vertices = 0.0;

        if (FindString(text, open, close, "name", name) &&
            FindString(text, open, close, "hash", hashText) &&
            FindNumber(text, open, close, "vertices", vertices) &&
            FindNumbers(text, open, close, "transform", matrix) && matrix.size() >= 12) {

            ManifestEntry entry;
            entry.hash     = (u32)std::strtoul(hashText.c_str(), nullptr, 16);
            entry.vertices = (size_t)vertices;
            FindString(text, open, close, "model", entry.model);

            entry.transform = Mat4::Identity();
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    entry.transform.m[row][column] = (float)matrix[(size_t)row * 3 + column];
            entry.transform.m[3][0] = (float)matrix[9];
            entry.transform.m[3][1] = (float)matrix[10];
            entry.transform.m[3][2] = (float)matrix[11];

            entries.emplace(name, entry);
        }

        cursor = close + 1;
        const size_t next = text.find_first_not_of(" \t\r\n,", cursor);
        if (next == std::string::npos || text[next] == ']') break;
    }

    // An empty list is still a manifest: a session for new objects starts
    // with nothing in it but the origin.
    return true;
}

// The shapes an OBJ holds, in the order Blender wrote them.
struct ObjObject {
    std::string       name;
    std::vector<Vec3> positions;
};

bool ReadObj(const std::wstring& path, std::vector<ObjObject>& objects) {
    std::ifstream file(path);
    if (!file) return false;

    // Geometry before any `o` line -- a file from a tool that names nothing --
    // is one object, called what the mesh reader calls it.
    ObjObject current;
    current.name = "object";
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() < 2) continue;

        if (line[0] == 'o' && line[1] == ' ') {
            if (!current.name.empty() && !current.positions.empty())
                objects.push_back(std::move(current));
            current = ObjObject{};
            std::istringstream stream(line.substr(2));
            stream >> current.name;
        } else if (line[0] == 'v' && line[1] == ' ') {
            std::istringstream stream(line.substr(2));
            Vec3 position;
            if (stream >> position.x >> position.y >> position.z)
                current.positions.push_back(position);
        }
    }
    if (!current.name.empty() && !current.positions.empty()) objects.push_back(std::move(current));

    return !objects.empty();
}

// Blender's axes back to the game's. The export wrote obj(x,y,z) = game(x,-z,y).
Vec3 FromBlender(const Vec3& v) { return { v.x, -v.z, v.y }; }

u64 AlignUp(u64 value, u64 to) { return (value + to - 1) / to * to; }

// Copies the region's files aside once, before anything is written to them.
bool MakeBackup(const std::wstring& tracksFolder, const std::wstring& region,
                std::wstring& folder) {
    folder = tracksFolder + L"\\_frs-original-backup";
    ::CreateDirectoryW(folder.c_str(), nullptr);

    const std::wstring names[] = { region + L".BUN", L"STREAM" + region + L".BUN" };
    for (const std::wstring& name : names) {
        const std::wstring target = folder + L"\\" + name;
        if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

        // FALSE would overwrite; the whole point is that the first copy is the
        // untouched one, however many times this runs afterwards.
        if (!::CopyFileW((tracksFolder + L"\\" + name).c_str(), target.c_str(), TRUE)) {
            LOG_ERROR("could not back up %S before writing", name.c_str());
            return false;
        }
        LOG_INFO("backed up %S", name.c_str());
    }
    return true;
}


// The manifest that belongs to this OBJ.
//
// Beside it with the same name is the normal case. But Blender exports under
// whatever name was typed, and then the pair no longer share one, so the folder
// is searched for a manifest that actually names the shapes in this file.
// Guessing wrong here would write a building's vertices into a lamp post, so the
// test is that every name in the OBJ is present in the manifest.
bool FindManifest(const std::wstring& objPath, const std::vector<ObjObject>& objects,
                  std::unordered_map<std::string, ManifestEntry>& entries, Vec3& origin,
                  std::wstring& used) {
    used = ReplaceExtension(objPath, L".json");
    if (ReadManifest(used, entries, origin)) return true;

    const std::wstring folder = DirectoryOf(objPath);
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*.json").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;

    bool matched = false;
    do {
        std::unordered_map<std::string, ManifestEntry> candidate;
        Vec3 candidateOrigin{};
        if (!ReadManifest(folder + L"\\" + found.cFileName, candidate, candidateOrigin))
            continue;

        bool coversAll = true;
        for (const ObjObject& object : objects) {
            if (candidate.count(object.name)) continue;

            const std::string& name = object.name;
            const size_t dot = name.find_last_of('.');
            if (dot != std::string::npos && candidate.count(name.substr(0, dot))) continue;

            const size_t tail = name.rfind("__");
            if (tail != std::string::npos && candidate.count(name.substr(0, tail))) continue;

            coversAll = false;
            break;
        }

        if (coversAll) {
            entries = std::move(candidate);
            origin  = candidateOrigin;
            used    = folder + L"\\" + found.cFileName;
            matched = true;
            LOG_INFO("import: using the manifest %S", found.cFileName);
        }
    } while (!matched && ::FindNextFileW(search, &found));

    ::FindClose(search);
    return matched;
}


// Blender's default material colour, 0.8 linear, in sRGB.
constexpr u32 kDefaultDiffuse = 0xFFE7E7E7u;

// A texture of one colour, for faces nothing else paints. Shared by every
// face of that colour: the name, and so the hash, is the colour.
u32 ColorTexture(u32 color, std::vector<NewTexture>& newTextures, std::vector<u32>& list) {
    char name[32];
    ::_snprintf_s(name, _TRUNCATE, "FRS_COLOR_%06X", color & 0xFFFFFFu);
    const u32 hash = TextureNameHash(name);
    if (std::none_of(newTextures.begin(), newTextures.end(),
                     [&](const NewTexture& t) { return t.hash == hash; })) {
        NewTexture texture;
        TextureFromPixels(name, std::vector<u32>(8 * 8, color | 0xFF000000u), 8, 8, texture);
        newTextures.push_back(std::move(texture));
    }
    if (std::find(list.begin(), list.end(), hash) == list.end()) list.push_back(hash);
    return hash;
}

// An OBJ mesh as the game stores one: in the space given by `toLocal`, in the
// layout `stride` names, with its materials pointing into a texture list that
// starts from `startingHashes`. `newTextures` gets whatever the section has to
// be given for it -- images beside the OBJ or named by its MTL, game textures
// the section cannot see, colours.
bool BuildReplacement(const nfs::World& world, const std::wstring& objPath, const ObjMesh* source,
                      const Mat4& toLocal, const Vec3& origin, u32 stride,
                      const nfs::Section* section, const std::vector<u32>& startingHashes,
                      ReplacementMesh& replacement, std::vector<NewTexture>& newTextures,
                      std::wstring& why) {
    std::wstring error;

    replacement = ReplacementMesh{};
    newTextures.clear();
    replacement.stride = stride;
    replacement.vertices.reserve(source->positions.size());

    for (size_t i = 0; i < source->positions.size(); ++i) {
        nfs::SolidVertex vertex;
        vertex.position = toLocal.TransformPoint(FromBlender(source->positions[i]) + origin);

        Vec3 normal = toLocal.TransformDirection(FromBlender(source->normals[i]));
        vertex.normal = Length(normal) > 1e-6f ? Normalize(normal) : Vec3{ 0.0f, 0.0f, 1.0f };

        // The vertical texture axis runs the other way in OBJ, the same swap the
        // export made on the way out.
        vertex.uv = { source->uvs[i].x, 1.0f - source->uvs[i].y };

        // The colour the OBJ carried, which is the lighting the game baked in.
        // Mid grey stands in when the file has none -- the track shader combines
        // texture and colour at double strength, so white would come out twice
        // as bright as everything around it while half leaves the texture as it
        // is. A mesh that arrives grey was exported without vertex colours.
        vertex.color = i < source->colors.size() ? source->colors[i] : 0xFF808080;

        replacement.vertices.push_back(vertex);
    }

    replacement.indices = source->indices;

    // Welded back together when the layout stores no normal.
    //
    // Blender writes a normal per corner, so the OBJ splits a position into as
    // many vertices as it has differing normals -- 1,211 positions came back as
    // 6,706. A section that stores 24-byte vertices has nowhere to put a normal,
    // so those copies are identical in every field the file keeps and only
    // quadruple its size. Merging them on position, texture coordinate and
    // colour gives back what the mesh actually is.
    if (replacement.stride == 24) {
        struct Key {
            float x, y, z, u, v;
            u32   color;
            bool operator==(const Key& other) const {
                return x == other.x && y == other.y && z == other.z &&
                       u == other.u && v == other.v && color == other.color;
            }
        };
        struct Hash {
            size_t operator()(const Key& k) const {
                size_t h = 1469598103934665603ull;
                for (float f : { k.x, k.y, k.z, k.u, k.v }) {
                    u32 bits;
                    std::memcpy(&bits, &f, 4);
                    h = (h ^ bits) * 1099511628211ull;
                }
                return (h ^ k.color) * 1099511628211ull;
            }
        };

        std::unordered_map<Key, u16, Hash> welded;
        std::vector<nfs::SolidVertex> kept;
        std::vector<u16> remap(replacement.vertices.size(), 0);

        for (size_t i = 0; i < replacement.vertices.size(); ++i) {
            const nfs::SolidVertex& vertex = replacement.vertices[i];
            const Key key{ vertex.position.x, vertex.position.y, vertex.position.z,
                           vertex.uv.x, vertex.uv.y, vertex.color };

            const auto found = welded.find(key);
            if (found != welded.end()) { remap[i] = found->second; continue; }

            const u16 index = (u16)kept.size();
            welded.emplace(key, index);
            kept.push_back(vertex);
            remap[i] = index;
        }

        if (kept.size() < replacement.vertices.size()) {
            LOG_INFO("welded %zu vertices down to %zu: the layout keeps no normal",
                     replacement.vertices.size(), kept.size());
            for (u16& index : replacement.indices)
                if (index < remap.size()) index = remap[index];
            replacement.vertices = std::move(kept);
        }
    }

    // --- textures ------------------------------------------------------------
    //
    // Read before the materials, because a group can only be pointed at a new
    // texture once that texture has a place in the object's list.
    std::unordered_map<std::string, std::string> materialImages;
    std::unordered_set<std::string> tgaMaterials;
    std::unordered_map<std::string, std::wstring> imageFiles;
    std::unordered_map<std::string, u32> diffuse;
    ReadObjMaterials(objPath, materialImages, &tgaMaterials, &imageFiles, &diffuse);

    if (!LoadTextureFolder(objPath, newTextures, error)) {
        why = error;
        return false;
    }

    // An image the MTL points at wherever it is. Blender links a picture from
    // where it was picked -- the Downloads folder, more often than not -- and
    // the folder beside the OBJ never sees it, so the faces came back naming a
    // texture nothing had built. Only the materials this mesh draws with are
    // read, and never one of our own tex_ materials, which name the game's.
    for (const ObjFaceGroup& group : source->groups) {
        if (group.indexCount == 0 || group.material.compare(0, 4, "tex_") == 0) continue;
        const auto file = imageFiles.find(group.material);
        const auto image = materialImages.find(group.material);
        if (file == imageFiles.end() || image == materialImages.end()) continue;
        if (tgaMaterials.count(group.material) && FindGameTextureByFile(world, image->second))
            continue;

        std::string name = image->second;
        for (char& c : name) c = (char)::toupper((unsigned char)c);
        const u32 hash = TextureNameHash(name);
        if (std::any_of(newTextures.begin(), newTextures.end(),
                        [&](const NewTexture& t) { return t.hash == hash; }))
            continue;
        if (::GetFileAttributesW(file->second.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("import: material '%s' paints with %S, which is not there",
                     group.material.c_str(), file->second.c_str());
            continue;
        }

        NewTexture texture;
        if (!LoadTextureFile(file->second, texture, error)) {
            why = error;
            return false;
        }
        LOG_INFO("import: new texture %s (%ux%u) read from %S", texture.name.c_str(),
                 texture.width, texture.height, file->second.c_str());
        newTextures.push_back(std::move(texture));
    }

    // A TGA beside the OBJ that is one of the game's textures comes back as
    // that texture, not as a new one squeezed through the compressor again.
    newTextures.erase(std::remove_if(newTextures.begin(), newTextures.end(), [&](const NewTexture& t) {
                          if (!t.fromTga || !FindGameTextureByFile(world, t.name)) return false;
                          LOG_INFO("import: %s.tga is the game's own texture, used as it is", t.name.c_str());
                          return true;
                      }), newTextures.end());

    // The section the mesh lives in, to know which game textures it can see.
    size_t loadedIndex = SIZE_MAX;
    for (size_t i = 0; i < world.Loaded().size(); ++i)
        if (world.Loaded()[i]->info == section) loadedIndex = i;

    replacement.textureHashes = startingHashes;

    for (const NewTexture& texture : newTextures)
        if (std::find(replacement.textureHashes.begin(),
                      replacement.textureHashes.end(),
                      texture.hash) == replacement.textureHashes.end())
            replacement.textureHashes.push_back(texture.hash);

    // --- materials -----------------------------------------------------------
    //
    // The export named each material after the texture it uses, so the group
    // names carry the hash back. A group whose name says nothing recognisable
    // keeps the first texture rather than losing its faces.
    for (const ObjFaceGroup& group : source->groups) {
        if (group.indexCount == 0) continue;

        // A group named `tex_<hash>` came from our own export and names a
        // texture the object already had. Any other name is taken as a texture
        // name, which is how a new one arrives: a PNG called MY_WALL.png in the
        // folder beside the OBJ, and `usemtl MY_WALL` on the faces that use it.
        u32 hash = 0;
        const nfs::Texture* gameTexture = nullptr;
        if (group.material.size() > 4 && group.material.compare(0, 4, "tex_") == 0) {
            hash = (u32)std::strtoul(group.material.c_str() + 4, nullptr, 16);
            gameTexture = world.FindTexture(hash);

        } else if (tgaMaterials.count(group.material) &&
                   (gameTexture = FindGameTextureByFile(world, materialImages[group.material])) != nullptr) {
            // Painted with a TGA the export wrote: the game's own texture.
            hash = gameTexture->hash;

        } else if (!group.material.empty()) {
            // The material's own name is the fallback, not the answer. What the
            // texture is called is the name of the image the material paints
            // with, which the MTL records and the material name need not
            // resemble at all -- `body_upper` drawn with `upper body.png`.
            const auto link = materialImages.find(group.material);
            std::string name =
                link == materialImages.end() ? group.material : link->second;

            for (char& c : name) c = (char)::toupper((unsigned char)c);
            hash = TextureNameHash(name);

            // Named after one of the game's textures, and not arriving as a
            // new one: that texture, as it is.
            const bool coming = std::any_of(newTextures.begin(), newTextures.end(),
                                            [&](const NewTexture& t) { return t.hash == hash; });
            if (!coming) gameTexture = world.FindTexture(hash);
        }

        // Nothing paints it -- Blender's default material, or a colour chosen
        // without an image. A small texture of that colour stands in, rather
        // than the faces borrowing whatever the object happens to list first.
        const bool painted = gameTexture ||
                             std::any_of(newTextures.begin(), newTextures.end(),
                                         [&](const NewTexture& t) { return t.hash == hash; });
        if (!painted) {
            const auto kd = diffuse.find(group.material);
            hash = ColorTexture(kd == diffuse.end() ? kDefaultDiffuse : kd->second, newTextures,
                                replacement.textureHashes);
        }

        // A game texture the object did not use before joins its list, and
        // one the section cannot see -- stored in another positioned section
        // -- is copied into the section's pack as it is.
        if (gameTexture) {
            if (std::find(replacement.textureHashes.begin(), replacement.textureHashes.end(), hash) ==
                replacement.textureHashes.end())
                replacement.textureHashes.push_back(hash);
            const bool coming = std::any_of(newTextures.begin(), newTextures.end(),
                                            [&](const NewTexture& t) { return t.hash == hash; });
            if (!coming && !TextureVisibleTo(world, hash, loadedIndex)) {
                NewTexture copy;
                if (CopyGameTexture(*gameTexture, copy)) {
                    LOG_INFO("import: texture %s copied into section %s", gameTexture->name.c_str(),
                             section->name.c_str());
                    newTextures.push_back(std::move(copy));
                }
            }
        }

        u32 textureIndex = 0;
        for (size_t t = 0; t < replacement.textureHashes.size(); ++t)
            if (replacement.textureHashes[t] == hash) { textureIndex = (u32)t; break; }

        nfs::SolidMaterial material;
        material.firstIndex   = group.firstIndex;
        material.indexCount   = group.indexCount;
        material.textureIndex = textureIndex;

        for (u32 i = group.firstIndex; i < group.firstIndex + group.indexCount; ++i)
            if (i < replacement.indices.size() &&
                replacement.indices[i] < replacement.vertices.size())
                material.bounds.Add(replacement.vertices[replacement.indices[i]].position);

        replacement.materials.push_back(material);
    }

    if (replacement.materials.empty()) {
        if (replacement.textureHashes.empty())
            ColorTexture(kDefaultDiffuse, newTextures, replacement.textureHashes);
        nfs::SolidMaterial material;
        material.firstIndex = 0;
        material.indexCount = (u32)replacement.indices.size();
        for (const nfs::SolidVertex& vertex : replacement.vertices)
            material.bounds.Add(vertex.position);
        replacement.materials.push_back(material);
    }

    return true;
}

// Puts a mesh back that no longer has the vertex count it left with.
//
// Everything here exists because the chunks change size. The mesh is read again
// from the OBJ with its faces and material groups, turned into the vertex layout
// the section already uses, and the section is rebuilt around it. If the result
// no longer fits between this section and the next, it is given an address past
// the end of the stream and the region's table is pointed at it -- the loader
// answers reads the file cannot, so growing is not capped by the room the
// original layout happened to leave.
// The object as the game's own file has it, not as the editor is showing it.
//
// The editor applies whatever overlays are on disk when it opens a region, so
// the object it hands out already carries the last import's changes. Rebuilding
// from that compounds: import twice and the texture list grows twice, keeping
// hashes from a folder that is no longer there. The section is rebuilt from the
// original bytes, so the object it is rebuilt around has to come from them too.
bool OriginalObject(const ByteSpan& stream, const nfs::Section& section, u32 hash,
                    nfs::SolidObject& out) {
    if (section.originalStreamOffset + section.originalStreamSize > stream.size)
        return false;

    const ByteSpan bytes =
        stream.sub((size_t)section.originalStreamOffset, section.originalStreamSize);

    nfs::ChunkReader top(bytes, section.originalStreamOffset);
    nfs::Chunk chunk;
    while (top.Next(chunk)) {
        if (chunk.id != nfs::kChunkSolidListPack) continue;

        nfs::ChunkReader pack = nfs::ChunkReader::Children(chunk);
        nfs::Chunk entry;
        while (pack.Next(entry)) {
            if (entry.id != nfs::kChunkSolidObject) continue;

            nfs::SolidObject candidate;
            if (!nfs::ParseSolidObject(entry, candidate)) continue;
            if (candidate.hash != hash) continue;

            out = std::move(candidate);
            return true;
        }
    }
    return false;
}

// Whether the section shipped with a texture pack of its own.
bool HasOwnTexturePack(const nfs::World& world, const nfs::Section& section) {
    const ByteSpan stream = world.StreamBytes();
    if (section.originalStreamOffset + (u64)section.originalStreamSize > stream.size) return false;
    nfs::ChunkReader top(stream.sub(section.originalStreamOffset, section.originalStreamSize),
                         section.originalStreamOffset);
    nfs::Chunk chunk;
    while (top.Next(chunk))
        if (chunk.id == nfs::kChunkTexturePack) return true;
    return false;
}

// Textures for a section that has no pack to put them in.
//
// About half the positioned sections of a region ship without a texture pack:
// everything they draw with comes from the library, which the game keeps
// loaded everywhere (175 of L4RA's 360). A section rebuild can only add a
// texture to a pack the section has, so for these it dropped them without a
// word -- the faces came back naming a texture nothing held. They go to the
// region's main library pack instead, the largest one, from where every
// section can see them.
void RouteTexturesToLibrary(const nfs::World& world, const nfs::Section& section,
                            std::vector<NewTexture>& textures, ImportPlan& plan) {
    if (textures.empty() || HasOwnTexturePack(world, section)) return;

    const nfs::Section* library = nullptr;
    for (const auto& loaded : world.Loaded()) {
        if (!loaded->info || !world.IsLibrary(*loaded) || !HasOwnTexturePack(world, *loaded->info))
            continue;
        if (!library || loaded->info->originalStreamSize > library->originalStreamSize)
            library = loaded->info;
    }
    if (!library) {
        LOG_WARN("import: section %s has no texture pack and the region no library pack; "
                 "%zu textures left out", section.name.c_str(), textures.size());
        return;
    }

    std::vector<SectionEdit> edits;
    for (NewTexture& texture : textures) {
        SectionEdit edit;
        edit.kind = SectionEdit::Kind::TextureAdded;
        edit.hash = texture.hash;
        edit.label = texture.name;
        edit.order = 0;       // the library section's first pack
        edit.texture = std::move(texture);
        LOG_INFO("import: texture %s goes to the library pack in %s (section %s has none)",
                 edit.label.c_str(), library->name.c_str(), section.name.c_str());
        edits.push_back(std::move(edit));
    }
    textures.clear();
    plan.addedEdits.push_back({ library->name, std::move(edits) });
}

// Gives an object chunk a name, hash and header matrix of its own.
//
// The name sits at 0xA4 of the header, zero terminated, in whatever room the
// header leaves; the hash, which is the game's hash of that name, at 0x10; the
// storage matrix at 0x40. With `pickName` the name is made to fit and to be
// unused in the region; otherwise it is kept as given.
bool RenameObjectChunk(const nfs::World& world, SectionEdit& edit, const Mat4& placement,
                       bool pickName, const ImportPlan& plan, std::string& name, u32& hash,
                       std::wstring& why) {
    nfs::Chunk wrapper;
    wrapper.id = nfs::kChunkSolidObject;
    // Only the position modulo 128 matters to the filler inside, and that is
    // what the chunk was laid out for.
    wrapper.headerOffset = edit.mod128;
    wrapper.dataOffset = edit.mod128 + 8;
    wrapper.raw = ByteSpan{ edit.chunk.data() + 8, edit.chunk.size() - 8 };
    wrapper.data = wrapper.raw;
    const nfs::Chunk header = nfs::FindChunk(wrapper, nfs::kChunkSolidObjectHeader);
    if (!header.id || header.data.size < 0xA4 + 8) {
        why = L"the object it was built from has no header to name it in";
        return false;
    }
    const size_t at = (size_t)(header.data.data - edit.chunk.data());
    const size_t room = header.data.size - 0xA4;     // terminator included

    if (pickName) {
        std::string base = name.substr(0, room - 1);
        const auto taken = [&](const std::string& candidate) {
            const u32 h = TextureNameHash(candidate);
            if (world.FindModel(h)) return true;
            return std::any_of(plan.created.begin(), plan.created.end(),
                               [&](const ImportPlan::Created& c) { return c.hash == h; });
        };
        name = base;
        for (int n = 2; taken(name) && n < 1000; ++n) {
            const std::string suffix = "_" + std::to_string(n);
            name = base.substr(0, std::min(base.size(), room - 1 - suffix.size())) + suffix;
        }
        hash = TextureNameHash(name);
    }
    if (name.size() >= room) {
        why = L"its name does not fit the object header";
        return false;
    }

    std::memcpy(edit.chunk.data() + at + 0x10, &hash, 4);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            std::memcpy(edit.chunk.data() + at + 0x40 + (size_t)(row * 4 + column) * 4,
                        &placement.m[row][column], 4);
    std::memset(edit.chunk.data() + at + 0xA4, 0, room);
    std::memcpy(edit.chunk.data() + at + 0xA4, name.data(), name.size());

    edit.kind = SectionEdit::Kind::ModelAdded;
    edit.hash = hash;
    edit.label = name;
    return true;
}

// A model the section did not ship with, built from a mesh in the OBJ.
//
// There is no writer for an object chunk from nothing, and the chunk holds
// fields nobody has decoded, so none is invented. One of the section's own
// objects is rebuilt around the new mesh -- exactly what editing it would do --
// and the result is taken out under a name and hash of its own, as a model
// added to the section: the same edit a model copied in from elsewhere is. The
// object it was built from stays as it shipped.
bool BuildAddedModel(const nfs::World& world, const std::wstring& objPath, const ObjMesh* source,
                     const Vec3& origin, const nfs::Section& section, const Mat4& placement,
                     bool pickName, std::string& name, u32& hash, ImportPlan& plan,
                     std::vector<SectionEdit>& edits, std::wstring& why) {
    const ByteSpan stream = world.StreamBytes();
    if (section.originalStreamOffset + (u64)section.originalStreamSize > stream.size) {
        why = L"the section is not in the stream file";
        return false;
    }
    const ByteSpan bytes = stream.sub(section.originalStreamOffset, section.originalStreamSize);

    // Props first: a road or a backdrop piece may carry flags a free-standing
    // object should not inherit.
    std::vector<nfs::SolidObject> templates;
    nfs::ChunkReader top(bytes, section.originalStreamOffset);
    nfs::Chunk chunk;
    while (top.Next(chunk)) {
        if (chunk.id != nfs::kChunkSolidListPack) continue;
        nfs::ChunkReader pack = nfs::ChunkReader::Children(chunk);
        nfs::Chunk entry;
        while (pack.Next(entry)) {
            if (entry.id != nfs::kChunkSolidObject) continue;
            nfs::SolidObject candidate;
            // The sky and the panorama are drawn apart from the world, and
            // not every piece of them is named so the classifier knows.
            if (!nfs::ParseSolidObject(entry, candidate) || !candidate.vertexStride ||
                candidate.vertices.empty())
                continue;
            const bool sky = candidate.name.find("SKY") != std::string::npos ||
                             candidate.name.find("PAN_") != std::string::npos;
            if (!sky) templates.push_back(std::move(candidate));
        }
    }
    std::stable_sort(templates.begin(), templates.end(),
                     [](const nfs::SolidObject& a, const nfs::SolidObject& b) {
                         const bool plainA = !a.IsTerrain() && !a.IsBackdrop();
                         const bool plainB = !b.IsTerrain() && !b.IsBackdrop();
                         return plainA && !plainB;
                     });
    if (templates.size() > 6) templates.resize(6);
    if (templates.empty()) {
        why = L"the section has no object of its own to build a new one from";
        return false;
    }

    const Mat4 toLocal = InverseAffine(placement);
    std::wstring error;
    for (const nfs::SolidObject& model : templates) {
        ReplacementMesh replacement;
        std::vector<NewTexture> textures;
        if (!BuildReplacement(world, objPath, source, toLocal, origin, model.vertexStride,
                              &section, {}, replacement, textures, why))
            return false;
        // Kept aside until the model is made, so a template that fails does
        // not leave its textures behind in the library.
        std::vector<NewTexture> forLibrary;
        if (!HasOwnTexturePack(world, section)) forLibrary.swap(textures);

        RebuiltSection rebuilt;
        if (!RebuildSection(stream.data, stream.size, section.originalStreamOffset,
                            section.originalStreamSize, 0xFFFFFFFFu, model.hash, replacement,
                            textures, rebuilt, error)) {
            why = error;
            continue;
        }

        std::vector<SectionEdit> diff;
        if (!DiffSectionBytes(world, section, ByteSpan{ rebuilt.bytes.data(), rebuilt.bytes.size() },
                              section.originalStreamOffset, diff, error)) {
            why = error;
            continue;
        }

        edits.clear();
        bool made = false;
        for (SectionEdit& edit : diff) {
            if (edit.kind == SectionEdit::Kind::MeshChanged && edit.hash == model.hash) {
                if (!RenameObjectChunk(world, edit, placement, pickName, plan, name, hash, why))
                    return false;
                made = true;
                edits.push_back(std::move(edit));
            } else if (edit.kind == SectionEdit::Kind::TextureAdded ||
                       edit.kind == SectionEdit::Kind::TextureChanged) {
                edits.push_back(std::move(edit));
            }
        }
        if (made) {
            RouteTexturesToLibrary(world, section, forLibrary, plan);
            LOG_INFO("import: model %s (%08X) built in section %s from %s, %zu vertices",
                     name.c_str(), hash, section.name.c_str(), model.name.c_str(),
                     replacement.vertices.size());
            return true;
        }
    }
    if (why.empty()) why = L"no object of the section could be rebuilt around it";
    return false;
}

// A shape in the OBJ that the manifest does not know: a new object.
//
// It becomes a model of its own in the positioned section under it, and a
// placement of that model where it stands -- pivot in the middle of its
// footprint, on its lowest point, so the editor's move and rotate tools turn
// it about where it touches the ground.
bool CreateNewObject(const nfs::World& world, const std::wstring& objPath,
                     const std::string& objectName, const Vec3& origin, ImportPlan& plan,
                     std::string& modelName, std::wstring& why) {
    std::vector<ObjMesh> meshes;
    std::wstring error;
    if (!ReadObjMeshes(objPath, meshes, error)) { why = error; return false; }
    const ObjMesh* source = nullptr;
    for (const ObjMesh& mesh : meshes)
        if (mesh.name == objectName) { source = &mesh; break; }
    if (!source || source->indices.empty()) { why = L"it has no faces"; return false; }

    Aabb box;
    for (const Vec3& p : source->positions) box.Add(FromBlender(p) + origin);
    const Vec3 pivot{ (box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, box.min.z };

    const size_t target = world.SectionForPlacement(pivot);
    const nfs::LoadedSection* loaded =
        target < world.Loaded().size() ? world.Loaded()[target].get() : nullptr;
    if (!loaded || !loaded->info) { why = L"no section where it stands can take scenery"; return false; }
    const nfs::Section& section = *loaded->info;

    // Named after the object in Blender, marked as the editor's own so it can
    // never be mistaken for, or collide with, one of the game's.
    std::string name = "FRS_";
    for (char c : objectName) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        name.push_back(plain ? (char)::toupper((unsigned char)c) : '_');
    }
    u32 hash = 0;
    const Mat4 placement = Mat4::Translation(pivot);

    std::vector<SectionEdit> edits;
    if (!BuildAddedModel(world, objPath, source, origin, section, placement, true, name, hash,
                         plan, edits, why))
        return false;

    Aabb local;
    for (const Vec3& p : source->positions) local.Add(FromBlender(p) + origin - pivot);

    SectionEdit place;
    place.kind = SectionEdit::Kind::Placement;
    place.label = name;
    place.sceneryNumber = section.number;
    place.placement.modelHash = hash;
    place.placement.modelName = name;
    place.placement.transform = placement;
    place.placement.modelBounds = local;
    place.placement.triangles = (u32)source->TriangleCount();
    place.placement.radius = Length(local.max - local.min) * 0.5f;
    edits.push_back(std::move(place));

    plan.addedEdits.push_back({ section.name, std::move(edits) });

    ImportPlan::Created created;
    created.objectName = objectName;
    created.model = name;
    created.hash = hash;
    created.vertices = source->positions.size();
    created.triangles = source->TriangleCount();
    created.transform = placement;
    created.section = section.name;
    plan.created.push_back(std::move(created));

    modelName = name;
    return true;
}

// Adds the objects just made to the manifest, so the next import of the same
// OBJ edits them rather than making them again. Creates the manifest when
// the OBJ came without one.
bool RecordCreated(const ImportPlan& plan, const std::wstring& region) {
    std::string entries;
    for (const ImportPlan::Created& c : plan.created) {
        const Mat4& m = c.transform;
        char text[1024];
        ::_snprintf_s(text, _TRUNCATE,
                      "    {\n"
                      "      \"name\": \"%s\",\n"
                      "      \"model\": \"%s\",\n"
                      "      \"hash\": \"%08X\",\n"
                      "      \"section\": \"%s\",\n"
                      "      \"placedBy\": \"instance\",\n"
                      "      \"vertices\": %zu,\n"
                      "      \"triangles\": %zu,\n"
                      "      \"placementsInSelection\": 1,\n"
                      "      \"transform\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, "
                      "%.6f, %.6f, %.6f, %.6f, %.6f, %.6f]\n"
                      "    }",
                      c.objectName.c_str(), c.model.c_str(), c.hash, c.section.c_str(),
                      c.vertices, c.triangles,
                      m.m[0][0], m.m[0][1], m.m[0][2], m.m[1][0], m.m[1][1], m.m[1][2],
                      m.m[2][0], m.m[2][1], m.m[2][2], m.m[3][0], m.m[3][1], m.m[3][2]);
        if (!entries.empty()) entries += ",\n";
        entries += text;
    }

    std::string text;
    {
        std::ifstream in(plan.manifestPath, std::ios::binary);
        if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const size_t list = text.find("\"objects\"");
    const size_t open = list == std::string::npos ? std::string::npos : text.find('[', list);
    const size_t close = text.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        std::string regionText(region.begin(), region.end());
        char head[512];
        ::_snprintf_s(head, _TRUNCATE,
                      "{\n  \"region\": \"%s\",\n  \"space\": \"world\",\n"
                      "  \"axes\": \"obj is Y-up: obj(x,y,z) = game(x, -z, y)\",\n"
                      "  \"origin\": [%.4f, %.4f, %.4f],\n  \"objects\": [\n",
                      regionText.c_str(), plan.origin.x, plan.origin.y, plan.origin.z);
        text = std::string(head) + entries + "\n  ]\n}\n";
    } else {
        const bool hasEntries = text.find('{', open) < close;
        size_t end = close;
        while (end > open && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\r'))
            --end;
        text = text.substr(0, end) + (hasEntries ? ",\n" : "\n") + entries + "\n  " +
               text.substr(close);
    }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, plan.manifestPath.c_str(), L"wb") != 0 || !file) {
        LOG_ERROR("import: could not record the new objects in %S", plan.manifestPath.c_str());
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    LOG_INFO("import: %zu new objects recorded in %S", plan.created.size(), plan.manifestPath.c_str());
    return true;
}

bool RebuildForNewMesh(const nfs::World& world, const std::wstring& objPath,
                       const std::string& objectName, const ManifestEntry& entry,
                       const Vec3& origin, const nfs::SolidObject& model,
                       ImportPlan& plan, std::wstring& why) {
    if (model.vertexStride == 0 || model.vertexFileOffset == 0) {
        why = L"the game's copy of that mesh has no vertices on file";
        return false;
    }

    std::vector<ObjMesh> meshes;
    std::wstring error;
    if (!ReadObjMeshes(objPath, meshes, error)) { why = error; return false; }

    const ObjMesh* source = nullptr;
    for (const ObjMesh& mesh : meshes) {
        if (mesh.name == objectName) { source = &mesh; break; }
        const size_t dot = mesh.name.find_last_of('.');
        if (dot != std::string::npos && mesh.name.substr(0, dot) == objectName) {
            source = &mesh;
            break;
        }
    }
    if (!source) { why = L"its faces are not in the OBJ"; return false; }

    // --- the section it lives in --------------------------------------------
    const nfs::Section* section = nullptr;
    const nfs::Section* next    = nullptr;
    for (const nfs::Section& candidate : world.Table().Sections()) {
        if (model.vertexFileOffset >= candidate.streamOffset &&
            model.vertexFileOffset <  candidate.streamOffset + candidate.streamSize) {
            section = &candidate;
        }
    }
    if (!section) { why = L"it is not inside any section of this region"; return false; }

    // The room it can grow into is whatever lies before the next section starts.
    u64 nextStart = world.StreamBytes().size;
    for (const nfs::Section& candidate : world.Table().Sections()) {
        if (candidate.streamOffset > section->streamOffset &&
            candidate.streamOffset < nextStart) {
            nextStart = candidate.streamOffset;
            next = &candidate;
        }
    }
    (void)next;
    const u64 room = nextStart - section->streamOffset;

    // --- the mesh, in the game's own terms ----------------------------------
    const Mat4 toLocal = InverseAffine(entry.transform);

    // From the file, not from the world the editor is showing -- see
    // OriginalObject above.
    nfs::SolidObject pristine;
    if (!OriginalObject(world.StreamBytes(), *section, model.hash, pristine)) {
        // Not in the section as it shipped: a model the editor made, or copied
        // in from elsewhere. It is built again the way it was made -- as a
        // model added to the section, under its own name and hash.
        //
        // Placed in other sections as well, it was copied into each of them,
        // and every copy is rebuilt -- the places it stands must not disagree
        // about its shape.
        std::vector<const nfs::Section*> holders;
        for (const auto& loaded : world.Loaded())
            if (loaded->info && !world.IsLibrary(*loaded) && loaded->HoldsModel(model.hash) &&
                !OriginalObject(world.StreamBytes(), *loaded->info, model.hash, pristine))
                holders.push_back(loaded->info);
        if (std::find(holders.begin(), holders.end(), section) == holders.end())
            holders.push_back(section);

        for (const nfs::Section* holder : holders) {
            std::string name = model.name;
            u32 hash = model.hash;
            std::vector<SectionEdit> edits;
            if (!BuildAddedModel(world, objPath, source, origin, *holder, entry.transform, false,
                                 name, hash, plan, edits, why))
                return false;
            plan.addedEdits.push_back({ holder->name, std::move(edits) });
        }
        return true;
    }

    ReplacementMesh replacement;
    std::vector<NewTexture> newTextures;
    if (!BuildReplacement(world, objPath, source, toLocal, origin, model.vertexStride, section,
                          pristine.textureHashes, replacement, newTextures, why))
        return false;
    RouteTexturesToLibrary(world, *section, newTextures, plan);

    // --- rebuild -------------------------------------------------------------
    const ByteSpan stream = world.StreamBytes();

    RebuiltSection rebuilt;
    if (!RebuildSection(stream.data, stream.size, section->originalStreamOffset,
                        section->originalStreamSize,
                        (u32)std::min<u64>(room, 0xFFFFFFFFu),
                        model.hash, replacement, newTextures, rebuilt, error)) {
        // Not fitting is not the end: the section can live past the end of the
        // file, where nothing is in the way.
        RebuiltSection relocated;
        const u64 elsewhere = AlignUp(stream.size + 4096, 2048);

        if (!RebuildSection(stream.data, stream.size, section->originalStreamOffset,
                            section->originalStreamSize, 0xFFFFFFFFu, model.hash,
                            replacement, newTextures, relocated, error)) {
            why = error;
            return false;
        }

        ImportPlan::Span span;
        span.name       = model.name + " (section " + section->name + ", moved)";
        span.section    = section->name;
        span.fileOffset = elsewhere;
        span.bytes.assign(relocated.bytes.begin(), relocated.bytes.end());
        plan.spans.push_back(std::move(span));

        // And the table entry that points at it: offset first, then the size.
        ImportPlan::Span table;
        table.name       = "section " + section->name + " in the region table";
        table.fileOffset = section->tableEntryOffset + 0x14;
        table.layoutFile = true;
        table.bytes.resize(12);

        const u32 newOffset = (u32)elsewhere;
        const u32 newSize   = (u32)relocated.bytes.size();

        const u32 newOther  = relocated.geometrySize;
        std::memcpy(table.bytes.data(), &newOffset, 4);
        std::memcpy(table.bytes.data() + 4, &newSize, 4);
        std::memcpy(table.bytes.data() + 8, &newOther, 4);
        plan.spans.push_back(std::move(table));

        LOG_INFO("section %s moved to %llu and grown to %zu bytes",
                 section->name.c_str(), elsewhere, relocated.bytes.size());
        return true;
    }

    ImportPlan::Span span;
    span.name       = model.name + " (section " + section->name + ")";
    span.section    = section->name;
    span.fileOffset = rebuilt.fileOffset;
    span.bytes.assign(rebuilt.bytes.begin(), rebuilt.bytes.end());
    plan.spans.push_back(std::move(span));

    if (rebuilt.grew) {
        // Both size fields, not just the one that bounds the chunks.
        //
        // The entry carries a second length after it whose meaning is not settled
        // -- it does not end on a chunk boundary -- but it covers the part of the
        // section the edit sits inside, so leaving it alone describes a section
        // that no longer exists. It is moved by the same amount the section grew.
        ImportPlan::Span table;
        table.name       = "section " + section->name + " in the region table";
        table.fileOffset = section->tableEntryOffset + 0x18;
        table.layoutFile = true;
        table.bytes.resize(8);

        const u32 newSize = (u32)rebuilt.bytes.size();
        const u32 newOther = rebuilt.geometrySize;

        std::memcpy(table.bytes.data(), &newSize, 4);
        std::memcpy(table.bytes.data() + 4, &newOther, 4);
        plan.spans.push_back(std::move(table));
    }

    return true;
}

} // namespace

bool PlanImport(const nfs::World& world, const std::wstring& objPath, ImportPlan& plan,
                const Vec3* newOrigin) {
    plan.objPath = objPath;
    std::vector<ObjObject> objects;
    if (!ReadObj(objPath, objects)) {
        plan.report = L"That OBJ holds no named objects.";
        LOG_ERROR("import: the OBJ has no objects");
        return false;
    }
    plan.objectsRead = objects.size();

    std::unordered_map<std::string, ManifestEntry> entries;
    Vec3 origin{};
    const bool manifest = FindManifest(objPath, objects, entries, origin, plan.manifestPath);
    if (!manifest && newOrigin) {
        // An OBJ from anywhere, brought in as new objects: its origin goes
        // where it was asked for, and a manifest is started beside it.
        entries.clear();
        origin = *newOrigin;
        plan.manifestPath = ReplaceExtension(objPath, L".json");
    } else if (!manifest) {
        plan.report = L"No manifest in that folder names the objects in this OBJ.\n\n"
                      L"Keep the .json the editor wrote next to whatever you export "
                      L"from Blender.";
        LOG_ERROR("import: no manifest covers the objects in the OBJ");
        return false;
    }

    plan.origin = origin;

    std::wostringstream report;
    std::wostringstream rebuilds;

    for (const ObjObject& object : objects) {
        // Blender appends a suffix when two objects share a name, and older
        // exports carried a placement counter, so the match falls back through
        // both before giving up.
        const std::string& name = object.name;
        auto entry = entries.find(name);
        if (entry == entries.end()) {
            const size_t dot = name.find_last_of('.');
            if (dot != std::string::npos) entry = entries.find(name.substr(0, dot));
        }
        if (entry == entries.end()) {
            const size_t tail = name.rfind("__");
            if (tail != std::string::npos) entry = entries.find(name.substr(0, tail));
        }

        const std::wstring wide(name.begin(), name.end());

        if (entry == entries.end()) {
            std::string model;
            std::wstring why;
            if (CreateNewObject(world, objPath, name, origin, plan, model, why)) {
                rebuilds << L"  " << wide << L": new object " << std::wstring(model.begin(), model.end())
                         << L" in section "
                         << std::wstring(plan.created.back().section.begin(),
                                         plan.created.back().section.end())
                         << L"\n";
            } else {
                ++plan.unknownName;
                report << L"  " << wide << L": no manifest entry, and it could not be made a new "
                       << L"object: " << why << L"\n";
            }
            continue;
        }

        const nfs::SolidObject* model = world.FindModel(entry->second.hash);
        if (!model || model->vertexFileOffset == 0 || model->vertexStride == 0) {
            ++plan.unknownName;
            report << L"  " << wide << L": its mesh is not loaded, left alone\n";
            continue;
        }

        // Every mesh is rebuilt from the OBJ whole -- faces, UVs, colours and
        // materials -- rather than having only its positions written back when
        // the vertex count happens to match. A matching count says nothing
        // about order: Blender lists vertices by its own `v` lines, a rebuilt
        // mesh stores them by face corner, and an object whose count came out
        // equal had its positions laid onto the wrong vertices, with its
        // materials left as they were.
        std::wstring why;
        if (RebuildForNewMesh(world, objPath, name, entry->second, origin, *model, plan, why)) {
            ++plan.rebuilt;
            rebuilds << L"  " << wide << L": " << object.positions.size()
                     << L" vertices (was " << model->vertices.size()
                     << L"), rebuilt with its faces, UVs and materials\n";
        } else {
            ++plan.countMismatch;
            report << L"  " << wide << L": " << why << L"\n";
        }
    }

    // A mesh placed twice has one shape, not two.
    //
    // The export writes each mesh once, but nothing stops someone duplicating an
    // object inside Blender. The file holds the mesh once, so writing both would
    // let the last one win and quietly change the other placement as well.
    std::vector<ImportPlan::Mesh> kept;
    std::unordered_map<u64, size_t> firstAtOffset;
    std::vector<bool> conflicted(plan.meshes.size(), false);

    for (size_t i = 0; i < plan.meshes.size(); ++i) {
        const auto found = firstAtOffset.find(plan.meshes[i].fileOffset);
        if (found == firstAtOffset.end()) {
            firstAtOffset.emplace(plan.meshes[i].fileOffset, i);
            continue;
        }

        const ImportPlan::Mesh& first = plan.meshes[found->second];
        float worst = 0.0f;
        for (size_t v = 0; v < plan.meshes[i].positions.size() && v < first.positions.size(); ++v)
            worst = std::max(worst, Length(plan.meshes[i].positions[v] - first.positions[v]));

        conflicted[i] = true;   // a second copy is never written on its own
        if (worst > 0.01f) {
            conflicted[found->second] = true;
            ++plan.countMismatch;

            const std::wstring a(plan.meshes[i].name.begin(), plan.meshes[i].name.end());
            const std::wstring b(first.name.begin(), first.name.end());
            report << L"  " << a << L" and " << b
                   << L" are the same mesh shaped two different ways, up to "
                   << (int)worst << L" units apart. The game keeps one copy of a mesh, "
                   << L"so neither was written.\n";
        }
    }

    for (size_t i = 0; i < plan.meshes.size(); ++i)
        if (!conflicted[i]) kept.push_back(std::move(plan.meshes[i]));
    plan.meshes = std::move(kept);

    // A rebuilt section is not a problem to report, it is the work getting done,
    // so it is listed apart from the things that were left alone.
    // Each rebuilt section is turned into edits and merged with the ones
    // already in force, then rebuilt from the shipped bytes with all of them.
    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(world.OverlayFolder(), world.Region(), existing);
    // Every mesh of one section comes in its own rebuild, each made from the
    // shipped bytes with only that mesh -- so they are gathered per section
    // first and the section is written once, with all of them.
    std::vector<std::string> merged;
    std::vector<std::pair<std::string, std::vector<SectionEdit>>> incomingBySection;
    for (const ImportPlan::Span& span : plan.spans) {
        if (span.layoutFile || span.section.empty()) continue;
        const nfs::Section* section = world.Table().FindByName(span.section);
        if (!section) continue;

        std::vector<SectionEdit> incoming;
        std::wstring error;
        if (!DiffSectionBytes(world, *section, ByteSpan{ span.bytes.data(), span.bytes.size() },
                              section->originalStreamOffset, incoming, error)) {
            report << L"  section " << std::wstring(span.section.begin(), span.section.end())
                   << L": " << error << L"\n";
            continue;
        }
        auto slot = std::find_if(incomingBySection.begin(), incomingBySection.end(),
                                 [&](const auto& entry) { return entry.first == span.section; });
        if (slot == incomingBySection.end()) {
            incomingBySection.push_back({ span.section, {} });
            slot = incomingBySection.end() - 1;
        }
        for (SectionEdit& fresh : incoming) {
            if (fresh.kind != SectionEdit::Kind::MeshChanged && fresh.kind != SectionEdit::Kind::TextureAdded &&
                fresh.kind != SectionEdit::Kind::TextureChanged)
                continue;
            // The same mesh or texture from two rebuilds: the later one wins.
            auto& list = slot->second;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const SectionEdit& e) {
                           const bool isTexture = e.kind != SectionEdit::Kind::MeshChanged;
                           const bool freshTexture = fresh.kind != SectionEdit::Kind::MeshChanged;
                           return isTexture == freshTexture && e.hash == fresh.hash;
                       }), list.end());
            list.push_back(std::move(fresh));
        }
    }

    // Models built as added ones -- new objects and re-edits of them -- come
    // as edits already, with their textures and, for a new one, its placement.
    for (auto& [sectionName, edits] : plan.addedEdits) {
        auto slot = std::find_if(incomingBySection.begin(), incomingBySection.end(),
                                 [&](const auto& entry) { return entry.first == sectionName; });
        if (slot == incomingBySection.end()) {
            incomingBySection.push_back({ sectionName, {} });
            slot = incomingBySection.end() - 1;
        }
        for (SectionEdit& fresh : edits) {
            const bool texture = fresh.kind == SectionEdit::Kind::TextureAdded ||
                                 fresh.kind == SectionEdit::Kind::TextureChanged;
            if (texture) {
                auto& list = slot->second;
                list.erase(std::remove_if(list.begin(), list.end(), [&](const SectionEdit& e) {
                               return (e.kind == SectionEdit::Kind::TextureAdded ||
                                       e.kind == SectionEdit::Kind::TextureChanged) &&
                                      e.hash == fresh.hash;
                           }), list.end());
            }
            slot->second.push_back(std::move(fresh));
        }
    }
    plan.addedEdits.clear();

    // A mesh that kept its vertex count comes in as positions only. Written
    // on its own, as an overlay at the vertices' shipped file position, it
    // misses the game as soon as its section is served from a span in another
    // place -- so it goes into its section's rebuild instead: the object's
    // chunk with the positions written into its vertex block.
    std::vector<ImportPlan::Mesh> standalone;
    std::vector<MeshOverlayInfo> vertexOverlays;
    ListMeshOverlays(world.OverlayFolder(), vertexOverlays);
    for (ImportPlan::Mesh& mesh : plan.meshes) {
        ByteSpan object;
        u64 objectOffset = 0;
        u32 packOrder = 0;
        const size_t home = world.ModelHome(mesh.hash);
        const nfs::Section* section =
            home < world.Loaded().size() ? world.Loaded()[home]->info : nullptr;
        if (!section || world.IsLibrary(*world.Loaded()[home]) ||
            !FindModelChunk(world, mesh.hash, object, objectOffset, &packOrder)) {
            standalone.push_back(std::move(mesh));
            continue;
        }

        nfs::Chunk wrapper;
        wrapper.id = nfs::kChunkSolidObject;
        wrapper.headerOffset = objectOffset;
        wrapper.raw = ByteSpan{ object.data + 8, object.size - 8 };
        wrapper.data = wrapper.raw;
        const nfs::Chunk container = nfs::FindChunk(wrapper, nfs::kChunkMeshContainer);
        const nfs::Chunk vertices = container.id ? nfs::FindChunk(container, nfs::kChunkMeshVertices)
                                                 : nfs::Chunk{};
        const size_t count = mesh.positions.size();
        if (!vertices.id || count == 0 || vertices.data.size / count < 12) {
            standalone.push_back(std::move(mesh));
            continue;
        }
        const size_t stride = vertices.data.size / count;
        const size_t at = (size_t)(vertices.data.data - object.data);

        SectionEdit edit;
        edit.kind = SectionEdit::Kind::MeshChanged;
        edit.hash = mesh.hash;
        edit.label = mesh.model;
        edit.order = packOrder;
        edit.mod128 = (u32)(objectOffset % 128);
        edit.chunk.assign(object.data, object.data + object.size);
        for (size_t v = 0; v < count; ++v)
            std::memcpy(edit.chunk.data() + at + v * stride, &mesh.positions[v], 12);

        auto slot = std::find_if(incomingBySection.begin(), incomingBySection.end(),
                                 [&](const auto& entry) { return entry.first == section->name; });
        if (slot == incomingBySection.end()) {
            incomingBySection.push_back({ section->name, {} });
            slot = incomingBySection.end() - 1;
        }
        slot->second.push_back(std::move(edit));

        // The overlay that carried this mesh before is folded in and goes.
        for (const MeshOverlayInfo& overlay : vertexOverlays)
            if (overlay.hash == mesh.hash) plan.superseded.push_back(overlay.path);
    }
    plan.meshes = std::move(standalone);

    for (auto& [sectionName, incoming] : incomingBySection) {
        // A vertex overlay an older import left for one of these meshes would
        // be laid over the new mesh by the editor: it goes.
        for (const SectionEdit& fresh : incoming)
            if (fresh.kind == SectionEdit::Kind::MeshChanged)
                for (const MeshOverlayInfo& overlay : vertexOverlays)
                    if (overlay.hash == fresh.hash) plan.superseded.push_back(overlay.path);

        const nfs::Section* section = world.Table().FindByName(sectionName);
        std::vector<SectionEdit> current;
        std::wstring error;
        if (!section || !DiffSection(world, *section, current, error)) {
            report << L"  section " << std::wstring(sectionName.begin(), sectionName.end())
                   << L": " << error << L"\n";
            continue;
        }

        const auto replacedBy = [&](const SectionEdit& old) {
            const auto isObject = [](const SectionEdit& e) {
                return e.kind == SectionEdit::Kind::MeshChanged ||
                       e.kind == SectionEdit::Kind::ModelAdded;
            };
            const auto isTexture = [](const SectionEdit& e) {
                return e.kind == SectionEdit::Kind::TextureAdded ||
                       e.kind == SectionEdit::Kind::TextureChanged;
            };
            for (const SectionEdit& fresh : incoming) {
                const bool object = isObject(fresh) && isObject(old);
                const bool texture = isTexture(fresh) && isTexture(old);
                if ((object || texture) && fresh.hash == old.hash) return true;
            }
            return false;
        };
        std::vector<SectionEdit> edits;
        for (SectionEdit& old : current)
            if (!replacedBy(old)) edits.push_back(std::move(old));
        for (SectionEdit& fresh : incoming) edits.push_back(std::move(fresh));

        SectionSpanPlan spans;
        if (!PlanSectionFromEdits(world, *section, edits, existing, spans, error)) {
            report << L"  section " << std::wstring(sectionName.begin(), sectionName.end())
                   << L": " << error << L"\n";
            continue;
        }
        for (SectionSpan& s2 : spans.spans) plan.sectionSpans.push_back(std::move(s2));
        for (std::wstring& p2 : spans.superseded) plan.superseded.push_back(std::move(p2));
        merged.push_back(sectionName);
    }
    // The raw rebuilds of merged sections, and their table fixes, give way.
    plan.spans.erase(std::remove_if(plan.spans.begin(), plan.spans.end(), [&](const ImportPlan::Span& s) {
                         if (!s.section.empty())
                             return std::find(merged.begin(), merged.end(), s.section) != merged.end();
                         for (const std::string& name : merged)
                             if (s.layoutFile && s.name.find("section " + name + " ") == 0) return true;
                         return false;
                     }), plan.spans.end());

    plan.rebuiltReport = rebuilds.str();
    plan.report = report.str();
    return !plan.Empty();
}

bool ApplyImport(const ImportPlan& plan, const std::wstring& gameFolder,
                 const std::wstring& region, ImportSummary& summary) {
    if (plan.Empty()) return false;

    summary.modelsFolder = gameFolder + L"\\models";

    for (const ImportPlan::Mesh& mesh : plan.meshes) {
        MeshOverlay overlay;
        overlay.positions = mesh.positions;

        overlay.header.hash        = mesh.hash;
        overlay.header.stride      = mesh.stride;
        overlay.header.vertexCount = (u32)mesh.positions.size();
        overlay.header.fileOffset  = mesh.fileOffset;

        for (size_t i = 0; i < region.size() && i < sizeof(overlay.header.region); ++i)
            overlay.header.region[i] = (char)region[i];
        for (size_t i = 0; i < mesh.model.size() && i + 1 < sizeof(overlay.header.name); ++i)
            overlay.header.name[i] = mesh.model[i];

        if (!WriteMeshOverlay(summary.modelsFolder, overlay)) continue;

        summary.verticesWritten += mesh.positions.size();
        ++summary.objectsWritten;
    }

    // Sections rebuilt from their edits, replacing the spans they supersede.
    if (!plan.sectionSpans.empty())
        summary.objectsWritten +=
            WriteSectionSpans(plan.sectionSpans, plan.superseded, summary.modelsFolder, region);

    // The rebuilt sections, and the table corrections that go with them.
    for (size_t i = 0; i < plan.spans.size(); ++i) {
        const ImportPlan::Span& span = plan.spans[i];

        wchar_t key[64];
        ::_snwprintf_s(key, _TRUNCATE, L"%s-span%02zu-%s", region.c_str(), i,
                       span.layoutFile ? L"table" : L"stream");

        if (WriteSpanOverlay(summary.modelsFolder, region, span.name, span.fileOffset,
                             span.layoutFile, span.bytes, key)) {
            summary.verticesWritten += 0;
            ++summary.objectsWritten;
        }
    }

    if (!plan.created.empty() && summary.objectsWritten > 0) RecordCreated(plan, region);
    summary.objectsCreated = plan.created.size();

    LOG_INFO("import: %zu overlays written into %S, %zu vertices in all",
             summary.objectsWritten, summary.modelsFolder.c_str(), summary.verticesWritten);
    return summary.objectsWritten > 0;
}
