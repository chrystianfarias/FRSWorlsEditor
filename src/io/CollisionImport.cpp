#include "io/CollisionImport.h"

#include "core/Log.h"
#include "io/MeshOverlay.h"
#include "io/SectionRebuild.h"
#include "io/SectionEdits.h"
#include "io/SectionSplice.h"
#include "nfs/Collision.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace {

// --- small file helpers ------------------------------------------------------

std::string ReadWholeFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{ L"." } : path.substr(0, slash);
}

std::wstring ReplaceExtension(const std::wstring& path, const wchar_t* extension) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        return path.substr(0, dot) + extension;
    return path + extension;
}

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }


// The inverse of the exporter's axis change: OBJ is Y-up.
Vec3 FromBlender(const Vec3& v) { return { v.x, -v.z, v.y }; }

// --- OBJ ---------------------------------------------------------------------

struct ObjFace {
    std::string       material;
    std::vector<Vec3> corners;   // OBJ space, as read
};

struct ObjCollisionObject {
    std::string          name;
    std::vector<ObjFace> faces;
};

// Only what a collision edit needs: positions, objects, materials and faces.
// Texture and normal indices are read past, and negative indices are relative
// to the end of the vertex list, as the format allows.
bool ReadCollisionObj(const std::wstring& path, std::vector<ObjCollisionObject>& out) {
    const std::string text = ReadWholeFile(path);
    if (text.empty()) return false;

    std::vector<Vec3> positions;
    std::string material;
    std::istringstream lines(text);
    std::string line;

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string tag;
        in >> tag;

        if (tag == "v") {
            Vec3 p{};
            in >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "o") {
            ObjCollisionObject object;
            in >> object.name;
            out.push_back(std::move(object));
        } else if (tag == "usemtl") {
            in >> material;
        } else if (tag == "f") {
            if (out.empty()) out.push_back({ "unnamed", {} });
            ObjFace face;
            face.material = material;
            std::string token;
            while (in >> token) {
                const long index = std::strtol(token.c_str(), nullptr, 10);
                const long resolved = index < 0 ? (long)positions.size() + index : index - 1;
                if (resolved < 0 || resolved >= (long)positions.size()) return false;
                face.corners.push_back(positions[(size_t)resolved]);
            }
            if (face.corners.size() >= 3) out.back().faces.push_back(std::move(face));
        }
    }
    return !out.empty();
}

// --- manifest ----------------------------------------------------------------

struct ManifestObject {
    std::string      name;
    std::string      section;
    u32              pack = 0;
    size_t           packPolygons = 0;
    u32              packFingerprint = 0;   // 0 in manifests written before it existed
    std::vector<u32> polygons;
};

struct CollisionManifest {
    Vec3 origin{};
    std::vector<ManifestObject> objects;
};

bool FindString(const std::string& text, size_t begin, size_t end, const char* key,
                std::string& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted, begin);
    if (at == std::string::npos || at >= end) return false;
    const size_t open = text.find('"', text.find(':', at + quoted.size()) + 1);
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
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
    value = std::strtod(first, &last);
    return last != first;
}

bool FindNumbers(const std::string& text, size_t begin, size_t end, const char* key,
                 std::vector<double>& values) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted, begin);
    if (at == std::string::npos || at >= end) return false;
    const size_t open = text.find('[', at + quoted.size());
    const size_t close = open == std::string::npos ? open : text.find(']', open);
    if (close == std::string::npos || close > end) return false;

    values.clear();
    const char* cursor = text.c_str() + open + 1;
    const char* stop = text.c_str() + close;
    while (cursor < stop) {
        char* last = nullptr;
        const double parsed = std::strtod(cursor, &last);
        if (last == cursor) { ++cursor; continue; }
        values.push_back(parsed);
        cursor = last;
    }
    return true;
}

bool ReadCollisionManifest(const std::wstring& path, CollisionManifest& out) {
    const std::string text = ReadWholeFile(path);
    std::string kind;
    if (text.empty() || !FindString(text, 0, text.size(), "kind", kind) || kind != "collision")
        return false;

    std::vector<double> origin;
    if (!FindNumbers(text, 0, text.size(), "origin", origin) || origin.size() < 3) return false;
    out.origin = { (float)origin[0], (float)origin[1], (float)origin[2] };

    const size_t list = text.find("\"objects\"");
    if (list == std::string::npos) return false;

    // Object entries hold arrays but no nested objects, so braces delimit them.
    size_t cursor = list;
    while (true) {
        const size_t open = text.find('{', cursor);
        if (open == std::string::npos) break;
        const size_t close = text.find('}', open);
        if (close == std::string::npos) break;

        ManifestObject object;
        double pack = 0, packPolygons = 0;
        std::vector<double> polygons;
        if (FindString(text, open, close, "name", object.name) &&
            FindString(text, open, close, "section", object.section) &&
            FindNumber(text, open, close, "pack", pack) &&
            FindNumber(text, open, close, "packPolygons", packPolygons) &&
            FindNumbers(text, open, close, "polygons", polygons)) {
            object.pack = (u32)pack;
            object.packPolygons = (size_t)packPolygons;
            std::string fingerprint;
            if (FindString(text, open, close, "packFingerprint", fingerprint))
                object.packFingerprint = (u32)std::strtoul(fingerprint.c_str(), nullptr, 16);
            for (double p : polygons) object.polygons.push_back((u32)p);
            out.objects.push_back(std::move(object));
        }
        cursor = close + 1;
    }
    return !out.objects.empty();
}

// Blender appends ".001" when two objects share a name.
std::string BaseName(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < name.size() &&
        std::all_of(name.begin() + (long)dot + 1, name.end(), ::isdigit))
        return name.substr(0, dot);
    return name;
}

const ManifestObject* FindObject(const CollisionManifest& manifest, const std::string& name) {
    for (const ManifestObject& object : manifest.objects)
        if (object.name == name || object.name == BaseName(name)) return &object;
    return nullptr;
}

// The manifest beside the OBJ, or any collision manifest in the folder that
// names every object in it -- Blender exports under whatever name it is given.
bool FindCollisionManifest(const std::wstring& objPath,
                           const std::vector<ObjCollisionObject>& objects,
                           CollisionManifest& out) {
    const auto covers = [&](const CollisionManifest& manifest) {
        return std::all_of(objects.begin(), objects.end(), [&](const ObjCollisionObject& o) {
            return FindObject(manifest, o.name) != nullptr;
        });
    };

    CollisionManifest beside;
    if (ReadCollisionManifest(ReplaceExtension(objPath, L".json"), beside) && covers(beside)) {
        out = std::move(beside);
        return true;
    }

    const std::wstring folder = DirectoryOf(objPath);
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*.json").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;

    bool matched = false;
    do {
        CollisionManifest candidate;
        if (ReadCollisionManifest(folder + L"\\" + found.cFileName, candidate) &&
            covers(candidate)) {
            out = std::move(candidate);
            matched = true;
            LOG_INFO("collision import: using the manifest %S", found.cFileName);
            break;
        }
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
    return matched;
}

// --- faces into polygons -----------------------------------------------------

bool ParseMaterial(const std::string& name, bool& barrier, u8 attributes[4]) {
    unsigned b[4];
    char kind[16] = {};
    if (std::sscanf(name.c_str(), "%15[a-z]_%2x_%2x_%2x_%2x", kind, &b[0], &b[1], &b[2], &b[3]) != 5)
        return false;
    const std::string k = kind;
    if (k != "ground" && k != "barrier") return false;
    barrier = k == "barrier";
    for (int i = 0; i < 4; ++i) attributes[i] = (u8)b[i];
    return true;
}

double Cross2(const Vec3& o, const Vec3& a, const Vec3& b) {
    return (double)(a.x - o.x) * (b.y - o.y) - (double)(a.y - o.y) * (b.x - o.x);
}

double PlanArea(const std::vector<Vec3>& corners) {
    double twice = 0.0;
    for (size_t i = 0; i < corners.size(); ++i) {
        const Vec3& a = corners[i];
        const Vec3& b = corners[(i + 1) % corners.size()];
        twice += (double)a.x * b.y - (double)b.x * a.y;
    }
    return twice * 0.5;
}

nfs::CollisionPolygon MakeGround(const std::vector<Vec3>& corners, const u8 attributes[4]) {
    nfs::CollisionPolygon polygon;
    polygon.quad = corners.size() == 4;
    for (int c = 0; c < 4; ++c)
        polygon.corners[c] = corners[(size_t)std::min(c, (int)corners.size() - 1)];
    std::memcpy(polygon.attributes, attributes, 4);

    // The two flag bits the shape itself decides: quad, and vertical -- every
    // shipped polygon with no area in plan carries 0x02, and no other does.
    const bool vertical = std::fabs(PlanArea(corners)) < 1e-3;
    polygon.attributes[3] = (u8)((attributes[3] & ~0x12) | (polygon.quad ? 0x10 : 0) |
                                 (vertical ? 0x02 : 0));
    polygon.SyncFromAttributes();
    return polygon;
}

struct FaceStats {
    size_t ground = 0, barriers = 0, splitQuads = 0, splitNgons = 0;
    size_t unknownMaterial = 0, dropped = 0;
};

// Appends the polygons one OBJ face stands for.
void FaceToPolygons(const ObjFace& face, const Vec3& origin, const u8 fallback[4],
                    std::vector<nfs::CollisionPolygon>& out, FaceStats& stats) {
    std::vector<Vec3> corners;
    for (const Vec3& p : face.corners) {
        const Vec3 game = FromBlender(p) + origin;
        if (corners.empty() || Length(game - corners.back()) > 1e-4f) corners.push_back(game);
    }
    while (corners.size() > 1 && Length(corners.front() - corners.back()) <= 1e-4f)
        corners.pop_back();

    bool barrier = false;
    u8 attributes[4];
    if (!ParseMaterial(face.material, barrier, attributes)) {
        ++stats.unknownMaterial;
        std::memcpy(attributes, fallback, 4);
    }

    if (barrier) {
        // A wall runs along the direction the face's normal and "up" agree on:
        // the export wrote it bottom-start, top-start, top-end, bottom-end, so
        // normal = up x direction, and direction = normal x up.
        Vec3 normal{};
        for (size_t i = 0; i < corners.size(); ++i) {
            const Vec3& a = corners[i];
            const Vec3& b = corners[(i + 1) % corners.size()];
            normal.x += (a.y - b.y) * (a.z + b.z);
            normal.y += (a.z - b.z) * (a.x + b.x);
            normal.z += (a.x - b.x) * (a.y + b.y);
        }
        Vec3 direction{ normal.y, -normal.x, 0.0f };

        const Vec3* start = nullptr;
        const Vec3* end = nullptr;
        if (Length(direction) > 1e-6f) {
            direction = Normalize(direction);
            float lo = 1e30f, hi = -1e30f;
            for (const Vec3& p : corners) {
                const float d = p.x * direction.x + p.y * direction.y;
                if (d < lo) { lo = d; start = &p; }
                if (d > hi) { hi = d; end = &p; }
            }
        } else {
            // Lying flat, so there is no wall direction to read: the two
            // corners farthest apart are the best guess.
            float best = -1.0f;
            for (const Vec3& a : corners)
                for (const Vec3& b : corners) {
                    const float d = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
                    if (d > best) { best = d; start = &a; end = &b; }
                }
        }

        if (!start || !end ||
            std::hypot(start->x - end->x, start->y - end->y) < 0.125f) {
            ++stats.dropped;
            return;
        }

        nfs::CollisionPolygon polygon;
        polygon.barrier = true;
        polygon.quad = true;
        polygon.corners[0] = polygon.corners[1] = Vec3{ start->x, start->y, 0.0f };
        polygon.corners[2] = polygon.corners[3] = Vec3{ end->x, end->y, 0.0f };
        std::memcpy(polygon.attributes, attributes, 4);
        polygon.SyncFromAttributes();
        out.push_back(polygon);
        ++stats.barriers;
        return;
    }

    if (corners.size() < 3) { ++stats.dropped; return; }

    if (corners.size() == 3) {
        out.push_back(MakeGround(corners, attributes));
        ++stats.ground;
        return;
    }

    if (corners.size() == 4) {
        // Convex in plan (or standing on edge) stays a quad. A reflex corner
        // would give the game a shape its point-in-polygon test gets wrong, so
        // the quad is cut through that corner instead.
        int reflex = -1, positive = 0, negative = 0;
        for (int i = 0; i < 4; ++i) {
            const double turn = Cross2(corners[(size_t)i], corners[(size_t)(i + 1) % 4],
                                       corners[(size_t)(i + 2) % 4]);
            if (turn > 1e-6) ++positive;
            else if (turn < -1e-6) ++negative;
        }
        if (positive && negative) {
            const bool counterClockwise = PlanArea(corners) > 0.0;
            for (int i = 0; i < 4; ++i) {
                const double turn = Cross2(corners[(size_t)(i + 3) % 4], corners[(size_t)i],
                                           corners[(size_t)(i + 1) % 4]);
                if (counterClockwise ? turn < 0.0 : turn > 0.0) { reflex = i; break; }
            }
        }
        if (reflex < 0) {
            out.push_back(MakeGround(corners, attributes));
            ++stats.ground;
            return;
        }
        const auto at = [&](int k) { return corners[(size_t)((reflex + k) % 4)]; };
        out.push_back(MakeGround({ at(0), at(1), at(2) }, attributes));
        out.push_back(MakeGround({ at(0), at(2), at(3) }, attributes));
        stats.ground += 2;
        ++stats.splitQuads;
        return;
    }

    // Anything larger is fanned into triangles. Blender's n-gons from an
    // extrude or a fill are convex, which is what a fan needs.
    for (size_t i = 1; i + 1 < corners.size(); ++i) {
        out.push_back(MakeGround({ corners[0], corners[i], corners[i + 1] }, attributes));
        ++stats.ground;
    }
    ++stats.splitNgons;
}

} // namespace

bool IsCollisionObj(const std::wstring& objPath) {
    std::vector<ObjCollisionObject> objects;
    if (!ReadCollisionObj(objPath, objects)) return false;
    return std::all_of(objects.begin(), objects.end(), [](const ObjCollisionObject& o) {
        return o.name.rfind("collision_", 0) == 0;
    });
}

bool PlanCollisionImport(const nfs::World& world, const std::wstring& objPath,
                         const std::wstring& modelsFolder, CollisionImportPlan& plan,
                         const std::map<std::string, nfs::CollisionPack>* bases) {
    std::wostringstream report;

    std::vector<ObjCollisionObject> objects;
    if (!ReadCollisionObj(objPath, objects)) {
        plan.report = L"That OBJ could not be read, or a face points at a vertex it does not have.";
        return false;
    }
    plan.objectsRead = objects.size();

    CollisionManifest manifest;
    if (!FindCollisionManifest(objPath, objects, manifest)) {
        plan.report = L"No collision manifest in that folder names the objects in this OBJ.\n\n"
                      L"Keep the .json the editor wrote when it exported the collision.";
        return false;
    }

    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(modelsFolder, world.Region(), existing);

    const ByteSpan stream = world.StreamBytes();

    for (const ObjCollisionObject& object : objects) {
        const std::wstring objectName = Widen(object.name);
        const ManifestObject* entry = FindObject(manifest, object.name);
        const nfs::Section* section = entry ? world.Table().FindByName(entry->section) : nullptr;

        const nfs::LoadedSection* loaded = nullptr;
        for (const auto& candidate : world.Loaded())
            if (section && candidate->info == section) loaded = candidate.get();

        if (!entry || !section || !loaded || entry->pack >= loaded->collision.size()) {
            report << objectName << L": its section's collision is not loaded, left alone.\n\n";
            continue;
        }

        // Which state of the pack the export was taken from. Usually the one on
        // screen; but an edit made again from the same export -- the common
        // way to fix something after trying it in the game -- was taken from
        // the shipped pack, and then that is the base, and the earlier
        // collision edit of this section is replaced rather than stacked on.
        const auto matches = [&](const nfs::CollisionPack& candidate) {
            if (entry->packFingerprint)
                return nfs::CollisionFingerprint(candidate) == entry->packFingerprint;
            return candidate.polygons.size() == entry->packPolygons;
        };

        const nfs::CollisionPack* base = nullptr;
        nfs::CollisionPack shipped;
        bool replacesEarlierEdit = false;

        if (bases && bases->count(object.name)) {
            base = &bases->at(object.name);
        } else if (matches(loaded->collision[entry->pack])) {
            base = &loaded->collision[entry->pack];
        } else if (section->originalStreamOffset + (u64)section->originalStreamSize <= stream.size) {
            const ByteSpan original =
                stream.sub(section->originalStreamOffset, section->originalStreamSize);
            nfs::ChunkReader shippedReader(original, section->originalStreamOffset);
            nfs::Chunk chunk;
            u32 seenShipped = 0;
            while (shippedReader.Next(chunk)) {
                if (chunk.id != nfs::kChunkCollisionPack) continue;
                nfs::CollisionPack candidate;
                if (!nfs::ParseCollision(chunk, candidate)) continue;
                if (seenShipped++ != entry->pack) continue;
                if (matches(candidate)) {
                    shipped = std::move(candidate);
                    base = &shipped;
                    replacesEarlierEdit = true;
                }
                break;
            }
        }

        if (!base) {
            report << objectName << L": the section's collision is neither the state this "
                      L"export was taken from nor the shipped one. Export it again and redo "
                      L"the edit on that. Left alone.\n\n";
            continue;
        }
        const nfs::CollisionPack& current = *base;

        // The slots being replaced, and the attributes a face with an unknown
        // material falls back to: the commonest ground among them.
        std::vector<u32> slots(entry->polygons.begin(), entry->polygons.end());
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        if (!slots.empty() && slots.back() >= current.polygons.size()) {
            report << objectName << L": the manifest names polygons the pack does not have, "
                                     L"left alone.\n\n";
            continue;
        }

        u8 fallback[4] = { 0x00, 0x00, 0x01, 0x00 };
        {
            std::map<u32, size_t> tally;
            for (u32 slot : slots) {
                const nfs::CollisionPolygon& p = current.polygons[slot];
                if (p.barrier) continue;
                u32 key;
                std::memcpy(&key, p.attributes, 4);
                ++tally[key];
            }
            size_t best = 0;
            for (const auto& [key, n] : tally)
                if (n > best) { best = n; std::memcpy(fallback, &key, 4); }
        }

        std::vector<nfs::CollisionPolygon> faces;
        FaceStats stats;
        for (const ObjFace& face : object.faces)
            FaceToPolygons(face, manifest.origin, fallback, faces, stats);

        // A barrier only stops a car on one side: across every shipped region
        // the road lies to the left of the segment's direction in 98% of them.
        // A wall drawn in Blender is meant to stop cars from wherever they come,
        // so each one goes in both directions. Segments already present in
        // either direction -- an export of walls made this way -- are kept once.
        std::vector<nfs::CollisionPolygon> replacements;
        size_t twinned = 0;
        {
            std::vector<std::pair<std::pair<long, long>, std::pair<long, long>>> seen;
            const auto key = [](const Vec3& p) {
                return std::make_pair(std::lround(p.x * 8.0f), std::lround(p.y * 8.0f));
            };
            const auto present = [&](const Vec3& a, const Vec3& b) {
                return std::find(seen.begin(), seen.end(), std::make_pair(key(a), key(b))) !=
                       seen.end();
            };

            for (const nfs::CollisionPolygon& polygon : faces) {
                if (!polygon.barrier) { replacements.push_back(polygon); continue; }
                const Vec3 a = polygon.corners[0], b = polygon.corners[2];
                if (!present(a, b)) {
                    replacements.push_back(polygon);
                    seen.push_back({ key(a), key(b) });
                }
            }
            const size_t count = replacements.size();
            for (size_t i = 0; i < count; ++i) {
                const nfs::CollisionPolygon polygon = replacements[i];
                if (!polygon.barrier) continue;
                const Vec3 a = polygon.corners[0], b = polygon.corners[2];
                if (present(b, a)) continue;
                nfs::CollisionPolygon twin = polygon;
                twin.corners[0] = twin.corners[1] = b;
                twin.corners[2] = twin.corners[3] = a;
                replacements.push_back(twin);
                seen.push_back({ key(b), key(a) });
                ++twinned;
            }
        }
        stats.barriers = 0;
        for (const nfs::CollisionPolygon& polygon : replacements) stats.barriers += polygon.barrier;

        // Replacements take over the exported slots in order, so the rest of
        // the pack -- and the grid order recovered for it -- stays as it was.
        nfs::CollisionPack pack = current;
        if (pack.gridRank.size() != pack.polygons.size()) {
            pack.gridRank.resize(pack.polygons.size());
            for (u32 i = 0; i < pack.gridRank.size(); ++i) pack.gridRank[i] = i;
        }
        u32 nextRank = pack.gridRank.empty()
                           ? 0 : *std::max_element(pack.gridRank.begin(), pack.gridRank.end()) + 1;

        const size_t shared = std::min(slots.size(), replacements.size());
        for (size_t k = 0; k < shared; ++k) pack.polygons[slots[k]] = replacements[k];
        for (size_t k = shared; k < replacements.size(); ++k) {
            pack.polygons.push_back(replacements[k]);
            pack.gridRank.push_back(nextRank++);
        }
        for (size_t k = slots.size(); k-- > shared;) {
            pack.polygons.erase(pack.polygons.begin() + slots[k]);
            pack.gridRank.erase(pack.gridRank.begin() + slots[k]);
        }

        std::vector<u8> chunk;
        std::wstring error;
        if (!nfs::EncodeCollision(pack, chunk, error)) {
            report << objectName << L": " << error << L" Left alone.\n\n";
            continue;
        }

        // --- the section around it --------------------------------------------
        const ByteSpan bytes = world.SectionBytes(*section);

        nfs::ChunkReader reader(bytes, section->streamOffset);
        nfs::Chunk top;
        u32 seen = 0, order = 0, allCollision = 0;
        bool found = false;
        while (reader.Next(top)) {
            if (top.id != nfs::kChunkCollisionPack || found) continue;
            const u32 position = allCollision++;
            // Counted the way the loader counts them: empty packs are skipped.
            nfs::CollisionPack probe;
            if (!nfs::ParseCollision(top, probe)) continue;
            if (seen++ != entry->pack) continue;
            order = position;
            found = true;
        }
        if (!found) {
            report << objectName << L": the collision chunk is not where the loader found it, "
                                     L"left alone.\n\n";
            continue;
        }

        // Rebuilt from the shipped section with every edit in force, this
        // pack's collision swapped for the new one.
        std::vector<SectionEdit> edits;
        std::wstring error2;
        if (!DiffSection(world, *section, edits, error2)) {
            report << objectName << L": " << error2 << L" Left alone.\n\n";
            continue;
        }
        edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const SectionEdit& e) {
                        return e.kind == SectionEdit::Kind::Collision && e.order == order;
                    }), edits.end());
        SectionEdit collisionEdit;
        collisionEdit.kind = SectionEdit::Kind::Collision;
        collisionEdit.order = order;
        collisionEdit.chunk = std::move(chunk);
        edits.push_back(std::move(collisionEdit));

        SectionSpanPlan spans;
        if (!PlanSectionFromEdits(world, *section, edits, existing, spans, error2)) {
            report << objectName << L": " << error2 << L" Left alone.\n\n";
            continue;
        }
        const u64 offset = spans.offset;
        const u32 size = spans.size;
        const bool tableChanged = spans.tableChanged;
        for (SectionSpan& span : spans.spans) plan.spans.push_back(std::move(span));
        for (std::wstring& path : spans.superseded)
            if (std::find(plan.superseded.begin(), plan.superseded.end(), path) == plan.superseded.end())
                plan.superseded.push_back(std::move(path));

        report << objectName << L": " << slots.size() << L" polygons replaced by "
               << replacements.size() << L" (" << stats.ground << L" ground, "
               << stats.barriers << L" barriers); the section now has "
               << pack.polygons.size() << L".";
        if (stats.splitQuads) report << L" " << stats.splitQuads << L" quads not convex in plan were cut in two.";
        if (stats.splitNgons) report << L" " << stats.splitNgons << L" faces with more than four corners were split into triangles.";
        if (stats.unknownMaterial) report << L" " << stats.unknownMaterial << L" faces had no collision material and took the section's commonest ground.";
        if (stats.dropped) report << L" " << stats.dropped << L" faces too small to keep were dropped.";
        if (twinned) report << L" " << twinned << L" walls were doubled to block from both sides.";
        if (replacesEarlierEdit) report << L" This replaces the earlier collision edit of the section.";
        if (offset != section->streamOffset)
            report << L" The section no longer fits where it was and moves past the end of the stream.";
        report << L"\n\n";

        LOG_INFO("collision import %s: %zu polygons -> %zu, section %zu -> %u bytes at %llu%s",
                 section->name.c_str(), slots.size(), replacements.size(), bytes.size, size,
                 offset, tableChanged ? " (table corrected)" : "");
    }

    plan.report = report.str();
    return !plan.Empty();
}

size_t ApplyCollisionImport(const CollisionImportPlan& plan, const std::wstring& modelsFolder,
                            const std::wstring& region) {
    return WriteSectionSpans(plan.spans, plan.superseded, modelsFolder, region);
}
