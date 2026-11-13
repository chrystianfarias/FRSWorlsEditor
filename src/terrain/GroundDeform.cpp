#include "terrain/GroundDeform.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

namespace terrain {

namespace {

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

} // namespace

float* GroundDeform::Corner(int ix, int iy, bool create, float** base) {
    const int tx = FloorDiv(ix, kTile), ty = FloorDiv(iy, kTile);
    auto found = tiles_.find(Key(tx, ty));
    if (found == tiles_.end()) {
        if (!create) return nullptr;
        Tile tile;
        tile.offset.assign((size_t)kTile * kTile, 0.0f);
        tile.base.assign((size_t)kTile * kTile, std::numeric_limits<float>::quiet_NaN());
        found = tiles_.emplace(Key(tx, ty), std::move(tile)).first;
    }
    const size_t k = (size_t)(iy - ty * kTile) * kTile + (size_t)(ix - tx * kTile);
    if (base) *base = &found->second.base[k];
    return &found->second.offset[k];
}

float GroundDeform::Offset(int ix, int iy) const {
    const int tx = FloorDiv(ix, kTile), ty = FloorDiv(iy, kTile);
    const auto found = tiles_.find(Key(tx, ty));
    if (found == tiles_.end()) return 0.0f;
    return found->second.offset[(size_t)(iy - ty * kTile) * kTile + (size_t)(ix - tx * kTile)];
}

float GroundDeform::At(float x, float y) const {
    if (tiles_.empty()) return 0.0f;
    const float fx = x / kCell, fy = y / kCell;
    const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    const float u = fx - ix, v = fy - iy;
    const float a = Offset(ix, iy), b = Offset(ix + 1, iy), c = Offset(ix, iy + 1), d = Offset(ix + 1, iy + 1);
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}

Aabb GroundDeform::Apply(Brush brush, const Vec3& centre, float radius, float amount, float level,
                         const std::function<float(float, float)>& base) {
    Aabb changed;
    if (brush == Brush::None || brush == Brush::Paint || brush == Brush::AddArea || brush == Brush::EraseArea ||
        radius <= 0.0f)
        return changed;
    const int i0 = (int)std::floor((centre.x - radius) / kCell), i1 = (int)std::ceil((centre.x + radius) / kCell);
    const int j0 = (int)std::floor((centre.y - radius) / kCell), j1 = (int)std::ceil((centre.y + radius) / kCell);

    const auto baseAt = [&](int i, int j) {
        float* b = nullptr;
        Corner(i, j, true, &b);
        if (std::isnan(*b)) *b = base ? base(i * kCell, j * kCell) : 0.0f;
        return *b;
    };

    // Smoothing needs the neighbours as they were before this step.
    std::vector<float> finalBefore;
    const int w = i1 - i0 + 3;
    if (brush == Brush::Smooth) {
        finalBefore.resize((size_t)w * (j1 - j0 + 3));
        for (int j = j0 - 1; j <= j1 + 1; ++j)
            for (int i = i0 - 1; i <= i1 + 1; ++i)
                finalBefore[(size_t)(j - j0 + 1) * w + (i - i0 + 1)] = baseAt(i, j) + Offset(i, j);
    }
    const auto before = [&](int i, int j) { return finalBefore[(size_t)(j - j0 + 1) * w + (i - i0 + 1)]; };

    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const float x = i * kCell, y = j * kCell;
            const float d2 = ((x - centre.x) * (x - centre.x) + (y - centre.y) * (y - centre.y)) / (radius * radius);
            if (d2 >= 1.0f) continue;
            const float weight = (1.0f - d2) * (1.0f - d2);
            float* offset = Corner(i, j, true);
            switch (brush) {
                case Brush::Raise: *offset += amount * weight; break;
                case Brush::Lower: *offset -= amount * weight; break;
                case Brush::Flatten: {
                    const float now = baseAt(i, j) + *offset;
                    *offset += (level - now) * std::min(1.0f, amount * weight * 0.5f);
                    break;
                }
                case Brush::Smooth: {
                    const float average = (before(i - 1, j) + before(i + 1, j) + before(i, j - 1) + before(i, j + 1) +
                                           before(i, j) * 4.0f) / 8.0f;
                    *offset += (average - before(i, j)) * std::min(1.0f, amount * weight * 0.5f);
                    break;
                }
                default: break;
            }
            changed.Add({ x, y, centre.z });
        }
    if (changed.Valid()) {
        changed = Pad(changed, kCell);
        ++revision_;
    }
    return changed;
}

Aabb GroundDeform::Bounds() const {
    Aabb box;
    for (const auto& [key, tile] : tiles_) {
        const int tx = (int)(key >> 32), ty = (int)(u32)(key & 0xFFFFFFFF);
        box.Add({ tx * kTile * kCell - kCell, ty * kTile * kCell - kCell, -1e4f });
        box.Add({ (tx + 1) * kTile * kCell + kCell, (ty + 1) * kTile * kCell + kCell, 1e4f });
    }
    return box;
}

bool GroundDeform::Save(const std::wstring& path) const {
    if (tiles_.empty()) {
        ::_wremove(path.c_str());
        return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "FRSGROUND 1\n# tile <tx> <ty> then 32 x 32 height offsets (cell 2 units), not yet baked\n";
    char text[64];
    for (const auto& [key, tile] : tiles_) {
        const int tx = (int)(key >> 32), ty = (int)(u32)(key & 0xFFFFFFFF);
        out << "tile " << tx << " " << ty;
        for (float v : tile.offset) {
            std::snprintf(text, sizeof(text), " %.3f", v);
            out << text;
        }
        out << "\n";
    }
    return (bool)out;
}

bool GroundDeform::Load(const std::wstring& path) {
    tiles_.clear();
    ++revision_;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("tile ", 0) != 0) continue;
        std::istringstream fields(line.substr(5));
        int tx = 0, ty = 0;
        fields >> tx >> ty;
        Tile tile;
        tile.offset.assign((size_t)kTile * kTile, 0.0f);
        tile.base.assign((size_t)kTile * kTile, std::numeric_limits<float>::quiet_NaN());
        for (float& v : tile.offset) fields >> v;
        tiles_[Key(tx, ty)] = std::move(tile);
    }
    LOG_INFO("ground: %zu tiles of unbaked height offsets read", tiles_.size());
    return true;
}

} // namespace terrain
