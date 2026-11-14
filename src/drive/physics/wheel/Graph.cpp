#include "drive/physics/wheel/Graph.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <stdexcept>

namespace bb {

namespace {
const float K_Eps = X87::Bits(0x358637BDu);   // flt_787594 1e-6
}

Graph Graph::FromObject(const Speed2Image& exe, uint32_t va) {
    return FromPoints(exe, exe.U32(va), static_cast<int>(exe.U32(va + 4)));
}

Graph Graph::FromPoints(const Speed2Image& exe, uint32_t pts, int n) {
    std::vector<float> p(static_cast<size_t>(2 * n));
    for (int i = 0; i < 2 * n; i++) p[i] = exe.F32(pts + 4 * static_cast<uint32_t>(i));
    return Graph(std::move(p), n);
}

bool Graph::GetValueTo(float& result, float x) const {
    float v;
    if (!BWareMath2::GraphGetValueTo(Points.data(), Count, x, v)) return false;
    result = v;
    return true;
}

F24 Graph::GetValue(float x) const {
    const float* p = Points.data();
    int n = Count;
    if (n <= 1) return p[1];
    // fcomp x0; test ah,41h; jp: continue only when x > x0 or unordered
    if (X87::ParityEven(WheelK::Fcom(x, p[0]) & 0x41) == false) return p[1];
    // fcomp x[n-1]; test ah,1; jnz: search when x < x[n-1] or unordered
    if ((WheelK::Fcom(x, p[2 * n - 2]) & X87::C0) == 0) return p[2 * n - 1];
    for (int i = 0; i < n - 1; i++) {
        if ((WheelK::Fcom(x, p[2 * i]) & X87::C0) != 0) continue;          // x < x_i or unordered
        if (X87::ParityEven(WheelK::Fcom(x, p[2 * i + 2]) & 0x05)) continue; // not (x < x_{i+1})
        F24 dy = F24(p[2 * i + 3]) - p[2 * i + 1];
        F24 dx = F24(p[2 * i + 2]) - p[2 * i];
        if (F24::Abs(dx).V > K_Eps)                                         // test ah,41h; jnz
            return (F24(x) - p[2 * i]) / dx * dy + p[2 * i + 1];
        return dy * WheelK::Half + p[2 * i + 1];
    }
    return p[1];
}

void Graph::Verify(const Speed2Image& exe) {
    if (exe.U32(0x787594) != X87::Bits(K_Eps))
        throw std::runtime_error("Graph constant at 0x00787594 differs from the exe");
}

} // namespace bb
