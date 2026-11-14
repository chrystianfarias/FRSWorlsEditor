#include "drive/cars/CarHeadlightsU2.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"

#include <cmath>
#include <cstring>

namespace bb {

namespace {

using BM = BWareMath;
inline float A(float x, float y) { return BM::A(x, y); }
inline float S(float x, float y) { return BM::S(x, y); }
inline float M(float x, float y) { return BM::M(x, y); }
inline float D(float x, float y) { return BM::D(x, y); }

// sub_5C2AE0 constants
const float EyeForward = 0.0f;                          // flt_7A27B0
const float EyeUp = X87::Bits(0x3F400000u);            // flt_7A27B4 0.75
const float DirDrop = X87::Bits(0x3DCCCCCDu);          // flt_7A27C0 0.1
const float Aspect9 = X87::Bits(0x3F19A956u);          // 0.6002401 (views 9 and 10)

} // namespace

const float CarHeadlightsU2::Near = 1.0f;                // flt_7A27AC
const float CarHeadlightsU2::Far = 100.0f;               // dword_7FEE58
const uint16_t CarHeadlightsU2::Fov =
    static_cast<uint16_t>(X87::FtoL2Lo(M(X87::Bits(0x40200000u), X87::Bits(0x4622F983u)))); // 2.5 rad -> 26075
const float CarHeadlightsU2::ColourScale = X87::Bits(0x3C000000u);

void CarHeadlightsU2::ProjectorView(const float* carMatrix, float* view, float* proj) {
    const float* m = carMatrix;
    float eye[3];
    for (int i = 0; i < 3; i++) eye[i] = A(A(m[12 + i], M(m[i], EyeForward)), M(m[8 + i], EyeUp));
    float dir[3] = { m[0], m[1], S(m[2], DirDrop) };
    BM::Normalize(dir, dir);
    const float look[3] = { A(eye[0], dir[0]), A(eye[1], dir[1]), A(eye[2], dir[2]) };
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    CameraMath::LookAt(view, eye, look, up);
    // sub_5C7EC0, views 9/10: aspect forced, no horizontal widening
    const uint16_t hx = static_cast<uint16_t>(X87::FtoL2Lo(M(M(static_cast<float>(Fov), 1.0f), 0.5f)));
    const uint16_t hy = static_cast<uint16_t>(X87::FtoL2Lo(M(M(static_cast<float>(Fov), Aspect9), 0.5f)));
    const float xs = D(1.0f, D(BM::Sin(hx), BM::Cos(hx)));
    const float ys = D(1.0f, D(BM::Sin(hy), BM::Cos(hy)));
    const float q = D(Far, S(Far, Near));
    std::memset(proj, 0, 16 * sizeof(float));
    proj[0] = xs; proj[5] = -ys; proj[10] = q; proj[11] = 1.0f; proj[14] = -M(q, Near); proj[15] = 0.0f;
}

std::array<float, 16> CarHeadlightsU2::ProjectorMatrix(const float* carMatrix) {
    float v[16], p[16], vp[16];
    ProjectorView(carMatrix, v, p);
    Mul(v, p, vp);
    const float bias[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1 };
    std::array<float, 16> o{};
    Mul(vp, bias, o.data());
    return o;
}

void CarHeadlightsU2::Mul(const float* a, const float* b, float* o) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s = A(s, M(a[i * 4 + k], b[k * 4 + j]));
            o[i * 4 + j] = s;
        }
}

std::array<uint32_t, 64> CarHeadlightsU2::ClipRamp() {
    std::array<uint32_t, 64> t{};
    const float step = X87::Bits(0x3D888889u);                // flt_7844EC 0.06666667
    for (int i = 33; i < 48; i++) {
        const float v = std::sqrt(S(1.0f, M(static_cast<float>(i - 33), step)));
        const uint32_t g = static_cast<uint32_t>(X87::FtoL2Lo(M(v, 255.0f)));
        t[(size_t)i] = g * 0x01010101u;
    }
    return t;
}

bool CarHeadlightsU2::SplashVisible(const float* carMatrix) { return carMatrix[10] >= X87::Bits(0x3F34FDF4u); }

std::array<float, 12> CarHeadlightsU2::SplashQuad() {
    const float c = X87::Bits(0x41500000u), h = 10.0f, w = X87::Bits(0x409CCCCDu);
    return { S(c, h), -w, 0.0f, A(c, h), -w, 0.0f, A(c, h), w, 0.0f, S(c, h), w, 0.0f };
}

} // namespace bb
