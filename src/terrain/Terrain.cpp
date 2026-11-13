#include "terrain/Terrain.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace terrain {

namespace {

constexpr int kChunk = 16;     // cells a side in one baked piece

float Smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

const wchar_t* BrushName(Brush brush) {
    switch (brush) {
        case Brush::Raise:   return L"raise";
        case Brush::Lower:   return L"lower";
        case Brush::Smooth:  return L"smooth";
        case Brush::Flatten: return L"flatten";
        case Brush::Paint:   return L"paint";
        case Brush::AddArea:   return L"add area";
        case Brush::EraseArea: return L"erase area";
        default:             return L"off";
    }
}

Aabb Patch::Bounds() const {
    Aabb box;
    for (int j = 0; j <= rows; ++j)
        for (int i = 0; i <= columns; ++i) box.Add(Corner(i, j));
    return box;
}

Patch* Terrain::Find(u32 id) {
    for (Patch& p : patches_)
        if (p.id == id) return &p;
    return nullptr;
}

const Patch* Terrain::Find(u32 id) const {
    for (const Patch& p : patches_)
        if (p.id == id) return &p;
    return nullptr;
}

u32 Terrain::Add(const Vec3& centre, float size, float cell) {
    Patch patch;
    patch.id = nextId_++;
    patch.cell = std::clamp(cell, 0.5f, 64.0f);
    patch.columns = patch.rows = std::clamp((int)std::lround(size / patch.cell), 2, 512);
    patch.origin = { centre.x - patch.columns * patch.cell * 0.5f, centre.y - patch.rows * patch.cell * 0.5f };
    patch.heights.assign((size_t)(patch.columns + 1) * (patch.rows + 1), centre.z);
    patch.paint.assign((size_t)patch.columns * patch.rows, 0);
    patch.layers.push_back(Layer{});
    Layer road;
    road.texture = "RDP_INTERSECTMIDTILE";
    road.tile = 16.0f;
    road.surface = 0x01;
    patch.layers.push_back(road);
    Layer walk;
    walk.texture = "ARC_SIDEWALKPAVE1";
    walk.tile = 8.0f;
    walk.surface = 0x10;
    patch.layers.push_back(walk);
    patches_.push_back(std::move(patch));
    ++revision_;
    return patches_.back().id;
}

u32 Terrain::AddWhere(const Vec2& min, const Vec2& max, float cell, const std::function<bool(float, float)>& inside,
                      const HeightFn& height) {
    Patch patch;
    patch.cell = std::clamp(cell, 0.5f, 64.0f);
    patch.origin = { std::floor(min.x / patch.cell) * patch.cell, std::floor(min.y / patch.cell) * patch.cell };
    patch.columns = std::clamp((int)std::ceil((max.x - patch.origin.x) / patch.cell), 1, 1024);
    patch.rows = std::clamp((int)std::ceil((max.y - patch.origin.y) / patch.cell), 1, 1024);
    patch.heights.resize((size_t)(patch.columns + 1) * (patch.rows + 1));
    for (int j = 0; j <= patch.rows; ++j)
        for (int i = 0; i <= patch.columns; ++i)
            patch.H(i, j) = height ? height(patch.origin.x + i * patch.cell, patch.origin.y + j * patch.cell) : 0.0f;
    patch.paint.assign((size_t)patch.columns * patch.rows, kEmpty);
    size_t filled = 0;
    for (int j = 0; j < patch.rows; ++j)
        for (int i = 0; i < patch.columns; ++i)
            if (inside(patch.origin.x + (i + 0.5f) * patch.cell, patch.origin.y + (j + 0.5f) * patch.cell)) {
                patch.paint[(size_t)j * patch.columns + i] = 0;
                ++filled;
            }
    if (!filled) return 0;
    patch.id = nextId_++;
    patch.layers.push_back(Layer{});
    Layer road;
    road.texture = "RDP_INTERSECTMIDTILE";
    road.tile = 16.0f;
    road.surface = 0x01;
    patch.layers.push_back(road);
    patches_.push_back(std::move(patch));
    ++revision_;
    return patches_.back().id;
}

bool Terrain::Grow(Patch& patch, const Aabb& area, const HeightFn& height) {
    const int left = std::max(0, (int)std::ceil((patch.origin.x - area.min.x) / patch.cell));
    const int bottom = std::max(0, (int)std::ceil((patch.origin.y - area.min.y) / patch.cell));
    const int right = std::max(0, (int)std::ceil((area.max.x - (patch.origin.x + patch.columns * patch.cell)) / patch.cell));
    const int top = std::max(0, (int)std::ceil((area.max.y - (patch.origin.y + patch.rows * patch.cell)) / patch.cell));
    if (!left && !bottom && !right && !top) return false;
    const int columns = patch.columns + left + right, rows = patch.rows + bottom + top;
    if (columns > 2048 || rows > 2048) return false;
    const Vec2 origin{ patch.origin.x - left * patch.cell, patch.origin.y - bottom * patch.cell };
    std::vector<float> heights((size_t)(columns + 1) * (rows + 1));
    std::vector<u8> paint((size_t)columns * rows, kEmpty);
    for (int j = 0; j <= rows; ++j)
        for (int i = 0; i <= columns; ++i) {
            const int oi = i - left, oj = j - bottom;
            float& h = heights[(size_t)j * (columns + 1) + i];
            if (oi >= 0 && oj >= 0 && oi <= patch.columns && oj <= patch.rows) h = patch.H(oi, oj);
            else h = height ? height(origin.x + i * patch.cell, origin.y + j * patch.cell) : patch.heights.front();
        }
    for (int j = 0; j < patch.rows; ++j)
        for (int i = 0; i < patch.columns; ++i)
            paint[(size_t)(j + bottom) * columns + (i + left)] = patch.paint[(size_t)j * patch.columns + i];
    patch.origin = origin;
    patch.columns = columns;
    patch.rows = rows;
    patch.heights = std::move(heights);
    patch.paint = std::move(paint);
    ++revision_;
    return true;
}

void Terrain::Move(Patch& patch, const Vec3& by) {
    patch.origin.x += by.x;
    patch.origin.y += by.y;
    for (float& h : patch.heights) h += by.z;
    ++revision_;
}

void Terrain::Remove(u32 id) {
    patches_.erase(std::remove_if(patches_.begin(), patches_.end(), [&](const Patch& p) { return p.id == id; }),
                   patches_.end());
    ++revision_;
}

bool Terrain::Apply(Patch& patch, Brush brush, const Vec3& centre, float radius, float amount, float level,
                    u8 layer, const HeightFn& height) {
    if (brush == Brush::None || radius <= 0.0f) return false;
    if (brush == Brush::AddArea) {
        Aabb area;
        area.Add({ centre.x - radius, centre.y - radius, centre.z });
        area.Add({ centre.x + radius, centre.y + radius, centre.z });
        bool changed = Grow(patch, area, height);
        for (int j = 0; j < patch.rows; ++j)
            for (int i = 0; i < patch.columns; ++i) {
                const float x = patch.origin.x + (i + 0.5f) * patch.cell, y = patch.origin.y + (j + 0.5f) * patch.cell;
                if ((x - centre.x) * (x - centre.x) + (y - centre.y) * (y - centre.y) > radius * radius) continue;
                u8& cell = patch.paint[(size_t)j * patch.columns + i];
                if (cell == kEmpty) { cell = std::min<u8>(layer, (u8)(patch.layers.size() - 1)); changed = true; }
            }
        if (changed) ++revision_;
        return changed;
    }
    const int i0 = std::max(0, (int)std::floor((centre.x - radius - patch.origin.x) / patch.cell));
    const int i1 = std::min(patch.columns, (int)std::ceil((centre.x + radius - patch.origin.x) / patch.cell));
    const int j0 = std::max(0, (int)std::floor((centre.y - radius - patch.origin.y) / patch.cell));
    const int j1 = std::min(patch.rows, (int)std::ceil((centre.y + radius - patch.origin.y) / patch.cell));
    if (i0 > i1 || j0 > j1) return false;

    const auto weight = [&](float x, float y) {
        const float d2 = ((x - centre.x) * (x - centre.x) + (y - centre.y) * (y - centre.y)) / (radius * radius);
        return d2 >= 1.0f ? 0.0f : (1.0f - d2) * (1.0f - d2);
    };

    bool changed = false;
    if (brush == Brush::Paint || brush == Brush::EraseArea) {
        if (brush == Brush::Paint && layer >= patch.layers.size()) return false;
        for (int j = j0; j < std::min(j1, patch.rows); ++j)
            for (int i = i0; i < std::min(i1, patch.columns); ++i) {
                const float x = patch.origin.x + (i + 0.5f) * patch.cell, y = patch.origin.y + (j + 0.5f) * patch.cell;
                if (weight(x, y) < 0.05f) continue;
                u8& cell = patch.paint[(size_t)j * patch.columns + i];
                if (brush == Brush::EraseArea) {
                    if (cell != kEmpty) { cell = kEmpty; changed = true; }
                    continue;
                }
                if (cell != kEmpty && cell != layer) { cell = layer; changed = true; }
            }
        if (changed) ++revision_;
        return changed;
    }

    const std::vector<float> before = patch.heights;
    const auto old = [&](int i, int j) {
        i = std::clamp(i, 0, patch.columns);
        j = std::clamp(j, 0, patch.rows);
        return before[(size_t)j * (patch.columns + 1) + i];
    };
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const float w = weight(patch.origin.x + i * patch.cell, patch.origin.y + j * patch.cell);
            if (w <= 0.0f) continue;
            float& h = patch.H(i, j);
            switch (brush) {
                case Brush::Raise:   h += amount * w; break;
                case Brush::Lower:   h -= amount * w; break;
                case Brush::Flatten: h += (level - h) * std::min(1.0f, amount * w * 0.5f); break;
                case Brush::Smooth: {
                    const float average = (old(i - 1, j) + old(i + 1, j) + old(i, j - 1) + old(i, j + 1) +
                                           old(i, j) * 4.0f) / 8.0f;
                    h += (average - h) * std::min(1.0f, amount * w * 0.5f);
                    break;
                }
                default: break;
            }
            changed = true;
        }
    if (changed) ++revision_;
    return changed;
}

std::vector<roads::MeshPiece> Terrain::BuildMesh(const Patch& patch, const roads::RoadField* roads,
                                                 const std::vector<roads::Pole>& poles) const {
    std::vector<roads::MeshPiece> pieces;
    if (patch.layers.empty()) return pieces;
    const int stride = patch.columns + 1;

    // The ground as it will be: the sculpted heights, fitted around the roads.
    std::vector<float> z(patch.heights.size());
    std::vector<u8> underRoad(patch.heights.size(), 0);
    for (int j = 0; j <= patch.rows; ++j)
        for (int i = 0; i <= patch.columns; ++i) {
            const size_t k = (size_t)j * stride + i;
            const float x = patch.origin.x + i * patch.cell, y = patch.origin.y + j * patch.cell;
            float h = patch.heights[k];
            float roadZ = 0.0f, outside = 0.0f;
            if (patch.conform && roads && !roads->Empty() &&
                roads->Query(x, y, patch.shoulder, roadZ, outside)) {
                const float bed = roadZ - patch.sink;
                if (outside <= 0.0f) {
                    h = bed;
                    underRoad[k] = outside < -0.5f;
                } else {
                    h = bed + (h - bed) * Smoothstep(outside / std::max(0.01f, patch.shoulder));
                }
            }
            z[k] = h;
        }
    const auto at = [&](int i, int j) {
        return Vec3{ patch.origin.x + i * patch.cell, patch.origin.y + j * patch.cell, z[(size_t)j * stride + i] };
    };
    const auto normal = [&](int i, int j) {
        const float l = z[(size_t)j * stride + std::max(0, i - 1)];
        const float r = z[(size_t)j * stride + std::min(patch.columns, i + 1)];
        const float d = z[(size_t)std::max(0, j - 1) * stride + i];
        const float u = z[(size_t)std::min(patch.rows, j + 1) * stride + i];
        return Normalize(Vec3{ (l - r) / (2.0f * patch.cell), (d - u) / (2.0f * patch.cell), 1.0f });
    };

    for (int cj = 0; cj < patch.rows; cj += kChunk)
        for (int ci = 0; ci < patch.columns; ci += kChunk) {
            roads::MeshPiece piece;
            char name[64];
            std::snprintf(name, sizeof(name), "ROAD_T%u_%d_%d", patch.id, ci / kChunk, cj / kChunk);
            piece.name = name;
            for (int j = cj; j < std::min(patch.rows, cj + kChunk); ++j)
                for (int i = ci; i < std::min(patch.columns, ci + kChunk); ++i) {
                    // Under a road on every corner: the road covers it.
                    const size_t k00 = (size_t)j * stride + i;
                    if (underRoad[k00] && underRoad[k00 + 1] && underRoad[k00 + stride] &&
                        underRoad[k00 + stride + 1])
                        continue;
                    const u8 index = patch.paint[(size_t)j * patch.columns + i];
                    if (index == kEmpty) continue;
                    const Layer& layer = patch.layers[std::min<size_t>(index, patch.layers.size() - 1)];
                    roads::MeshGroup& group = piece.Group(layer.texture);
                    const u32 base = (u32)group.vertices.size();
                    const int corners[4][2] = { { i, j }, { i + 1, j }, { i + 1, j + 1 }, { i, j + 1 } };
                    roads::CollisionArea area;
                    area.surface = layer.surface;
                    for (const auto& c : corners) {
                        roads::MeshVertex v;
                        v.position = at(c[0], c[1]);
                        v.normal = normal(c[0], c[1]);
                        v.uv = { v.position.x / layer.tile, -v.position.y / layer.tile };
                        group.vertices.push_back(v);
                        area.outline.push_back(v.position);
                    }
                    // Counter-clockwise from above, split along the flatter diagonal.
                    const float d0 = std::fabs(at(i, j).z - at(i + 1, j + 1).z);
                    const float d1 = std::fabs(at(i + 1, j).z - at(i, j + 1).z);
                    if (d0 <= d1) {
                        for (u32 q : { 0u, 1u, 2u, 0u, 2u, 3u }) group.indices.push_back(base + q);
                    } else {
                        for (u32 q : { 0u, 1u, 3u, 1u, 2u, 3u }) group.indices.push_back(base + q);
                    }
                    piece.areas.push_back(std::move(area));
                }
            if (piece.VertexCount()) pieces.push_back(std::move(piece));
        }
    roads::ApplyLighting(pieces, poles, patch.ambient);
    return pieces;
}

// ---------------------------------------------------------------------------

bool Terrain::Save(const std::wstring& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "FRSTERRAIN 1\n";
    out << "# patch <id> <originX> <originY> <cell> <columns> <rows> <conform> <shoulder> <sink> <ambient>\n"
           "# layer <texture> <tile> <surface>, then a heights line and a paint line\n";
    char line[256];
    for (const Patch& p : patches_) {
        std::snprintf(line, sizeof(line), "patch %u %.4f %.4f %.4f %d %d %d %.3f %.3f %06X\n", p.id, p.origin.x,
                      p.origin.y, p.cell, p.columns, p.rows, p.conform ? 1 : 0, p.shoulder, p.sink,
                      p.ambient & 0xFFFFFF);
        out << line;
        for (const Layer& l : p.layers) {
            std::snprintf(line, sizeof(line), "layer %s %.3f %02X\n", l.texture.empty() ? "-" : l.texture.c_str(),
                          l.tile, l.surface);
            out << line;
        }
        out << "heights";
        for (float h : p.heights) {
            std::snprintf(line, sizeof(line), " %.3f", h);
            out << line;
        }
        out << "\npaint ";
        for (u8 c : p.paint) out << (c == kEmpty ? '.' : (char)('0' + std::min<u8>(c, 9)));
        out << "\n";
    }
    return (bool)out;
}

bool Terrain::Load(const std::wstring& path) {
    Clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text;
    u32 highest = 0;
    Patch* current = nullptr;
    while (std::getline(in, text)) {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        if (text.empty() || text[0] == '#') continue;
        std::istringstream line(text);
        std::string kind;
        line >> kind;
        if (kind == "patch") {
            Patch p;
            int conform = 1;
            std::string ambient;
            line >> p.id >> p.origin.x >> p.origin.y >> p.cell >> p.columns >> p.rows >> conform >> p.shoulder >>
                p.sink >> ambient;
            if (!line || p.columns < 1 || p.rows < 1 || p.columns > 2048 || p.rows > 2048) { current = nullptr; continue; }
            p.conform = conform != 0;
            p.ambient = (u32)std::strtoul(ambient.c_str(), nullptr, 16) & 0xFFFFFF;
            p.heights.assign((size_t)(p.columns + 1) * (p.rows + 1), 0.0f);
            p.paint.assign((size_t)p.columns * p.rows, 0);
            patches_.push_back(std::move(p));
            current = &patches_.back();
            highest = std::max(highest, current->id);
        } else if (current && kind == "layer") {
            Layer l;
            std::string texture, surface;
            line >> texture >> l.tile >> surface;
            if (!line) continue;
            l.texture = texture == "-" ? std::string() : texture;
            l.surface = (u8)std::strtoul(surface.c_str(), nullptr, 16);
            if (current->layers.size() < (size_t)kMaxLayers) current->layers.push_back(l);
        } else if (current && kind == "heights") {
            for (float& h : current->heights) line >> h;
        } else if (current && kind == "paint") {
            std::string digits;
            line >> digits;
            for (size_t k = 0; k < digits.size() && k < current->paint.size(); ++k)
                current->paint[k] = digits[k] == '.' ? kEmpty : (u8)std::max(0, digits[k] - '0');
        }
    }
    for (Patch& p : patches_)
        if (p.layers.empty()) p.layers.push_back(Layer{});
    nextId_ = highest + 1;
    ++revision_;
    LOG_INFO("terrain: %zu patches read from %S", patches_.size(), path.c_str());
    return true;
}

} // namespace terrain
