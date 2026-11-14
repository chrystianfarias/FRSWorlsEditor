#include "drive/cars/CarShadowU2.h"

#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace bb {

namespace {

using BM = BWareMath;
inline float A(float x, float y) { return BM::A(x, y); }
inline float S(float x, float y) { return BM::S(x, y); }
inline float M(float x, float y) { return BM::M(x, y); }
inline float D(float x, float y) { return BM::D(x, y); }

const float AirHeight = 3.0f;                             // flt_7A6664
const float LightHeight = X87::Bits(0x450FC000u);        // flt_7A83E8 2300
const float HullLift = X87::Bits(0x3E8CCCCDu);           // flt_7A665C 0.275
const float MaxDz = 4.0f;                                 // flt_7A664C
const float MaxPlaneDist = 1.5f;                          // flt_7A671C
const float Round = X87::Bits(0x3EAA7EFAu);              // flt_7A6720 0.333
const float HullZ = X87::Bits(0x3D75C28Fu);              // flt_7A66B8 0.06
const float InnerU = X87::Bits(0x3E1EB852u);            // dword_7A6650 0.155
const float GridLiftGame = X87::Bits(0x3DA3D70Au);      // dword_7A66AC 0.08
const float KxGame = X87::Bits(0x3F866666u), KyGame = X87::Bits(0x3F866666u); // 1.05, 1.05
const float KxFe = X87::Bits(0x3F99999Au), KyFe = X87::Bits(0x3F91EB85u);     // 1.2, 1.14
const float Grow = X87::Bits(0x3F400000u);              // flt_7A6668 0.75
const float Third = X87::Bits(0x3EAAAAABu);             // flt_786348
const float GridZ = X87::Bits(0x3D23D70Au);             // flt_7A66B4 0.04
const float FeZ = X87::Bits(0x3E1BA5E3u);               // dword_7A66BC 0.152
const uint16_t MinLightAngle = 0x1A90;                   // 37.35 deg

// Wheel index used as a fallback ground plane per grid vertex (0x622246 table).
const int WheelOfVertex[16] = { 2, 2, 1, 1, 2, 2, 1, 1, 3, 3, 0, 0, 3, 3, 0, 0 };

void Xform(float* o, const float* m, float x, float y, float z) {
    o[0] = A(A(A(M(m[8], z), M(m[4], y)), M(m[0], x)), m[12]);
    o[1] = A(A(A(M(m[9], z), M(m[1], x)), M(m[5], y)), m[13]);
    o[2] = A(A(A(M(m[10], z), M(m[2], x)), M(m[6], y)), m[14]);
}

void Add(ShadowStripU2& s, const float* p, const float* uv, uint32_t col) {
    ShadowVertexU2 v;
    v.X = p[0]; v.Y = p[1]; v.Z = p[2]; v.U = uv[0]; v.V = uv[1]; v.Colour = col;
    s.Vertices.push_back(v);
}

float Cross(const std::vector<float>& o, const std::vector<float>& a, const std::vector<float>& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

// Andrew monotone chain (qsort by x then y, lower and upper half-hulls), counter-clockwise.
std::vector<std::vector<float>> ConvexHull(const std::vector<std::vector<float>>& pts) {
    std::vector<std::vector<float>> p(pts);
    // C# List.Sort (introsort, unstable) with the same comparer; equal points are interchangeable here.
    std::sort(p.begin(), p.end(), [](const std::vector<float>& a, const std::vector<float>& b) {
        return a[0] != b[0] ? a[0] < b[0] : a[1] < b[1];
    });
    if (p.size() < 3) return p;
    std::vector<std::vector<float>> h;
    for (const auto& q : p) {
        while (h.size() >= 2 && Cross(h[h.size() - 2], h[h.size() - 1], q) <= 0) h.pop_back();
        h.push_back(q);
    }
    const size_t lower = h.size() + 1;
    for (int i = (int)p.size() - 2; i >= 0; i--) {
        const auto& q = p[(size_t)i];
        while (h.size() >= lower && Cross(h[h.size() - 2], h[h.size() - 1], q) <= 0) h.pop_back();
        h.push_back(q);
    }
    h.pop_back();
    return h;
}

} // namespace

const float CarShadowU2::BiasNear = X87::Bits(0x3DD4FDF4u);   // flt_7A66C4 0.104
const float CarShadowU2::BiasAlt = X87::Bits(0x3CBC6A7Fu);    // flt_7A66C8 0.023

float CarShadowU2::DepthBiasScale(const float* camPos, const float* carPos, bool altBias) {
    const float dx = S(camPos[0], carPos[0]), dy = S(camPos[1], carPos[1]), dz = S(camPos[2], carPos[2]);
    const float d = std::sqrt(A(A(M(dx, dx), M(dy, dy)), M(dz, dz)));
    if (!(d > 0.0f)) return 1.0f;
    return D(S(d, altBias ? BiasAlt : BiasNear), d);
}

float CarShadowU2::LightVector(const float* pos, float* l, const BWareMath* math) {
    float lx = pos[0], ly = pos[1], lz = S(pos[2], LightHeight);
    const float horiz = std::sqrt(A(M(ly, ly), M(lx, lx)));
    const uint16_t a = math != nullptr
                           ? math->ATan(horiz, -lz)
                           : static_cast<uint16_t>(static_cast<int>(std::nearbyint(std::atan2(-lz, horiz) * 32768.0 / 3.14159265358979323846)));
    if (a < MinLightAngle) {
        float sn, cs;
        BWareMath2::SinCos(MinLightAngle, sn, cs);
        const float len = std::sqrt(A(A(M(lz, lz), M(ly, ly)), M(lx, lx)));
        lz = -M(sn, len);
        const float inv = D(1.0f, A(std::fabs(ly), std::fabs(lx)));
        lx = M(M(M(lx, inv), len), cs);
        ly = M(M(M(inv, ly), len), cs);
    }
    l[0] = lx; l[1] = ly; l[2] = lz;
    return lz != 0.0f ? D(1.0f, lz) : 0.0f;
}

std::vector<std::vector<float>> CarShadowU2::RoundCorners(const std::vector<std::vector<float>>& P) {
    const int n = (int)P.size();
    std::vector<std::vector<float>> o;
    o.reserve((size_t)(3 * std::max(n, 0)));
    if (n <= 0) return o;
    float back[3];
    const auto& last = P[(size_t)(n - 1)];
    back[0] = M(S(P[0][0], last[0]), Round);
    back[1] = M(S(P[0][1], last[1]), Round);
    back[2] = M(S(P[0][2], last[2]), Round);
    for (int i = 0; i < n; i++) {
        const auto& p = P[(size_t)i];
        const auto& q = P[(size_t)((i + 1) % n)];
        const float fwd[3] = { M(Round, S(q[0], p[0])), M(Round, S(q[1], p[1])), M(Round, S(q[2], p[2])) };
        o.push_back({ S(p[0], back[0]), S(p[1], back[1]), S(p[2], back[2]) });
        const float dx = S(fwd[0], back[0]), dy = S(fwd[1], back[1]), dz = S(fwd[2], back[2]);
        o.push_back({ A(M(Round, dx), p[0]), A(M(Round, dy), p[1]), A(M(Round, dz), p[2]) });
        o.push_back({ A(p[0], fwd[0]), A(fwd[1], p[1]), A(fwd[2], p[2]) });
        back[0] = fwd[0]; back[1] = fwd[1]; back[2] = fwd[2];
    }
    return o;
}

void CarShadowU2::Build(const float* carMatrix, const ShadowElevation& ground, const BWareMath* math) {
    Strips.clear();
    HullPointCount = 0;
    if (ShadowLevel == 0) return;
    const float* E = carMatrix;
    const float pos[3] = { E[12], E[13], E[14] };
    const bool inGame = (bool)ground && !FrontEnd;
    // ground under the car (0x6217A2) and the airborne factor (0x6217AD..0x6218F6)
    float gpos[3] = { pos[0], pos[1], pos[2] };
    float gn[3] = { 0, 0, 1 };
    float air = 0.0f;
    if (inGame) {
        float gz = 0.0f;
        if (ground(pos[0], pos[1], pos[2], gz, gn)) gpos[2] = gz;
        const float h = S(pos[2], gpos[2]);
        if (h <= 0.0f) air = 0.0f;
        else if (h < AirHeight) air = D(h, AirHeight);
        else if (h > AirHeight) air = 1.0f;
        else air = 0.0f;                                    // h == 3 exactly: the compare chain falls through to 0
        if (air < 0.0f) air = 0.0f;
        if (air > 1.0f) air = 1.0f;
    }
    AirFactor = air;

    if (ShadowLevel >= 2 && inGame && CarLod <= 2 && ViewId != 3 && !HullPoints.empty())
        BuildHull(E, pos, gpos, gn, ground, math);
    BuildGrid(E, pos, gpos, gn, ground, inGame);
    // draw order: grid (RenderingOrder 2) before the hull ramp (RenderingOrder 3)
    std::stable_sort(Strips.begin(), Strips.end(),
                     [](const ShadowStripU2& a, const ShadowStripU2& b) { return a.Texture < b.Texture; });
}

bool CarShadowU2::NearPlane(const float* p, const float* gpos, const float* gn) const {
    const float d = A(A(M(S(p[0], gpos[0]), gn[0]), M(S(p[1], gpos[1]), gn[1])), M(S(p[2], gpos[2]), gn[2]));
    return std::fabs(d) < MaxPlaneDist;
}

void CarShadowU2::BuildGrid(const float* E, const float* pos, const float* gpos, const float* gn,
                            const ShadowElevation& ground, bool inGame) {
    const float lift = FrontEnd ? 0.0f : GridLiftGame;
    const float kx = FrontEnd ? KxFe : KxGame, ky = FrontEnd ? KyFe : KyGame;
    const float g = A(M(Grow, AirFactor), 1.0f);
    const float y0 = M(M(BBoxMin[1], g), ky), y1 = M(M(BBoxMax[1], g), ky);
    const float x0 = M(M(BBoxMin[0], g), kx), x1 = M(M(BBoxMax[0], g), kx);
    const float dy = M(S(y1, y0), Third), dx = M(S(x1, x0), Third);
    float V[16][3];
    float UV[16][2];
    float yy = y0, vv = 0.0f;
    for (int r = 0; r < 4; r++) {
        float xx = x0, uu = 0.0f;
        for (int c = 0; c < 4; c++) {
            float* p = V[4 * r + c];
            Xform(p, E, xx, yy, 0.0f);
            p[2] = pos[2];
            UV[4 * r + c][0] = uu;
            UV[4 * r + c][1] = vv;
            xx = A(xx, dx); uu = A(uu, Third);
        }
        yy = A(yy, dy); vv = A(vv, Third);
    }
    int bad[4] = {};
    if (inGame) {
        // wheel ground planes
        float W[4][3], Wn[4][3] = {};
        bool Wok[4];
        for (int w = 0; w < 4; w++) {
            Xform(W[w], E, WheelPos[w][0], WheelPos[w][1], WheelPos[w][2]);
            float n[3] = {};
            float wz = 0.0f;
            Wok[w] = ground(W[w][0], W[w][1], W[w][2], wz, n);
            if (Wok[w]) {
                W[w][2] = wz;
                const float k = D(-1.0f, n[2]);
                Wn[w][0] = M(n[0], k); Wn[w][1] = M(n[1], k); Wn[w][2] = M(n[2], k);
            }
        }
        float last = E[14];
        for (int k = 0; k < 16; k++) {
            float* p = V[k];
            float z = 0.0f;
            bool ok = ground(p[0], p[1], p[2], z, nullptr);
            if (!ok) {
                const int w = WheelOfVertex[k];
                if (Wok[w]) {
                    z = A(A(M(S(p[1], W[w][1]), Wn[w][1]), M(S(p[0], W[w][0]), Wn[w][0])), W[w][2]);
                    ok = true;
                }
            }
            if ((((k >> 1) ^ k) & 5) == 5) z = A(z, lift);
            p[2] = z;
            if (ok && std::fabs(S(z, gpos[2])) <= MaxDz && NearPlane(p, gpos, gn)) {
                last = z;
                p[2] = A(z, GridZ);
            } else {
                p[2] = A(last, GridZ);
                bad[k >> 2]++;
            }
        }
    } else {
        for (int k = 0; k < 16; k++) {
            V[k][2] = FeZ;
            if ((((k >> 1) ^ k) & 5) == 5) V[k][2] = A(V[k][2], lift);
        }
    }
    // alpha (0x6226CE..0x62276B): 2 (fe128 + (base - fe128) |m22| (1 - air))
    const float fe128 = FrontEnd ? 128.0f : 0.0f, b = FrontEnd ? 128.0f : 114.0f;
    const float alpha = M(2.0f, A(fe128, M(M(S(b, fe128), std::fabs(E[10])), S(1.0f, AirFactor))));
    int ai = X87::FtoL2Lo(alpha);
    if (ai < 0) ai = 0;
    if (ai > 254) ai = 254;
    GridColour = ((uint32_t)ai << 24) | 0x808080u;
    if (ai == 0) return;
    for (int r = 0; r < 3; r++) {
        if (bad[r] + bad[r + 1] >= 4) continue;           // 0x622F83
        ShadowStripU2 s;
        s.Texture = 0;
        for (int j = 0; j < 4; j++) {
            Add(s, V[4 * r + j], UV[4 * r + j], GridColour);
            Add(s, V[4 * r + 4 + j], UV[4 * r + 4 + j], GridColour);
        }
        Strips.push_back(std::move(s));
    }
}

void CarShadowU2::BuildHull(const float* E, const float* pos, const float* gpos, const float* gn,
                            const ShadowElevation& ground, const BWareMath* math) {
    const float invLz = LightVector(pos, LightDir, math);
    const float lx = LightDir[0], ly = LightDir[1];
    // project the hull points onto the plane z = pos.z along the light (0x621A3D..)
    std::vector<std::vector<float>> pts;
    pts.reserve(HullPoints.size());
    float w[3];
    for (const auto& p : HullPoints) {
        Xform(w, E, p[0], p[1], A(p[2], HullLift));
        const float t = M(-S(w[2], pos[2]), invLz);
        pts.push_back({ A(w[0], M(t, lx)), A(w[1], M(t, ly)), 0.0f });
    }
    const auto hull = ConvexHull(pts);
    // z per hull point and filter (sub_60D000)
    const float z0 = A(gpos[2], HullZ);
    const bool cheap = CarLod >= 2;
    std::vector<std::vector<float>> kept;
    bool haveCheap = false;
    float cheapZ = 0;
    bool cheapOk = false;
    for (const auto& p : hull) {
        float z = 0.0f;
        bool ok;
        if (cheap && haveCheap) { z = cheapZ; ok = cheapOk; }
        else {
            ok = ground(p[0], p[1], z0, z, nullptr);
            z = A(z, HullZ);
            if (cheap) { haveCheap = true; cheapZ = z; cheapOk = ok; }
        }
        const float q[3] = { p[0], p[1], z };
        if (ok && std::fabs(S(z, z0)) <= MaxDz && NearPlane(q, gpos, gn)) kept.push_back({ q[0], q[1], q[2] });
    }
    // corner rounding (sub_60D1B0): three points per corner
    const auto L = RoundCorners(kept);
    const int n = (int)L.size();
    HullPointCount = n;
    if (n < 3) return;
    float C[3];
    for (int j = 0; j < 3; j++) C[j] = M(0.5f, A(L[0][(size_t)j], L[(size_t)(n / 2)][(size_t)j]));
    float alpha = M(55.0f, S(1.0f, AirFactor));
    if (CarLod == 2) alpha = M(alpha, D(S(static_cast<float>(PixelSize), 20.0f), S(30.0f, 20.0f)));
    int ai = X87::FtoL2Lo(alpha);
    if (ai < 0) ai = 0;
    if (ai > 254) ai = 254;
    HullColour = ((uint32_t)ai << 24) | 0x808080u;
    const float edge = S(1.4f, M(0.3f, AirFactor));     // FancyCarShadowEdgeMult = 1.4 - 0.3 air (0x8026BC)
    const float inner[2] = { InnerU, 0.0f };
    const float zero[2] = { 0.0f, 0.0f };
    const float one[2] = { 1.0f, 0.0f };
    // A: interior strips
    for (int i = 0; i < (n & ~1) - 1; i += 2) {
        ShadowStripU2 s;
        s.Texture = 1;
        Add(s, L[(size_t)i].data(), inner, HullColour);
        Add(s, C, zero, HullColour);
        Add(s, L[(size_t)(i + 1)].data(), inner, HullColour);
        Add(s, L[(size_t)((i + 2) % n)].data(), inner, HullColour);
        Strips.push_back(std::move(s));
    }
    // B: odd closer
    if ((n & 1) != 0) {
        ShadowStripU2 s;
        s.Texture = 1;
        Add(s, L[(size_t)(n - 1)].data(), inner, HullColour);
        Add(s, C, zero, HullColour);
        Add(s, L[0].data(), inner, HullColour);
        Strips.push_back(std::move(s));
    }
    // C: outer ring (inner point at u 0.155, outer point C + (L - C) * edge at u 1)
    ShadowStripU2 ring;
    ring.Texture = 1;
    for (int j = 0; j <= n; j++) {
        const auto& p = L[(size_t)(j % n)];
        const float o[3] = { A(C[0], M(S(p[0], C[0]), edge)), A(C[1], M(S(p[1], C[1]), edge)), p[2] };
        Add(ring, p.data(), inner, HullColour);
        Add(ring, o, one, HullColour);
    }
    Strips.push_back(std::move(ring));
}

} // namespace bb
