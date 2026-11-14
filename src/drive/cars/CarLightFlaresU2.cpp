#include "drive/cars/CarLightFlaresU2.h"

#include "drive/numerics/BWareMath.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <cmath>

namespace bb {

namespace {

using BM = BWareMath;
inline float A(float x, float y) { return BM::A(x, y); }
inline float S(float x, float y) { return BM::S(x, y); }
inline float M(float x, float y) { return BM::M(x, y); }
inline float D(float x, float y) { return BM::D(x, y); }

// eRenderLightFlare constants.
const float NearSq = X87::Bits(0x46FA0000u);         // flt_78D798 32000
const float FarSq = X87::Bits(0x471C4000u);          // flt_78D79C 40000
const float AngleScale = X87::Bits(0x43700000u);     // flt_7F3D50 240 (degrees over the screen width)
const float Max255 = X87::Bits(0x437F0000u);         // flt_784544 255
const float K65536 = X87::Bits(0x47800000u);         // flt_784238
const float PixelRadius = 3.0f;                      // push 40400000h (GetPixelSize radius)
const float ViewMode3Scale = X87::Bits(0x3F333333u); // flt_7A6700 0.7

} // namespace

constexpr uint32_t CarLightFlaresU2::TextureHashes[3];

FlareTableU2 CarLightFlaresU2::DefaultTable() {
    FlareLayerU2 a;
    a.MinSize = 0.0f; a.MaxSize = 64.0f; a.R = 100.0f; a.G = 120.0f; a.B = 170.0f; a.A = 80.0f;
    a.Power = X87::Bits(0x3FB9999Au); a.Offset = X87::Bits(0x3F4CCCCDu);
    a.MinScale = X87::Bits(0x3C23D70Au); a.MaxScale = X87::Bits(0x3D449BA6u); a.TexIndex = 0;
    FlareLayerU2 b;
    b.MinSize = 28.0f; b.MaxSize = 28.0f; b.R = 100.0f; b.G = 120.0f; b.B = 140.0f; b.A = 20.0f;
    b.Power = 3.0f; b.Offset = X87::Bits(0x3F4CCCCDu);
    b.MinScale = X87::Bits(0x3C23D70Au); b.MaxScale = X87::Bits(0x3C75C28Fu); b.TexIndex = 1;
    FlareLayerU2 c;
    c.MinSize = 20.0f; c.MaxSize = 30.0f; c.R = 128.0f; c.G = 25.0f; c.B = 25.0f; c.A = 120.0f;
    c.Power = X87::Bits(0x3EE66666u); c.Offset = X87::Bits(0x3E4CCCCDu);
    c.MinScale = X87::Bits(0x3C23D70Au); c.MaxScale = X87::Bits(0x3CF5C28Fu); c.TexIndex = 2;
    return { { a, b }, { c } };
}

FlareTableU2 CarLightFlaresU2::TableFromExe(const Speed2Image& exe) {
    FlareTableU2 t((size_t)NumTypes);
    for (int type = 0; type < NumTypes; ++type) {
        for (int li = 0; li < 2; ++li) {
            const uint32_t p = exe.U32(TableVa + (uint32_t)(8 * type + 4 * li));
            if (p == 0) continue;   // 0x491256: a null layer is skipped
            FlareLayerU2 L;
            L.MinSize = exe.F32(p + 0x00);
            L.MaxSize = exe.F32(p + 0x04);
            L.R = exe.F32(p + 0x08);
            L.G = exe.F32(p + 0x0C);
            L.B = exe.F32(p + 0x10);
            L.A = exe.F32(p + 0x14);
            L.Power = exe.F32(p + 0x18);
            L.Offset = exe.F32(p + 0x1C);
            L.MinScale = exe.F32(p + 0x20);
            L.MaxScale = exe.F32(p + 0x24);
            L.TexIndex = (int)exe.U32(p + 0x28);
            t[(size_t)type].push_back(L);
        }
    }
    return t;
}

void CarLightFlaresU2::PatchForView(FlareTableU2& t, int viewId, bool inRace) {
    if (t.size() < 2 || t[0].empty() || t[1].empty()) return;
    float power = inRace ? X87::Bits(0x3FB9999Au) : 2.8f;
    float mn = 0.0f, mx = 64.0f;                             // flt_7F39D8 / flt_7F39DC
    if (viewId == 3) {
        power = M(X87::Bits(0x3F400000u), power);            // flt_7A6760 0.75
        mn = M(X87::Bits(0x402CCCCDu), mn);                  // flt_7A6764 2.7
        mx = M(mx, X87::Bits(0x402CCCCDu));
    }
    t[0][0].Power = power;
    t[0][0].MinSize = mn;
    t[0][0].MaxSize = mx;
    t[1][0].Offset = (viewId == 4 || viewId == 5) ? X87::Bits(0x3EAE147Bu) : X87::Bits(0x3E4CCCCDu); // 0.34 / 0.2
}

FlareTableU2 CarLightFlaresU2::TableForView(int viewId, bool inRace) {
    FlareTableU2 t = DefaultTable();
    PatchForView(t, viewId, inRace);
    return t;
}

float CarLightFlaresU2::Intensity(const LightFlareU2& f, const LightState& st, uint32_t headlightColour, float scale,
                                  uint32_t& colourOverride) {
    // 0x615704..0x615823
    const float headBase = st.HasCar && st.Traffic ? 0.0f : 1.0f;
    float headL = headBase, headR = headBase;
    float brakeL = X87::Bits(0x3F59999Au), brakeR = brakeL; // flt_7A6684 / flt_7A6688 0.85 (tail lights)
    float centre = 0.0f;
    const int flags = st.ForceFlags;
    if ((flags & 1) != 0 || ((flags & 8) == 0 && st.HasCar && st.HeadlightsOn)) headL = headR = A(headBase, 1.0f);
    else if ((flags & 8) != 0) headL = headR = 0.0f;
    if ((flags & 2) != 0) {
        brakeL = brakeR = A(brakeL, 1.0f);
        centre = 1.0f;
    } else if (st.HasCar && st.Braking) {
        brakeL = brakeR = A(brakeL, 16.0f);                 // flt_7A6694
        centre = M(0.5f, brakeL);                           // flt_784260
    }
    if (st.CustomSpoiler && RemoveCentreBrakeWithCustomSpoiler(st.CarType)) centre = 0.0f;
    const int sel = st.HasCar ? st.FeSelectedSlot : 0x45;

    // 0x615860..0x6158FE
    float I = 0.0f;
    bool head = false, brake = false;
    colourOverride = 0;
    switch (f.NameHash) {
        case LeftHeadlight:
        case RightHeadlight:
            if (st.HasCar && !st.HeadlightsOn) return 0.0f;
            I = f.NameHash == LeftHeadlight ? headL : headR;
            colourOverride = headlightColour;
            head = true;
            break;
        case LeftBrakelight: I = brakeL; brake = true; break;
        case CentreBrakelight: I = centre; brake = true; break;
        case RightBrakelight: I = brakeR; brake = true; break;
        default: break;
    }
    if (sel == 0xF && brake) return 0.0f;
    if (sel == 0xE && head) return 0.0f;
    if (!(I > 0.0f)) return 0.0f;
    if (I > 1.0f) I = 1.0f;
    if (scale != 0.0f) I = M(I, scale);
    return I;
}

int CarLightFlaresU2::GetPixelSize(const FlareViewU2& v, const float* pos, float radius) {
    const float dx = S(pos[0], v.CamPos[0]), dy = S(pos[1], v.CamPos[1]), dz = S(pos[2], v.CamPos[2]);
    const float dot = A(A(M(dz, v.CamDir[2]), M(dy, v.CamDir[1])), M(dx, v.CamDir[0]));
    if (!(-radius <= dot)) return 0;
    float d = std::sqrt(A(A(M(dz, dz), M(dy, dy)), M(dx, dx)));
    d = S(d, radius);
    if (d > M(radius, 0.5f)) return X87::FtoL2Lo(M(D(v.Focal, d), radius));
    return X87::FtoL2Lo(A(v.Focal, v.Focal));
}

bool CarLightFlaresU2::CarBigEnough(const FlareViewU2& v, const float* carPos, bool viewMode3) {
    int px = GetPixelSize(v, carPos, PixelRadius);
    if (viewMode3) px = X87::FtoL2Lo(M(static_cast<float>(px), ViewMode3Scale));
    return px >= 4;
}

bool CarLightFlaresU2::PointInFrustum(const FlareViewU2& v, const float* p) {
    for (int i = 0; i < 6; i++) {
        const float* q = v.Planes;
        const int o = i * 4;
        const float d = A(A(A(M(q[o + 2], p[2]), M(q[o], p[0])), M(q[o + 1], p[1])), q[o + 3]);
        if (!(d > 0.0f)) return false;
    }
    return true;
}

uint16_t CarLightFlaresU2::DegToAngle(float deg) {
    return static_cast<uint16_t>(static_cast<uint32_t>(X87::FtoL2Lo(M(deg, K65536)) / 360));
}

void CarLightFlaresU2::AxisRotation(float* o, const float* axis, uint16_t angle) {
    const float c = BM::Cos(angle), s = BM::Sin(angle);
    const float t = S(1.0f, c);
    const float x = axis[0], y = axis[1], z = axis[2];
    const float xt = M(x, t), yt = M(y, t);
    o[3] = o[7] = o[11] = 0.0f; o[12] = o[13] = o[14] = 0.0f; o[15] = 1.0f;
    o[0] = A(M(x, xt), c);
    const float xyt = M(xt, y);                         // (x t) y, not x (y t)
    const float zs = M(z, s);
    o[1] = S(xyt, zs);
    const float xzt = M(xt, z);
    const float ys = M(y, s);
    o[2] = A(ys, xzt);
    o[4] = A(xyt, zs);
    o[5] = A(M(yt, y), c);
    const float yzt = M(yt, z);
    const float xs = M(x, s);
    o[6] = S(yzt, xs);
    o[8] = S(xzt, ys);
    o[9] = A(yzt, xs);
    o[10] = A(M(M(z, z), t), c);
}

float CarLightFlaresU2::ScreenX(const FlareViewU2& v, const float* p) {
    const float* m = v.ViewProj;
    const float x = A(A(A(M(p[2], m[8]), M(p[1], m[4])), M(p[0], m[0])), m[12]);
    const float w = A(A(A(M(p[2], m[11]), M(p[0], m[3])), M(p[1], m[7])), m[15]);
    const float inv = D(1.0f, w);
    return M(M(A(M(inv, x), 1.0f), static_cast<float>(v.Width)), 0.5f);
}

int CarLightFlaresU2::Render(const FlareViewU2& v, const LightFlareU2& f, const float* localWorld, float intensity,
                             uint32_t colourOverride, const FlareTableU2& table, FlarePolyU2* outPolys,
                             int outCapacity, bool screenRotation) {
    int n = 0;
    if (f.Type < 0 || f.Type >= (int)table.size()) return 0;
    // 0x490E49: world position and frustum test
    float P[3];
    const float local[3] = { f.X, f.Y, f.Z };
    BM::MulVector3(P, localWorld, local);
    if (!PointInFrustum(v, P)) return 0;
    // 0x490EDB: to-camera vector, squared distance fade 32000..40000
    const float Dx = S(v.CamPos[0], P[0]), Dy = S(v.CamPos[1], P[1]), Dz = S(v.CamPos[2], P[2]);
    const float d2 = A(A(M(Dz, Dz), M(Dy, Dy)), M(Dx, Dx));
    if (!(d2 <= FarSq)) return 0;
    float fade = 1.0f;
    if (!(d2 <= NearSq)) fade = S(1.0f, D(S(d2, NearSq), S(FarSq, NearSq)));
    // sub_4870B0: normalize
    const float len = std::sqrt(A(A(M(Dx, Dx), M(Dy, Dy)), M(Dz, Dz)));
    const float il = D(1.0f, len);
    const float Dn[3] = { M(il, Dx), M(il, Dy), M(il, Dz) };
    // direction to world (w = 0), cosine to the camera
    float Wd[4];
    const float dir4[4] = { f.DirX, f.DirY, f.DirZ, 0.0f };
    CameraMathLite::MulVec4(Wd, localWorld, dir4);
    const float c = A(A(M(Wd[2], Dn[2]), M(Wd[1], Dn[1])), M(Wd[0], Dn[0]));
    float facing;
    if ((f.Flags & 1) != 0) facing = std::fabs(c);
    else facing = c < 0.0f ? 0.0f : c;
    // camera axes (columns of the eCamera matrix)
    const float* cm = v.CamMatrix;
    float R0[3] = { cm[0], cm[4], cm[8] };
    float R1[3] = { cm[1], cm[5], cm[9] };
    float R2[3] = { cm[2], cm[6], cm[10] };
    const float viewZ = A(A(A(M(cm[10], P[2]), M(cm[2], P[0])), M(cm[6], P[1])), cm[14]);
    float k = D(viewZ, v.Focal);
    const std::vector<FlareLayerU2>& layers = table[(size_t)f.Type];
    float rot[16];
    for (int li = 0; li < 2 && li < (int)layers.size(); li++) {
        const FlareLayerU2& L = layers[(size_t)li];
        float I = intensity;
        if ((f.Flags & 4) != 0) {
            // __CIpow (SSE2 pow_pentium4), fmul, fst dword
            I = static_cast<float>(std::pow(static_cast<double>(facing), static_cast<double>(L.Power)) *
                                   static_cast<double>(intensity));
            if (I > 1.0f) I = 1.0f;
        }
        float size = A(M(S(L.MaxSize, L.MinSize), I), L.MinSize);
        if (!(size > 0.0f)) continue;
        // bScaleAdd: pull toward the camera
        const float Pbx = A(M(L.Offset, Dn[0]), P[0]);
        const float Pby = A(M(L.Offset, Dn[1]), P[1]);
        const float Pbz = A(M(L.Offset, Dn[2]), P[2]);
        if (k < L.MinScale) k = L.MinScale;                 // kept for the next layer (var_5F4)
        if (k > L.MaxScale) k = L.MaxScale;
        size = M(size, k);
        const float r = M(I, L.R), g = M(I, L.G), b = M(I, L.B);
        float a = M(M(I, L.A), fade);
        a = A(a, a);
        if (a > Max255) a = Max255;
        // flareMode 0: rotate the camera axes about the view direction by 240 deg x screen x / width
        if (screenRotation) {
            BM::Normalize(R2, R2);                          // in place: renormalized again for the next layer
            const float* fwd = R2;
            const float sx = ScreenX(v, P);
            const float deg = M(D(sx, static_cast<float>(v.Width)), AngleScale);
            AxisRotation(rot, fwd, DegToAngle(deg));
            Rotate(R0, rot);
            Rotate(R1, rot);
        }
        // sub_42FAF0: the axes are scaled in place (and reused, rotated and scaled again, by the next layer)
        for (int i = 0; i < 3; i++) { R0[i] = M(size, R0[i]); R1[i] = M(size, R1[i]); }
        uint32_t col;
        if (colourOverride != 0) col = (static_cast<uint32_t>(X87::FtoL2Lo(a)) << 24) | colourOverride;
        else {
            col = static_cast<uint32_t>(X87::FtoL2Lo(a));
            col = (col << 8) | static_cast<uint32_t>(X87::FtoL2Lo(b));
            col = (col << 8) | static_cast<uint32_t>(X87::FtoL2Lo(g));
            col = (col << 8) | static_cast<uint32_t>(X87::FtoL2Lo(r));
        }
        FlarePolyU2 q;
        q.Colour = col;
        q.TexIndex = L.TexIndex;
        q.Layer = li;
        q.X0 = A(S(-R0[0], R1[0]), Pbx); q.Y0 = A(S(-R0[1], R1[1]), Pby); q.Z0 = A(S(-R0[2], R1[2]), Pbz);
        q.X1 = A(S(R0[0], R1[0]), Pbx); q.Y1 = A(S(R0[1], R1[1]), Pby); q.Z1 = A(S(R0[2], R1[2]), Pbz);
        q.X2 = A(A(R1[0], R0[0]), Pbx); q.Y2 = A(A(R1[1], R0[1]), Pby); q.Z2 = A(A(R1[2], R0[2]), Pbz);
        q.X3 = A(A(-R0[0], R1[0]), Pbx); q.Y3 = A(A(-R0[1], R1[1]), Pby); q.Z3 = A(A(-R0[2], R1[2]), Pbz);
        if (outPolys != nullptr && n < outCapacity) outPolys[n] = q;
        n++;
    }
    return n;
}

void CarLightFlaresU2::Rotate(float* v, const float* m) {
    const float x = A(A(A(M(m[8], v[2]), M(m[4], v[1])), M(m[0], v[0])), m[12]);
    const float y = A(A(A(M(m[9], v[2]), M(m[5], v[1])), M(m[1], v[0])), m[13]);
    const float z = A(A(A(M(m[10], v[2]), M(m[6], v[1])), M(m[2], v[0])), m[14]);
    v[0] = x; v[1] = y; v[2] = z;
}

namespace CameraMathLite {

void MulVec4(float* o, const float* m, const float* v) {
    const float x = A(A(A(M(m[12], v[3]), M(m[8], v[2])), M(m[4], v[1])), M(m[0], v[0]));
    const float y = A(A(A(M(m[13], v[3]), M(m[1], v[0])), M(m[9], v[2])), M(m[5], v[1]));
    const float z = A(A(A(M(m[14], v[3]), M(m[2], v[0])), M(m[10], v[2])), M(m[6], v[1]));
    const float w = A(A(A(M(m[15], v[3]), M(m[3], v[0])), M(m[11], v[2])), M(m[7], v[1]));
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}

} // namespace CameraMathLite

} // namespace bb
