#pragma once

// Port of NFSU2Unity Core/Physics/Wheel/Graph.cs.

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace bb {

// struct Graph { float (*pts)[2]; int count; } of SPEED2.EXE: piecewise-linear curve over (x, y) pairs sorted
// by x (appendix A section 3.5 of docs/re/physics-overview.md).
class Graph {
public:
    std::vector<float> Points;   // +0x0 -> x0, y0, x1, y1, ...
    int Count = 0;               // +0x4

    Graph() = default;
    Graph(std::vector<float> points, int count) : Points(std::move(points)), Count(count) {}

    // Graph object at va ({pts*, count}) read from the exe.
    static Graph FromObject(const Speed2Image& exe, uint32_t va);

    // Points array at va (graphs built by static initialisers, e.g. unk_81E5B0 = {0x7E9F58, 4}).
    static Graph FromPoints(const Speed2Image& exe, uint32_t pts, int n);

    // sub_40C840 (Graph::GetValue(float* out, float x)): writes result unless the original leaves *out
    // untouched (count <= 0 or no segment for NaN). See BWareMath2::GraphGetValueTo.
    bool GetValueTo(float& result, float x) const;

    // Graph::GetValue(float x) 0x576790 (thiscall, st0).
    F24 GetValue(float x) const;

    static void Verify(const Speed2Image& exe);
};

} // namespace bb
