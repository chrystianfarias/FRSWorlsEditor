// Port of NFSU2Unity Core/Camera/CameraEffects.cs.

#include "drive/camera/CameraEffects.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"

#include <cmath>
#include <cstring>

namespace bb {

namespace CameraNoise {

namespace {

F24 R24(double v) { return F24::MulDouble(CameraMath::One, v); }

int Hash(int n) {
    uint32_t un = static_cast<uint32_t>(n);
    uint32_t h = (un << 13) ^ un;
    uint32_t e = h * h * 0x3D73u + 0xC0AE5u;
    e = e * h - 0x2DF722F3u;
    return static_cast<int>(e & 0x7FFFFFFFu);
}

// Inline o = m * r of sub_441340 (summation order copied from the asm).
void MulMR(float* dst, const float* m, const float* r) {
    float o[16];
    o[0] = (((F24(r[0]) * m[0]) + (F24(r[12]) * m[3])) + (F24(r[8]) * m[2]) + (F24(r[4]) * m[1])).Store();
    o[1] = (((F24(r[13]) * m[3]) + (F24(r[9]) * m[2])) + (F24(r[5]) * m[1]) + (F24(r[1]) * m[0])).Store();
    o[2] = (((F24(r[10]) * m[2]) + (F24(r[6]) * m[1])) + (F24(r[14]) * m[3]) + (F24(r[2]) * m[0])).Store();
    o[3] = (((F24(r[11]) * m[2]) + (F24(r[7]) * m[1])) + (F24(r[15]) * m[3]) + (F24(r[3]) * m[0])).Store();
    o[4] = (((F24(r[12]) * m[7]) + (F24(r[0]) * m[4])) + (F24(r[4]) * m[5]) + (F24(r[8]) * m[6])).Store();
    o[5] = (((F24(r[1]) * m[4]) + (F24(r[13]) * m[7])) + (F24(r[5]) * m[5]) + (F24(r[9]) * m[6])).Store();
    o[6] = (((F24(r[2]) * m[4]) + (F24(r[14]) * m[7])) + (F24(r[6]) * m[5]) + (F24(r[10]) * m[6])).Store();
    o[7] = (((F24(r[3]) * m[4]) + (F24(r[15]) * m[7])) + (F24(r[7]) * m[5]) + (F24(r[11]) * m[6])).Store();
    o[8] = (((F24(r[12]) * m[11]) + (F24(r[0]) * m[8])) + (F24(r[4]) * m[9]) + (F24(r[8]) * m[10])).Store();
    o[9] = (((F24(r[1]) * m[8]) + (F24(r[13]) * m[11])) + (F24(r[5]) * m[9]) + (F24(r[9]) * m[10])).Store();
    o[10] = (((F24(r[2]) * m[8]) + (F24(r[14]) * m[11])) + (F24(r[6]) * m[9]) + (F24(r[10]) * m[10])).Store();
    o[11] = (((F24(r[3]) * m[8]) + (F24(r[15]) * m[11])) + (F24(r[7]) * m[9]) + (F24(r[11]) * m[10])).Store();
    o[12] = (((F24(r[12]) * m[15]) + (F24(r[0]) * m[12])) + (F24(r[4]) * m[13]) + (F24(r[8]) * m[14])).Store();
    o[13] = (((F24(r[1]) * m[12]) + (F24(r[13]) * m[15])) + (F24(r[5]) * m[13]) + (F24(r[9]) * m[14])).Store();
    o[14] = (((F24(r[2]) * m[12]) + (F24(r[14]) * m[15])) + (F24(r[6]) * m[13]) + (F24(r[10]) * m[14])).Store();
    o[15] = (((F24(r[3]) * m[12]) + (F24(r[15]) * m[15])) + (F24(r[7]) * m[13]) + (F24(r[11]) * m[14])).Store();
    std::memcpy(dst, o, sizeof(o));
}

} // namespace

F24 Noise1(const CameraData& d, float x) {
    int64_t i0 = X87::FtoL2Lo(x);
    double fi = static_cast<double>(i0);
    if (!(fi <= x || std::isnan(x))) fi = R24(fi - 1.0).V;              // fcom x; test ah,41h; jnz
    int n = X87::FtoL2Lo(fi);
    F24 frac = R24(static_cast<double>(x) - n);                         // fild n; fsubr x
    F24 v0 = F24(CameraMath::One) - F24::MulDouble(d.Inv2p30, Hash(n));
    F24 a = v0 * (F24(CameraMath::One) - frac);
    F24 v1 = F24(CameraMath::One) - F24::MulDouble(d.Inv2p30, Hash(static_cast<int>(static_cast<uint32_t>(n) + 1u)));
    return a + v1 * frac;
}

F24 Octaves(const CameraData& d, float x) {
    float sum = 0.0f, freq = CameraMath::One, amp = CameraMath::One;
    for (int k = 0; k < 6; k++) {
        float arg = (F24(freq) * x).Store();
        sum = (Noise1(d, arg) * amp + sum).Store();
        freq = (F24(freq) + freq).Store();
        amp = (F24(d.Half) * amp).Store();
    }
    return sum;
}

uint16_t FovScaledAngle(const CameraData& d, const ViewCamera& cam, int a) {
    float s1 = BWareMath::Sin(static_cast<uint16_t>(cam.Current.Fov >> 1));
    float p = (F24(BWareMath::Sin(static_cast<uint16_t>(a))) * s1).Store();
    float s3 = BWareMath::Sin(static_cast<uint16_t>(d.NoiseRefFov >> 1));
    float r = (F24(p) / s3).Store();
    return d.Math.ASin(r);
}

void Apply(const CameraData& d, const ViewCamera& cam, float* m, float time, float scale) {
    const float* nz = cam.Current.Noise;
    float b0 = (F24(nz[0]) * time).Store(), b1 = (F24(nz[1]) * time).Store();
    float b2 = (F24(nz[2]) * time).Store(), b3 = (F24(nz[3]) * time).Store();
    float n0 = Octaves(d, b0).Store(), n1 = Octaves(d, b1).Store(), n2 = Octaves(d, b2).Store();
    F24 n3 = Octaves(d, b3);
    float n0a = (F24(n0) * nz[4]).Store(), n1a = (F24(n1) * nz[5]).Store(), n2a = (F24(n2) * nz[6]).Store();
    float n3a = (n3 * nz[7]).Store();
    float c0 = (F24(nz[8]) * time).Store(), c1 = (F24(nz[9]) * time).Store();
    float c2 = (F24(nz[10]) * time).Store(), c3 = (F24(nz[11]) * time).Store();
    float m0 = Octaves(d, c0).Store(), m1 = Octaves(d, c1).Store(), m2 = Octaves(d, c2).Store();
    F24 m3 = Octaves(d, c3);
    F24 m0a = F24(m0) * nz[12];
    float m1a = (F24(m1) * nz[13]).Store(), m2a = (F24(m2) * nz[14]).Store(), m3a = (m3 * nz[15]).Store();
    F24 s0 = m0a + n0a;
    F24 s1 = F24(m1a) + n1a;
    float s2 = (F24(m2a) + n2a).Store();
    float s3 = (F24(m3a) + n3a).Store();
    float t0 = (s0 * scale).Store(), t1 = (s1 * scale).Store();
    float t2 = (F24(s2) * scale).Store(), t3 = (F24(s3) * scale).Store();
    float r[16];
    CameraMath::Identity(r);
    r[12] = t0; r[13] = t1;
    int ax = X87::FtoL2Lo((F24(t2) * d.K65536).V) / 360;
    CameraMath::RotateX(r, r, FovScaledAngle(d, cam, ax));
    int ay = X87::FtoL2Lo((F24(t3) * d.K65536).V) / 360;
    CameraMath::RotateY(r, r, FovScaledAngle(d, cam, ay));
    MulMR(m, m, r);
}

std::array<float, 4> SpeedTable(const CameraData& d, float inv, float x) {
    const int count = 5;
    const float* t = d.NoiseSpeedTable;
    float f = ((F24(x) - 0.0f) * inv).Store();
    int i = X87::FtoL2Lo(((F24(x) - 0.0f) * inv).V);
    std::array<float, 4> o = {};
    if (i < 0) { std::memcpy(o.data(), t, 4 * sizeof(float)); return o; }
    if (i >= count - 1) { std::memcpy(o.data(), t + 4 * (count - 1), 4 * sizeof(float)); return o; }
    F24 fi = F24::MulDouble(CameraMath::One, i);
    if (!BWareMath2::LeOrNaN(fi, f)) fi = fi - CameraMath::One;
    float frac = (F24(f) - fi).Store();
    int a = 4 * (i + 1), b = 4 * i;
    o[0] = (F24(frac) * t[a]).Store();
    o[1] = (F24(t[a + 1]) * frac).Store();
    o[2] = (F24(t[a + 2]) * frac).Store();
    o[3] = (F24(t[a + 3]) * frac).Store();
    float omt = (F24(CameraMath::One) - frac).Store();
    const float tb[4] = { t[b], t[b + 1], t[b + 2], t[b + 3] };
    CameraMath::ScaleAdd4(o.data(), o.data(), tb, omt);
    return o;
}

void TerrainVelocityNoise(const CameraData& d, ViewCamera& cam, float* m, const CameraCar& car, float ampScale, float freqScale, int worldTimer) {
    float maxv = car.HasRideInfo ? car.RideInfo20 : d.NoiseDefaultMaxSpeed;
    float inv = (F24(d.NoiseTableSpan) / maxv).Store();
    std::array<float, 4> o = SpeedTable(d, inv, car.Speed);
    float v60 = o[2];
    const int* tr = car.WheelTerrain;
    float b01 = (F24(d.TerrainB(tr[0])) + d.TerrainB(tr[1])).Store();
    F24 a01 = F24(d.TerrainA(tr[0])) + d.TerrainA(tr[1]);
    F24 b012 = F24(b01) + d.TerrainB(tr[2]);
    float a012 = (a01 + d.TerrainA(tr[2])).Store();
    F24 b0123 = b012 + d.TerrainB(tr[3]);
    float A = ((F24(a012) + d.TerrainA(tr[3])) * d.Quarter).Store();
    float B = (F24(d.TerrainAmpScale) * b0123 * d.Quarter).Store();
    float v5C = (F24(o[0]) * ampScale).Store();
    if (car.IsNitrousOn() != 0 && car.IsBlowOff() == 0) {
        v5C = (F24(d.NosAmpAdd) + v5C).Store();
        v60 = (F24(d.NosFreqMul) * v60).Store();
    }
    F24 a2 = F24(o[1]) * B * freqScale;
    float v58 = (F24(o[3]) * A).Store();
    const float* k = d.NoiseK;
    float* nz = cam.Current.Noise;
    for (int i = 0; i < 4; i++) {
        nz[i] = (F24(k[i]) * v60).Store();
        nz[4 + i] = (F24(k[4 + i]) * v5C).Store();
        nz[8 + i] = (F24(k[8 + i]) * v58).Store();
        nz[12 + i] = (F24(k[12 + i]) * a2).Store();
    }
    float time = F24::MulDouble(d.TickToSec, worldTimer).Store();
    Apply(d, cam, m, time, CameraMath::One);
}

} // namespace CameraNoise

F24 CameraShaker::Curve(const std::vector<float>& p, float x) {
    int n = static_cast<int>(p.size()) / 2;
    if (!BWareMath2::GeOrNaN(x, p[0])) return 0.0f;
    if (!BWareMath2::LtOrNaN(x, p[2 * n - 2])) return 0.0f;
    int i = 0;
    for (; i < n; i++) if (!BWareMath2::GeOrNaN(x, p[2 * i])) break;
    if (i > 0) i--;
    if (i > n - 2) return F24(0.0f) * (F24(x) - p[2 * i]) + p[2 * i + 1];
    F24 slope = (F24(p[2 * i + 3]) - p[2 * i + 1]) / (F24(p[2 * i + 2]) - p[2 * i]);
    return slope * (F24(x) - p[2 * i]) + p[2 * i + 1];
}

std::array<float, 3> CameraShaker::Offset(const CameraData& d) const {
    int n = X87::FtoL2Lo((F24(Flicker) * T).V);
    F24 v = ((~n & 1) != 0 && Active) ? Curve(d.ShakeCurve, T) : F24(0.0f);
    return { (v * Dir[0]).Store(), (F24(Dir[1]) * v).Store(), (F24(Dir[2]) * v).Store() };
}

void CameraShaker::Update(const CameraData& d, float dt) {
    if (!Active) return;
    T = (F24(dt) / Duration + T).Store();
    const std::vector<float>& p = d.ShakeCurve;
    if (!BWareMath2::GeOrNaN(T, p[0])) { Active = false; return; }
    if (!BWareMath2::LtOrNaN(T, p[p.size() - 2])) Active = false;
}

void CameraShaker::Maybe(const CameraData& d, const float* accel, bool povInCar, int playerCinematic) {
    if (povInCar) return;
    float x = (F24(d.ShakeAccelScale[0]) * accel[0]).Store();
    float y = (F24(d.ShakeAccelScale[1]) * accel[1]).Store();
    float z = (F24(d.ShakeAccelScale[2]) * accel[2]).Store();
    F24 mag = F24::Sqrt(F24(z) * z + F24(x) * x + F24(y) * y);
    if (!BWareMath2::GeOrNaN(mag, d.ShakeMin)) return;
    if (!BWareMath2::LeOrNaN(mag, d.ShakeMax)) mag = d.ShakeMax;
    float t = ((mag - d.ShakeMin) / (F24(d.ShakeMax) - d.ShakeMin)).Store();
    float amp = ((F24(d.ShakeAmpHi) - d.ShakeAmpLo) * t + d.ShakeAmpLo).Store();
    F24 cur = Active ? Curve(d.ShakeCurve, T) : F24(0.0f);
    if (BWareMath2::LeOrNaN(amp, cur)) return;
    float dur = ((F24(d.ShakeDurHi) - d.ShakeDurLo) * t + d.ShakeDurLo).Store();
    if (playerCinematic == 7) {
        amp = (F24(amp) * d.ShakeCineAmp).Store();
        dur = (F24(dur) * d.ShakeCineDur).Store();
    }
    float dir[3];
    const float v[3] = { x, y, z };
    BWareMath2::NormalizeLenX87(dir, v, amp);
    Dir[0] = -dir[0]; Dir[1] = -dir[1]; Dir[2] = -dir[2];
    Active = true; T = 0.0f; Duration = dur; Flicker = d.ShakeFlicker;
}

void CameraShaker::Apply(const CameraData& d, float* m, float worldTimeScale) const {
    if (!BWareMath2::GeOrNaN(worldTimeScale, CameraMath::One)) return;
    std::array<float, 3> off = Offset(d);
    F24 Z0 = 0.0f;
    float X = (F24(off[2]) * m[8] + F24(off[1]) * m[4] + F24(off[0]) * m[0] + F24(m[12]) * Z0).Store();
    float Y = (F24(off[1]) * m[5] + F24(off[2]) * m[9] + F24(off[0]) * m[1] + F24(m[13]) * Z0).Store();
    float Zc = (F24(off[1]) * m[6] + F24(off[2]) * m[10] + F24(off[0]) * m[2] + F24(m[14]) * Z0).Store();
    float r[16];
    CameraMath::Identity(r);
    r[12] = X; r[13] = Y; r[14] = Zc; r[15] = CameraMath::One;
    CameraMath::RotateX(r, r, static_cast<uint16_t>(X87::FtoL2Lo(((F24(r[13]) + r[14]) * d.ShakeRotX).V)));
    CameraMath::RotateY(r, r, static_cast<uint16_t>(X87::FtoL2Lo((F24(d.ShakeRotY) * r[12]).V)));
    CameraMath::RotateZ(r, r, static_cast<uint16_t>(X87::FtoL2Lo(((F24(r[12]) + r[13]) * d.ShakeRotZ).V)));
    float o[16];
    o[0] = (((F24(r[4]) * m[1]) + (F24(r[0]) * m[0])) + (F24(r[8]) * m[2]) + (F24(r[12]) * m[3])).Store();
    o[1] = (((F24(r[5]) * m[1]) + (F24(r[1]) * m[0])) + (F24(r[9]) * m[2]) + (F24(r[13]) * m[3])).Store();
    o[2] = (((F24(r[6]) * m[1]) + (F24(r[2]) * m[0])) + (F24(r[10]) * m[2]) + (F24(r[14]) * m[3])).Store();
    o[3] = (((F24(r[7]) * m[1]) + (F24(r[3]) * m[0])) + (F24(r[11]) * m[2]) + (F24(r[15]) * m[3])).Store();
    o[4] = (((F24(r[4]) * m[5]) + (F24(r[12]) * m[7])) + (F24(r[0]) * m[4]) + (F24(r[8]) * m[6])).Store();
    o[5] = (((F24(r[5]) * m[5]) + (F24(r[13]) * m[7])) + (F24(r[1]) * m[4]) + (F24(r[9]) * m[6])).Store();
    o[6] = (((F24(r[6]) * m[5]) + (F24(r[14]) * m[7])) + (F24(r[2]) * m[4]) + (F24(r[10]) * m[6])).Store();
    o[7] = (((F24(r[7]) * m[5]) + (F24(r[15]) * m[7])) + (F24(r[3]) * m[4]) + (F24(r[11]) * m[6])).Store();
    o[8] = (((F24(r[8]) * m[10]) + (F24(r[12]) * m[11])) + (F24(r[0]) * m[8]) + (F24(r[4]) * m[9])).Store();
    o[9] = (((F24(r[9]) * m[10]) + (F24(r[13]) * m[11])) + (F24(r[1]) * m[8]) + (F24(r[5]) * m[9])).Store();
    o[10] = (((F24(r[10]) * m[10]) + (F24(r[14]) * m[11])) + (F24(r[2]) * m[8]) + (F24(r[6]) * m[9])).Store();
    o[11] = (((F24(r[11]) * m[10]) + (F24(r[15]) * m[11])) + (F24(r[3]) * m[8]) + (F24(r[7]) * m[9])).Store();
    o[12] = (((F24(r[4]) * m[13]) + (F24(r[0]) * m[12])) + (F24(r[12]) * m[15]) + (F24(r[8]) * m[14])).Store();
    o[13] = (((F24(r[5]) * m[13]) + (F24(r[1]) * m[12])) + (F24(r[13]) * m[15]) + (F24(r[9]) * m[14])).Store();
    o[14] = (((F24(r[6]) * m[13]) + (F24(r[2]) * m[12])) + (F24(r[14]) * m[15]) + (F24(r[10]) * m[14])).Store();
    o[15] = (((F24(r[7]) * m[13]) + (F24(r[3]) * m[12])) + (F24(r[15]) * m[15]) + (F24(r[11]) * m[14])).Store();
    std::memcpy(m, o, sizeof(o));
}

} // namespace bb
