#include "io/ObjMesh.h"

#include "core/Log.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace {

// One corner of a face, as the OBJ writes it: separate indices into the three
// lists, any of which may be absent.
struct Corner {
    int position = 0;
    int uv       = 0;
    int normal   = 0;

    bool operator==(const Corner& other) const {
        return position == other.position && uv == other.uv && normal == other.normal;
    }
};

struct CornerHash {
    size_t operator()(const Corner& corner) const {
        // Three small integers into one key: the mix is arbitrary but spreads
        // the common case, where two of the three move together.
        size_t hash = (size_t)corner.position * 73856093u;
        hash ^= (size_t)corner.uv * 19349663u;
        hash ^= (size_t)corner.normal * 83492791u;
        return hash;
    }
};

// OBJ indices count from one, and a negative one counts back from the end of
// whatever has been read so far.
int Resolve(int index, size_t count) {
    if (index > 0) return index - 1;
    if (index < 0) return (int)count + index;
    return -1;
}

Corner ParseCorner(const std::string& text) {
    Corner corner;
    int* fields[3] = { &corner.position, &corner.uv, &corner.normal };

    size_t begin = 0;
    for (int field = 0; field < 3 && begin <= text.size(); ++field) {
        const size_t slash = text.find('/', begin);
        const std::string part =
            text.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);

        if (!part.empty()) *fields[field] = std::atoi(part.c_str());
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return corner;
}

} // namespace

namespace {

// The path out of a `map_Kd` line.
//
// The line may carry options before the path (`-s 1 1 1 wall.png`) and the path
// itself may contain spaces, so neither the first token nor the last one can be
// taken for it. Options are what can be recognised: each is a `-name` followed
// by numeric arguments, and whatever is left once they are gone is the path.
std::string PathFromMapLine(const std::string& rest) {
    const char* kBlank = " \t";

    std::string text = rest;
    const size_t begin = text.find_first_not_of(kBlank);
    if (begin == std::string::npos) return {};

    text = text.substr(begin);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t'))
        text.pop_back();

    while (!text.empty() && text[0] == '-') {
        size_t at = text.find(' ');
        if (at == std::string::npos) return {};
        at = text.find_first_not_of(kBlank, at);

        while (at != std::string::npos) {
            const size_t space = text.find(' ', at);
            const std::string token =
                text.substr(at, space == std::string::npos ? space : space - at);

            if (token.find_first_not_of("0123456789.-eE") != std::string::npos) break;
            if (space == std::string::npos) return {};
            at = text.find_first_not_of(kBlank, space);
        }

        if (at == std::string::npos) return {};
        text = text.substr(at);
    }

    return text;
}

// Just the name: no directory, no extension.
std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);

    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

std::string Trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t");
    const size_t end = text.find_last_not_of(" \t\r");
    return begin == std::string::npos ? std::string()
                                      : text.substr(begin, end - begin + 1);
}

// Where a map_Kd path points. Relative paths are taken from the MTL's own
// folder, which is what every exporter means by them -- Blender writes
// `../../Downloads/wall.jpg` for an image it found outside the model's folder.
std::wstring ResolveImage(const std::wstring& mtlPath, const std::string& path) {
    std::wstring wide;
    if (!path.empty()) {
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(),
                                               nullptr, 0);
        wide.assign((size_t)size, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), wide.data(), size);
    }
    for (wchar_t& c : wide) if (c == L'/') c = L'\\';

    const bool absolute = (wide.size() > 1 && wide[1] == L':') ||
                          (wide.size() > 1 && wide[0] == L'\\' && wide[1] == L'\\');
    if (!absolute) {
        const size_t slash = mtlPath.find_last_of(L"/\\");
        const std::wstring folder = slash == std::wstring::npos ? L"." : mtlPath.substr(0, slash);
        wide = folder + L"\\" + wide;
    }

    wchar_t full[MAX_PATH] = {};
    return ::GetFullPathNameW(wide.c_str(), MAX_PATH, full, nullptr) ? std::wstring(full) : wide;
}

} // namespace

bool ReadObjMaterials(const std::wstring& objPath,
                      std::unordered_map<std::string, std::string>& out,
                      std::unordered_set<std::string>* tgaMaterials,
                      std::unordered_map<std::string, std::wstring>* imageFiles,
                      std::unordered_map<std::string, u32>* diffuse) {
    // Beside the model, under the same name. The OBJ does name its library in a
    // `mtllib` line, but every exporter that writes one writes it here, and
    // following the name would mean opening the OBJ a second time to read it.
    std::wstring mtlPath = objPath;
    const size_t dot = mtlPath.find_last_of(L'.');
    const size_t slash = mtlPath.find_last_of(L"/\\");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        mtlPath = mtlPath.substr(0, dot);
    mtlPath += L".mtl";

    std::ifstream file(mtlPath);
    if (!file) return false;

    std::string line, current;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream stream(line);
        std::string keyword;
        stream >> keyword;

        if (keyword == "newmtl") {
            std::string name;
            std::getline(stream, name);
            current = Trim(name);

        } else if (keyword == "Kd" && !current.empty() && diffuse) {
            // Linear in the file, as Blender writes it; the texture it becomes
            // is sRGB like every other one.
            float rgb[3] = { 0.8f, 0.8f, 0.8f };
            stream >> rgb[0] >> rgb[1] >> rgb[2];
            u32 color = 0xFF000000u;
            for (int k = 0; k < 3; ++k) {
                const float v = std::pow(std::min(std::max(rgb[k], 0.0f), 1.0f), 1.0f / 2.2f);
                color |= (u32)(v * 255.0f + 0.5f) << (16 - 8 * k);
            }
            (*diffuse)[current] = color;

        } else if (keyword == "map_Kd" && !current.empty()) {
            std::string rest;
            std::getline(stream, rest);

            const std::string path = PathFromMapLine(rest);
            const std::string image = BaseName(path);
            // A TGA is how the editor exports the game's own textures, so a
            // material painted with one names a texture the game already has.
            if (tgaMaterials && path.size() > 4) {
                std::string extension = path.substr(path.size() - 4);
                for (char& c : extension) c = (char)::tolower((unsigned char)c);
                if (extension == ".tga") tgaMaterials->insert(current);
            }
            if (!image.empty()) {
                LOG_INFO("material '%s' is painted with '%s'",
                         current.c_str(), image.c_str());
                out.emplace(current, image);
                if (imageFiles) imageFiles->emplace(current, ResolveImage(mtlPath, path));
            }
        }
    }

    return true;
}

bool ReadObjMeshes(const std::wstring& path, std::vector<ObjMesh>& out,
                   std::wstring& error) {
    std::ifstream file(path);
    if (!file) {
        error = L"That OBJ could not be opened.";
        return false;
    }

    // The three lists are global to the file, not to an object, so they are
    // gathered once and indexed by every object in it.
    std::vector<Vec3> positions;
    std::vector<u32>  colors;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;

    // Faces before any `o` or `g` line -- a file from a tool that names
    // nothing -- are one object rather than none.
    ObjMesh current;
    current.name = "object";
    std::string currentMaterial;
    std::unordered_map<Corner, u16, CornerHash> seen;

    const auto finishGroup = [&]() {
        if (current.groups.empty()) return;
        ObjFaceGroup& group = current.groups.back();
        group.indexCount = (u32)current.indices.size() - group.firstIndex;
    };

    const auto finishObject = [&]() {
        finishGroup();
        if (!current.name.empty() && !current.indices.empty())
            out.push_back(std::move(current));
        current = ObjMesh{};
        seen.clear();
        currentMaterial.clear();
    };

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream stream(line);
        std::string keyword;
        stream >> keyword;

        if (keyword == "v") {
            Vec3 position;
            stream >> position.x >> position.y >> position.z;
            positions.push_back(position);

            // Three more numbers on the line are the vertex colour. Blender
            // writes them when its "Colors" option is on, and without them
            // there is nothing to carry the baked lighting back.
            float r = 1.0f, g = 1.0f, b = 1.0f;
            const bool hasColor = (bool)(stream >> r >> g >> b);
            const auto toByte = [](float v) {
                const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return (u32)(clamped * 255.0f + 0.5f);
            };
            colors.push_back(hasColor ? (0xFF000000u | (toByte(r) << 16) |
                                         (toByte(g) << 8) | toByte(b))
                                      : 0xFFFFFFFFu);

        } else if (keyword == "vn") {
            Vec3 normal;
            stream >> normal.x >> normal.y >> normal.z;
            normals.push_back(normal);

        } else if (keyword == "vt") {
            Vec2 uv;
            stream >> uv.x >> uv.y;
            uvs.push_back(uv);

        } else if (keyword == "o" || keyword == "g") {
            std::string name;
            stream >> name;
            if (keyword == "o" || current.name.empty()) {
                finishObject();
                current.name = name;
            }

        } else if (keyword == "usemtl") {
            // The rest of the line, not the first word. A material may be named
            // after a texture file, and a file may have a space in its name --
            // `usemtl upper body` read one word at a time becomes `upper`, and
            // the hash of that matches nothing in the pack.
            std::string material;
            std::getline(stream, material);
            const size_t begin = material.find_first_not_of(" 	");
            const size_t end   = material.find_last_not_of(" 	");
            material = begin == std::string::npos ? std::string()
                                                  : material.substr(begin, end - begin + 1);

            if (material == currentMaterial) continue;

            finishGroup();
            currentMaterial = material;

            ObjFaceGroup group;
            group.material   = material;
            group.firstIndex = (u32)current.indices.size();
            current.groups.push_back(group);

        } else if (keyword == "f") {
            std::vector<u16> corners;
            std::string token;
            while (stream >> token) {
                const Corner corner = ParseCorner(token);

                const auto found = seen.find(corner);
                if (found != seen.end()) {
                    corners.push_back(found->second);
                    continue;
                }

                if (current.positions.size() >= 0xFFFF) {
                    error = L"That mesh needs more than 65,535 vertices. The track "
                            L"streams index with sixteen bits, so it cannot be stored.";
                    return false;
                }

                const int p = Resolve(corner.position, positions.size());
                if (p < 0 || (size_t)p >= positions.size()) continue;

                const int t = Resolve(corner.uv, uvs.size());
                const int n = Resolve(corner.normal, normals.size());

                const u16 index = (u16)current.positions.size();
                current.positions.push_back(positions[(size_t)p]);
                current.colors.push_back((size_t)p < colors.size() ? colors[(size_t)p]
                                                                   : 0xFFFFFFFFu);
                current.uvs.push_back(t >= 0 && (size_t)t < uvs.size() ? uvs[(size_t)t]
                                                                      : Vec2{});
                current.normals.push_back(n >= 0 && (size_t)n < normals.size()
                                              ? normals[(size_t)n] : Vec3{ 0, 0, 1 });

                seen.emplace(corner, index);
                corners.push_back(index);
            }

            if (corners.size() < 3) continue;

            // A face with more than three corners becomes a fan. OBJ allows any
            // convex polygon and the game draws triangles only.
            if (current.groups.empty()) {
                ObjFaceGroup group;
                group.material   = currentMaterial;
                group.firstIndex = 0;
                current.groups.push_back(group);
            }

            for (size_t i = 1; i + 1 < corners.size(); ++i) {
                current.indices.push_back(corners[0]);
                current.indices.push_back(corners[i]);
                current.indices.push_back(corners[i + 1]);
            }
        }
    }

    finishObject();

    if (out.empty()) {
        error = L"That OBJ holds no faces.";
        return false;
    }

    LOG_INFO("read %zu meshes from the OBJ", out.size());
    return true;
}
