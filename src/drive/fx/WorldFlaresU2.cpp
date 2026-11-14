#include "drive/fx/WorldFlaresU2.h"

#include "drive/numerics/BWareMath.h"
#include "nfs/World.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace bb {

constexpr int WorldFlaresU2::PoolCapacity[2];

namespace {

struct ChunkRef {
    uint32_t id = 0;
    size_t offset = 0;   // of the header, within the span
    size_t size = 0;     // payload bytes
};

// The chunks directly inside [begin, end) of `s`.
std::vector<ChunkRef> Children(const ByteSpan& s, size_t begin, size_t end) {
    std::vector<ChunkRef> out;
    size_t o = begin;
    while (o + 8 <= end) {
        uint32_t id, size;
        std::memcpy(&id, s.data + o, 4);
        std::memcpy(&size, s.data + o + 4, 4);
        if (o + 8 + (size_t)size > end) break;
        out.push_back({ id, o, size });
        o += 8 + (size_t)size;
    }
    return out;
}

// Payload start aligned to 16 bytes the way the loaders do it ((chunk + 0x17) & ~0xF); the section spans start
// 2 KiB-aligned in the stream file, so alignment within the span is alignment in the file.
size_t Aligned16(const ChunkRef& c) { return (c.offset + 8 + 15) & ~(size_t)15; }

float F(const ByteSpan& s, size_t o) { float v; std::memcpy(&v, s.data + o, 4); return v; }
uint32_t U(const ByteSpan& s, size_t o) { uint32_t v; std::memcpy(&v, s.data + o, 4); return v; }

struct ModelMarkers {
    float pivot[3] = {};
    struct Marker { int32_t iparam; float pos[3]; };
    std::vector<Marker> markers;
};

// Solid lists 0x80134000 -> solids 0x80134010 -> header 0x134011 (+0x10 name hash, +0x40 pivot matrix,
// row 3 at +0x70) and position markers 0x13401A (0x50 each: hash, IParam0, FParam0, FParam1, matrix).
void ReadModelMarkers(const ByteSpan& s, std::unordered_map<uint32_t, ModelMarkers>& out) {
    for (const ChunkRef& list : Children(s, 0, s.size)) {
        if (list.id != 0x80134000u) continue;
        for (const ChunkRef& solid : Children(s, list.offset + 8, list.offset + 8 + list.size)) {
            if (solid.id != 0x80134010u) continue;
            uint32_t hash = 0;
            bool haveHeader = false;
            ModelMarkers mm;
            for (const ChunkRef& c : Children(s, solid.offset + 8, solid.offset + 8 + solid.size)) {
                const size_t d = Aligned16(c), end = c.offset + 8 + c.size;
                if (c.id == 0x134011u && d + 0x80 <= end) {
                    hash = U(s, d + 0x10);
                    mm.pivot[0] = F(s, d + 0x70);
                    mm.pivot[1] = F(s, d + 0x74);
                    mm.pivot[2] = F(s, d + 0x78);
                    haveHeader = true;
                } else if (c.id == 0x13401Au) {
                    for (size_t o = d; o + 0x50 <= end; o += 0x50) {
                        ModelMarkers::Marker m;
                        m.iparam = (int32_t)U(s, o + 4);
                        m.pos[0] = F(s, o + 0x40);
                        m.pos[1] = F(s, o + 0x44);
                        m.pos[2] = F(s, o + 0x48);
                        mm.markers.push_back(m);
                    }
                }
            }
            if (haveHeader && !mm.markers.empty()) out.emplace(hash, std::move(mm));
        }
    }
}

} // namespace

void WorldFlaresU2::Clear() {
    world_ = nullptr;
    loadedSections_ = 0;
    packs_.clear();
    markers_.clear();
    flareCount_ = 0;
}

size_t WorldFlaresU2::Load(const nfs::World& world) {
    Clear();
    world_ = &world;
    loadedSections_ = world.Loaded().size();
    std::unordered_map<uint32_t, ModelMarkers> models;
    for (const nfs::Section& section : world.Table().Sections()) {
        const ByteSpan s = world.SectionBytes(section);
        if (s.empty()) continue;
        ReadModelMarkers(s, models);
        for (const ChunkRef& top : Children(s, 0, s.size)) {
            if (top.id != PackChunk) continue;
            Pack pack;
            bool header = false;
            size_t count = 0;
            for (const ChunkRef& c : Children(s, top.offset + 8, top.offset + 8 + top.size)) {
                const size_t d = Aligned16(c), end = c.offset + 8 + c.size;
                if (c.id == HeaderChunk && d + 0x58 <= end) {
                    if (U(s, d + 0x08) != 3) break;   // sub_48E070 links version 3 only
                    for (int k = 0; k < 3; ++k) {
                        pack.min[k] = F(s, d + 0x30 + 4 * (size_t)k);
                        pack.max[k] = F(s, d + 0x40 + 4 * (size_t)k);
                    }
                    uint16_t n;
                    std::memcpy(&n, s.data + d + 0x50, 2);
                    count = n;
                    pack.section = U(s, d + 0x54);
                    header = true;
                } else if (c.id == FlaresChunk && header) {
                    for (size_t i = 0; i < count && d + 0x30 * (i + 1) <= end; ++i) {
                        const size_t o = d + 0x30 * i;
                        LightFlareU2 f;
                        f.NameHash = U(s, o + 0x08);
                        f.ColourTint = U(s, o + 0x0C);
                        f.X = F(s, o + 0x10); f.Y = F(s, o + 0x14); f.Z = F(s, o + 0x18);
                        f.ReflectPosZ = F(s, o + 0x1C);
                        f.DirX = F(s, o + 0x20); f.DirY = F(s, o + 0x24); f.DirZ = F(s, o + 0x28);
                        f.Type = (int8_t)s.data[o + 0x2C];
                        f.Flags = (int8_t)s.data[o + 0x2D];
                        pack.flares.push_back(f);
                    }
                }
            }
            if (header && !pack.flares.empty()) {
                flareCount_ += pack.flares.size();
                packs_.push_back(std::move(pack));
            }
        }
    }

    // B: every shipped scenery instance whose model has position markers.
    for (const auto& loaded : world.Loaded()) {
        if (!loaded || !loaded->info) continue;
        const nfs::Scenery& sc = loaded->scenery;
        for (const nfs::SceneryInstance& inst : sc.instances) {
            if (inst.flags & 0x10) continue;   // hidden in free roam
            const auto it = models.find(sc.ModelHash(inst));
            if (it == models.end()) continue;
            const nfs::SceneryInfo& info = sc.infos[inst.infoIndex];
            float radius = 0.0f;
            if (info.raw.size() >= 0x40) std::memcpy(&radius, info.raw.data() + 0x3C, 4);
            const Mat4& T = inst.transform;
            for (const ModelMarkers::Marker& m : it->second.markers) {
                const float lx = m.pos[0] - it->second.pivot[0], ly = m.pos[1] - it->second.pivot[1],
                            lz = m.pos[2] - it->second.pivot[2];
                MarkerFlare mf;
                LightFlareU2& f = mf.flare;
                f.X = lz * T.m[2][0] + ly * T.m[1][0] + lx * T.m[0][0] + T.m[3][0];
                f.Y = lz * T.m[2][1] + ly * T.m[1][1] + lx * T.m[0][1] + T.m[3][1];
                f.Z = lz * T.m[2][2] + ly * T.m[1][2] + lx * T.m[0][2] + T.m[3][2];
                f.Type = (int8_t)(m.iparam + 10);   // 0x5D6939
                if (m.iparam == 3) {
                    f.Flags = 4;
                    float dx = f.X - T.m[3][0], dy = f.Y - T.m[3][1];
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.0f) { dx /= len; dy /= len; } else { dx = 1.0f; dy = 0.0f; }
                    f.DirX = dx; f.DirY = dy; f.DirZ = 0.0f;
                } else {
                    f.Flags = 2;
                    f.DirX = 0.0f; f.DirY = 0.0f; f.DirZ = -1.0f;
                }
                mf.list = m.iparam == 3 ? 1 : 0;
                mf.section = loaded->info->number;
                mf.origin[0] = T.m[3][0]; mf.origin[1] = T.m[3][1]; mf.origin[2] = T.m[3][2];
                mf.radius = radius;
                mf.bounds = inst.worldBounds;
                markers_.push_back(mf);
            }
        }
    }
    flareCount_ += markers_.size();
    return flareCount_;
}

bool WorldFlaresU2::BoxVisible(const FlareViewU2& v, const float* mn, const float* mx) {
    for (int i = 0; i < 6; ++i) {
        const float* q = v.Planes + 4 * i;
        const float x = q[0] >= 0.0f ? mx[0] : mn[0];
        const float y = q[1] >= 0.0f ? mx[1] : mn[1];
        const float z = q[2] >= 0.0f ? mx[2] : mn[2];
        if (q[0] * x + q[1] * y + q[2] * z + q[3] < 0.0f) return false;
    }
    return true;
}

bool WorldFlaresU2::SectionVisible(uint32_t section, const std::vector<u32>* visible) const {
    if (!visible) return true;
    for (u32 n : *visible)
        if (n == section) return true;
    return false;
}

void WorldFlaresU2::Build(const FlareViewU2& view, const std::vector<u32>* visibleSections, bool blinkOn,
                          const PlayerCar* car, std::vector<Quad>& out) const {
    out.clear();
    static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    FlarePolyU2 polys[2];
    // A: sub_491860, flareMode 0, intensity 1, no colour override.
    for (const Pack& p : packs_) {
        if (!SectionVisible(p.section, visibleSections)) continue;
        if (!BoxVisible(view, p.min, p.max)) continue;
        for (const LightFlareU2& f : p.flares) {
            const int n = CarLightFlaresU2::Render(view, f, identity, 1.0f, 0, table_, polys, 2);
            for (int k = 0; k < n && k < 2; ++k) out.push_back({ polys[k], false });
        }
    }
    // B: the pools, filled in section order by the scenery pass, drawn by sub_494270: list 0 then list 1.
    int used[2] = { 0, 0 };
    std::vector<const MarkerFlare*> pool[2];
    for (const MarkerFlare& m : markers_) {
        if (!SectionVisible(m.section, visibleSections)) continue;
        if (m.bounds.Valid()) {
            const float mn[3] = { m.bounds.min.x, m.bounds.min.y, m.bounds.min.z };
            const float mx[3] = { m.bounds.max.x, m.bounds.max.y, m.bounds.max.z };
            if (!BoxVisible(view, mn, mx)) continue;
        }
        // sub_5D64D0 >= 0x11: the model is drawn at all (inferred: GetPixelSize with radius + 6).
        if (CarLightFlaresU2::GetPixelSize(view, m.origin, m.radius + 6.0f) < 0x11) continue;
        if (used[m.list] >= PoolCapacity[m.list]) continue;   // sub_48AB20: pool full
        used[m.list]++;
        pool[m.list].push_back(&m);
    }
    for (int list = 0; list < 2; ++list)
        for (const MarkerFlare* m : pool[list]) {
            const LightFlareU2& f = m->flare;
            if (f.Type == 13 && list == 1 && !blinkOn) continue;   // 0x4942F5
            float intensity = 1.0f;
            if (f.Type != 13 && f.Type != 14) {
                // flareMode 3 (0x490DA4..0x490E46): lit only inside the player car's headlight cone.
                if (!car) continue;
                float d[3] = { f.X - car->position[0], f.Y - car->position[1], f.Z - car->position[2] };
                BWareMath::Normalize(d, d);
                const float c = d[1] * car->forward[1] + d[2] * car->forward[2] + d[0] * car->forward[0];
                if (c < rain_) continue;
                intensity = (c - rain_) / (1.0f - rain_);
            }
            const int n = CarLightFlaresU2::Render(view, f, identity, intensity, 0, table_, polys, 2, false);
            for (int k = 0; k < n && k < 2; ++k) out.push_back({ polys[k], true });
        }
}

int WorldFlaresU2::Draw(fx::StateGuard& guard, fx::PolyRenderer& polys, const FlareViewU2& view,
                        fx::GlobalTextures& globals, IDirect3DDevice9* device, const std::vector<u32>* visibleSections,
                        bool blinkOn, const PlayerCar* car) {
    Build(view, visibleSections, blinkOn, car, scratch_);
    for (const Quad& q : scratch_) {
        const FlarePolyU2& p = q.poly;
        const int tex = p.TexIndex < 0 || p.TexIndex > 2 ? 2 : p.TexIndex;
        const fx::FxTexture& t = globals.Get(device, CarLightFlaresU2::TextureHashes[tex]);
        const D3DCOLOR c = fx::PolyColour(p.Colour);
        const fx::PolyRenderer::Vertex v[4] = {
            { p.X0, p.Y0, p.Z0, c, 0.0f, 0.0f },
            { p.X1, p.Y1, p.Z1, c, 1.0f, 0.0f },
            { p.X2, p.Y2, p.Z2, c, 1.0f, 1.0f },
            { p.X3, p.Y3, p.Z3, c, 0.0f, 1.0f },
        };
        polys.Draw(guard, t, D3DPT_TRIANGLEFAN, v, 4);
    }
    return (int)scratch_.size();
}

std::string WorldFlaresU2::Describe() const {
    size_t packFlares = 0;
    int types[16] = {};
    for (const Pack& p : packs_)
        for (const LightFlareU2& f : p.flares) {
            ++packFlares;
            if (f.Type >= 0 && f.Type < 16) types[f.Type]++;
        }
    int markerTypes[16] = {};
    for (const MarkerFlare& m : markers_)
        if (m.flare.Type >= 0 && m.flare.Type < 16) markerTypes[m.flare.Type]++;
    char line[512];
    std::snprintf(line, sizeof line,
                  "world flares: %zu packs / %zu flares (type 9: %d), %zu scenery marker flares (types 10..14: %d %d %d %d "
                  "%d), table %s",
                  packs_.size(), packFlares, types[9], markers_.size(), markerTypes[10], markerTypes[11], markerTypes[12],
                  markerTypes[13], markerTypes[14], haveExeTable_ ? "from SPEED2.EXE" : "defaults");
    return line;
}

} // namespace bb
