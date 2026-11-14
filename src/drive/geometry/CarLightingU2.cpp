#include "drive/geometry/CarLightingU2.h"

#include "drive/cars/CarBin.h"
#include "drive/numerics/BWareMath.h"

#include <cmath>

namespace bb {

namespace {

ShaperLightRigU2 Make(uint32_t hash, const float* v) {
    ShaperLightRigU2 r;
    r.NameHash = hash;
    for (int i = 0; i < 3; i++) {
        ShaperLightRigU2::Light& l = r.Lights[i];
        l.CameraRelative = true;
        l.Azimuth = v[i * 3];
        l.Elevation = v[i * 3 + 1];
        l.R = l.G = l.B = 1.0f;
        l.Intensity = v[i * 3 + 2];
    }
    return r;
}

::Vec3 Normalize3(const ::Vec3& v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0 ? ::Vec3{ v.x / l, v.y / l, v.z / l } : v;
}

// sub_48A310's angle conversion: ftol(a * 65536.0f (flt_784238)) / 360 (magic 0xB60B60B7, signed,
// truncating), passed on as an unsigned short.
uint16_t ToBAngle(float degrees) {
    const int scaled = static_cast<int>(static_cast<double>(degrees) * 65536.0); // _ftol2 truncates
    return static_cast<uint16_t>(static_cast<uint32_t>(scaled / 360));
}

} // namespace

const ShaperLightRigU2& ShaperLightRigU2::CarsInGame() {
    static const float v[9] = { 200, 90, 0.5f, 42, 33, 0.35f, 219, 10, 0.1f };
    static const ShaperLightRigU2 rig = Make(0, v);
    return rig;
}

const ShaperLightRigU2& ShaperLightRigU2::Default() {
    static const float v[9] = { 149, 100, 0.2f, 303, 36, 0.2f, 76, 22, 0.2f };
    static const ShaperLightRigU2 rig = Make(0, v);
    return rig;
}

std::vector<ShaperLightRigU2> ShaperLightRigU2::ReadAll(const std::vector<uint8_t>& buf) {
    std::vector<ShaperLightRigU2> list;
    carbin::Walk(carbin::Parse(buf), [&](const carbin::Chunk& c) {
        if (c.Id != ChunkId) return;
        for (int k = 0; k < c.Size / 0x74; k++) {
            try {
                carbin::Reader r(buf.data(), c.DataOffset() + k * 0x74, c.DataOffset() + (k + 1) * 0x74);
                ShaperLightRigU2 rig;
                rig.NameHash = r.U32();
                for (int i = 0; i < 3; i++) {
                    r.Seek(0x20 + 0x1C * i);
                    Light& l = rig.Lights[i];
                    l.CameraRelative = r.U32() != 0;
                    l.Azimuth = r.F32();
                    l.Elevation = r.F32();
                    l.R = r.F32();
                    l.G = r.F32();
                    l.B = r.F32();
                    l.Intensity = r.F32();
                }
                list.push_back(rig);
            } catch (const std::exception&) {
            }
        }
    });
    return list;
}

::Vec3 CarLightingU2::AnglesToDir(float azDeg, float elDeg) {
    // elSetupLights passes (Azimuth + 180, 90 - Elevation, 1) (0x48EDE0..).
    const float a = azDeg + 180.0f;
    const float b = 90.0f - elDeg;
    float sinA, cosA, sinB, cosB;
    BWareMath2::SinCos(ToBAngle(a), sinA, cosA);   // bSinCos(&sin, &cos, angle) 0x43C390
    BWareMath2::SinCos(ToBAngle(b), sinB, cosB);
    // out = (sinB * cosA, sinB * sinA, cosB) * scale(1): one float rounding each (fstp).
    return { sinB * cosA, sinB * sinA, cosB };
}

void CarLightingU2::Project(float c[NumCoeffs][4], const ::Vec3& l, const float s[4]) {
    const float x = l.x, y = l.y, z = l.z;
    const float l20 = 0.315392f * (3 * z * z - 1);
    const float l22 = 0.546274f * (x * x - y * y);
    const float k[NumCoeffs] = {
        0.886227f * 0.282095f - 0.247708f * l20,
        0.429043f * l22,
        -0.429043f * l22,
        0.743125f * l20,
        0.858086f * 1.092548f * x * y,
        0.858086f * 1.092548f * x * z,
        0.429043f * 1.092548f * y * z, // game quirk: c1 instead of 2*c1
        1.023328f * 0.488603f * x,
        1.023328f * 0.488603f * y,
        1.023328f * 0.488603f * z,
    };
    for (int i = 0; i < NumCoeffs; i++)
        for (int ch = 0; ch < 4; ch++) c[i][ch] += s[ch] * k[i];
}

void CarLightingU2::AddLight(float c[NumCoeffs][4], const ::Vec3& dir, const float col[4]) {
    Project(c, dir, col);
    const float bounce[4] = { col[0] * -0.05f, col[1] * -0.05f, col[2] * -0.05f, col[3] * -0.05f };
    Project(c, ::Vec3{ -dir.x, -dir.y, -dir.z }, bounce);
}

void CarLightingU2::Compute(const ShaperLightRigU2& rig, const ::Vec3& camRight, const ::Vec3& camDown,
                            const ::Vec3& camForward, float c[NumCoeffs][4]) {
    for (int i = 0; i < NumCoeffs; i++)
        for (int ch = 0; ch < 4; ch++) c[i][ch] = 0.0f;
    for (const ShaperLightRigU2::Light& L : rig.Lights) {
        ::Vec3 d = AnglesToDir(L.Azimuth, L.Elevation);
        if (L.CameraRelative)
            d = ::Vec3{ -d.y * camRight.x - d.z * camDown.x + d.x * camForward.x,
                        -d.y * camRight.y - d.z * camDown.y + d.x * camForward.y,
                        -d.y * camRight.z - d.z * camDown.z + d.x * camForward.z };
        d = Normalize3(d);
        const float r = L.R * L.Intensity * 255.0f * (1.0f / 255.0f);
        const float g = L.G * L.Intensity * 255.0f * (1.0f / 255.0f);
        const float b = L.B * L.Intensity * 255.0f * (1.0f / 255.0f);
        const float col[4] = { r, g, b, 0.29f * r + 0.54f * g + 0.17f * b };
        AddLight(c, d, col);
    }
}

void CarLightingU2::RotateToLocal(const float world[NumCoeffs][4], const Mat4& l2w, float local[NumCoeffs][4]) {
    // SH(n) = c0 + n^T Q n + b.n with Q = [[c1, c4/2, c5/2], [c4/2, c2, c6/2], [c5/2, c6/2, c3]],
    // b = (c7, c8, c9). With n_world = W^T n (W = rows of l2w): Q' = W Q W^T, b' = W b.
    const float (*W)[4] = l2w.m;
    for (int ch = 0; ch < 4; ch++) {
        const float Q[3][3] = {
            { world[1][ch], 0.5f * world[4][ch], 0.5f * world[5][ch] },
            { 0.5f * world[4][ch], world[2][ch], 0.5f * world[6][ch] },
            { 0.5f * world[5][ch], 0.5f * world[6][ch], world[3][ch] },
        };
        float WQ[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) WQ[i][j] = W[i][0] * Q[0][j] + W[i][1] * Q[1][j] + W[i][2] * Q[2][j];
        float R[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) R[i][j] = WQ[i][0] * W[j][0] + WQ[i][1] * W[j][1] + WQ[i][2] * W[j][2];
        local[0][ch] = world[0][ch];
        local[1][ch] = R[0][0];
        local[2][ch] = R[1][1];
        local[3][ch] = R[2][2];
        local[4][ch] = R[0][1] + R[1][0];
        local[5][ch] = R[0][2] + R[2][0];
        local[6][ch] = R[1][2] + R[2][1];
        for (int i = 0; i < 3; i++)
            local[7 + i][ch] = W[i][0] * world[7][ch] + W[i][1] * world[8][ch] + W[i][2] * world[9][ch];
    }
}

void CarLightingU2::LocalEyePos(const Mat4& l2w, const ::Vec3& eyeWorld, bool frontEnd, float out[4]) {
    // e = R^T * (eye - l2w.t): the rows of l2w are the part's axes in world space.
    const float v[3] = { eyeWorld.x - l2w.m[3][0], eyeWorld.y - l2w.m[3][1], eyeWorld.z - l2w.m[3][2] };
    float e[3];
    for (int i = 0; i < 3; i++) e[i] = l2w.m[i][0] * v[0] + l2w.m[i][1] * v[1] + l2w.m[i][2] * v[2];
    float d = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    const float dMax = frontEnd ? EyeMaxFrontEnd : EyeMinDistance;
    if (d > 0.0f) {
        const float clamped = d < EyeMinDistance ? EyeMinDistance : (d > dMax ? dMax : d);
        const float k = clamped / d;   // normalize(e) * clamped
        out[0] = e[0] * k;
        out[1] = e[1] * k;
        out[2] = e[2] * k;
    } else {
        // Camera exactly at the part's origin: no direction to keep. Straight up, at the minimum.
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = EyeMinDistance;
    }
    out[3] = 1.0f;
}

void CarLightingU2::Evaluate(const float c[NumCoeffs][4], const ::Vec3& n, float out[4]) {
    const float b[NumCoeffs] = { 1.0f, n.x * n.x, n.y * n.y, n.z * n.z, n.x * n.y, n.x * n.z, n.y * n.z, n.x, n.y, n.z };
    for (int ch = 0; ch < 4; ch++) {
        float s = 0.0f;
        for (int i = 0; i < NumCoeffs; i++) s += c[i][ch] * b[i];
        out[ch] = s;
    }
}

} // namespace bb
