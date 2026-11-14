#include "drive/numerics/BWareMath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace bb {

namespace {

const float K_AngToRad = X87::Bits(0x38C90FDBu);   // flt_784240 2pi/65536
const float K_RadToAng = X87::Bits(0x4622F983u);   // flt_78423C 65536/2pi
const float K_3PiOver2 = X87::Bits(0x4096CBE4u);   // flt_786DA0
const float K_PiOver2 = X87::Bits(0x3FC90FDBu);    // flt_784254
const float K_2Pi = X87::Bits(0x40C90FDBu);        // flt_786D9C
const float K_Pi = X87::Bits(0x40490FDBu);         // flt_786D98
const float K_S3 = X87::Bits(0x3E2AAAABu);         // flt_786D94 1/3!
const float K_S5 = X87::Bits(0x3C088889u);         // flt_786D90 1/5!
const float K_S7 = X87::Bits(0x39500D01u);         // flt_786D8C 1/7!
const float K_S9 = X87::Bits(0x3638EF1Du);         // flt_786D88 1/9!
const float K_C4 = X87::Bits(0x3D2AAAABu);         // flt_786DB0 1/4!
const float K_C6 = X87::Bits(0x3AB60B61u);         // flt_786DAC 1/6!
const float K_C8 = X87::Bits(0x37D00D01u);         // flt_786DA8 1/8!
const float K_C10 = X87::Bits(0x3493F27Eu);        // flt_786DA4 1/10!
const float K_Minus256 = X87::Bits(0xC3800000u);   // flt_786DB8
const float K_65536 = X87::Bits(0x47800000u);      // flt_784238
const float K_Inv65536 = X87::Bits(0x37800000u);   // flt_784278
const float K_Minus65536 = X87::Bits(0xC7800000u); // flt_786DB4
const float K_ATanEps = X87::Bits(0x38D1B717u);    // flt_7843EC

void Check(const Speed2Image& exe, uint32_t va, float k) {
    if (exe.U32(va) != X87::Bits(k)) {
        char text[80];
        std::snprintf(text, sizeof text, "constant at 0x%08X differs from the exe", va);
        throw std::runtime_error(text);
    }
}

using BWareMath2::GeOrNaN;
using BWareMath2::LeOrNaN;
using BWareMath2::NeOrNaN;

} // namespace

// ============================================================== BWareMath

BWareMath::BWareMath(const Speed2Image& exe) {
    auto table = exe.Bytes(VA_FastATanTable, 257);
    std::memcpy(fastATan_, table.data(), 257);
    for (int i = 0; i < 208; ++i) {
        asinAngle_[i] = exe.U16(VA_ASinTable + static_cast<uint32_t>(8 * i));
        asinSlope_[i] = exe.F32(VA_ASinTable + static_cast<uint32_t>(8 * i) + 4);
    }
    Check(exe, 0x784240, K_AngToRad); Check(exe, 0x78423C, K_RadToAng); Check(exe, 0x786DA0, K_3PiOver2);
    Check(exe, 0x784254, K_PiOver2); Check(exe, 0x786D9C, K_2Pi); Check(exe, 0x786D98, K_Pi);
    Check(exe, 0x786D94, K_S3); Check(exe, 0x786D90, K_S5); Check(exe, 0x786D8C, K_S7); Check(exe, 0x786D88, K_S9);
    Check(exe, 0x786DB8, K_Minus256); Check(exe, 0x784238, K_65536); Check(exe, 0x784278, K_Inv65536);
    Check(exe, 0x786DB4, K_Minus65536); Check(exe, 0x7843EC, K_ATanEps);
}

float BWareMath::Sin(uint16_t angle) {
    float x = M(static_cast<float>(angle), K_AngToRad);
    float s = 1.0f;
    if (!(x < K_3PiOver2)) x = S(x, K_2Pi);
    else if (!(x < K_PiOver2)) { x = S(x, K_Pi); s = -1.0f; }
    float x2 = M(x, x);
    float x3 = M(x2, x);
    float x5 = M(x3, x2);
    float x7 = M(x5, x2);
    float r = S(x, M(x3, K_S3));
    r = A(r, M(x5, K_S5));
    r = S(r, M(x7, K_S7));
    r = A(r, M(M(x7, x2), K_S9));
    return M(r, s);
}

float BWareMath::Cos(uint16_t angle) { return Sin(static_cast<uint16_t>(angle + 0x4000)); }

float BWareMath::Sin(float radians) { return Sin(static_cast<uint16_t>(X87::FtoL2Lo(M(radians, K_RadToAng)))); }

uint16_t BWareMath::FastATan(float x, float y) const {
    int quad = 0;
    if (X87::ParityEven(X87::Fcom(x, 0.0f) & 0x05) == false) { x = -x; quad = 1; }
    float ay = y;
    if (X87::ParityEven(X87::Fcom(y, 0.0f) & 0x05) == false) { quad ^= 3; ay = -y; }
    int a;
    if ((X87::Fcom(x, ay) & 0x41) == 0) {
        int idx = X87::FtoL2Lo(M(D(ay, x), K_Minus256));
        a = fastATan_[-idx] << 5;
    } else if ((X87::Fcom(ay, x) & 0x41) == 0) {
        int idx = X87::FtoL2Lo(M(D(x, ay), K_Minus256));
        a = 0x4000 - (fastATan_[-idx] << 5);
    } else {
        a = X87::ParityEven(X87::Fcom(ay, 0.0f) & 0x44) ? 0x2000 : 0;
    }
    switch (quad) {
    case 0: return static_cast<uint16_t>(a);
    case 3: return static_cast<uint16_t>(-a);
    case 1: return static_cast<uint16_t>(0x8000 - a);
    default: return static_cast<uint16_t>(a + 0x8000);
    }
}

uint16_t BWareMath::ASin(float t) const {
    bool neg = false;
    if (!X87::ParityEven(X87::Fcom(t, 0.0f) & 0x05)) { t = -t; neg = true; }
    if ((X87::Fcom(t, 1.0f) & X87::C0) == 0) return neg ? static_cast<uint16_t>(0xC000) : static_cast<uint16_t>(0x4000);
    int v = X87::FtoL2Lo(M(t, K_65536));
    int width = 0x8000, top = 0x8000, seg = 0;
    if (v >= top) {
        do {
            if (seg >= 11) break;
            width >>= 1; top += width; seg++;
        } while (v >= top);
    }
    int bas = top - width;
    int sub = (v - bas) >> (11 - seg);
    int xi = (width >> 4) * sub + bas;
    int entry = (seg << 4) + sub;
    float d = M(M(S(t, M(static_cast<float>(xi), K_Inv65536)), asinSlope_[entry]), K_Minus65536);
    int dd = X87::FtoL2Lo(d);
    uint16_t cx = static_cast<uint16_t>(asinAngle_[entry] - static_cast<uint16_t>(dd));
    return neg ? static_cast<uint16_t>(0x10000 - cx) : cx;
}

uint16_t BWareMath::ATan(float x, float y) const {
    int quad = 0;
    if (!X87::ParityEven(X87::Fcom(x, 0.0f) & 0x05)) quad = 1;
    if (!X87::ParityEven(X87::Fcom(y, 0.0f) & 0x05)) quad ^= 3;
    float ay = std::fabs(y), ax = std::fabs(x);
    int s1 = X87::Fcom(ay, ax);
    float r2 = A(M(x, x), M(y, y));
    int s2 = X87::Fcom(r2, K_ATanEps);
    int angle;
    if (!X87::ParityEven(s1 & 0x05)) {
        float c = 0.0f;
        if ((s2 & 0x41) == 0 && X87::ParityEven(X87::Fcom(y, 0.0f) & 0x44)) c = D(ay, std::sqrt(r2));
        angle = ASin(c);
    } else {
        float c = 0.0f;
        if ((s2 & 0x41) == 0 && X87::ParityEven(X87::Fcom(x, 0.0f) & 0x44)) c = D(ax, std::sqrt(r2));
        angle = 0x4000 - ASin(c);
    }
    switch (quad) {
    case 0: return static_cast<uint16_t>(angle);
    case 3: return static_cast<uint16_t>(0x10000 - angle);
    case 1: return static_cast<uint16_t>(0x8000 - angle);
    default: return static_cast<uint16_t>(angle + 0x8000);
    }
}

void BWareMath::Normalize(float* o, const float* v) {
    float len = std::sqrt(A(A(M(v[0], v[0]), M(v[1], v[1])), M(v[2], v[2])));
    if (!X87::ParityEven(X87::Fcom(len, 0.0f) & 0x44)) { o[0] = 1.0f; o[1] = 0.0f; o[2] = 0.0f; return; }
    float inv = D(1.0f, len);
    float z = v[2];
    float x = M(inv, v[0]);
    float y = M(v[1], inv);
    o[0] = x; o[1] = y; o[2] = M(z, inv);
}

void BWareMath::Cross(float* o, const float* a, const float* b) {
    float y = S(M(b[0], a[2]), M(b[2], a[0]));
    float z = S(M(b[1], a[0]), M(a[1], b[0]));
    float x = S(M(b[2], a[1]), M(b[1], a[2]));
    o[0] = x; o[1] = y; o[2] = z;
}

void BWareMath::CrossX87(float* o, const float* a, const float* b) {
    F24 y = F24(b[0]) * a[2] - F24(b[2]) * a[0];
    F24 z = F24(b[1]) * a[0] - F24(a[1]) * b[0];
    F24 x = F24(b[2]) * a[1] - F24(b[1]) * a[2];
    o[0] = x.Store(); o[1] = y.Store(); o[2] = z.Store();
}

void BWareMath::MulVector3(float* o, const float* m, const float* v) {
    float x = A(A(A(M(m[8], v[2]), M(m[4], v[1])), M(m[0], v[0])), m[12]);
    float y = A(A(A(M(m[9], v[2]), M(m[1], v[0])), M(m[5], v[1])), m[13]);
    float z = A(A(A(M(m[10], v[2]), M(m[2], v[0])), M(m[6], v[1])), m[14]);
    o[0] = x; o[1] = y; o[2] = z;
}

void BWareMath::MulMatrix(float* outM, const float* a, const float* b) {
    float o[16];
    o[0] = A(A(A(M(a[0], b[0]), M(a[3], b[12])), M(a[2], b[8])), M(b[4], a[1]));
    o[1] = A(A(A(M(b[13], a[3]), M(a[1], b[5])), M(b[9], a[2])), M(b[1], a[0]));
    o[2] = A(A(A(M(b[10], a[2]), M(b[2], a[0])), M(a[3], b[14])), M(b[6], a[1]));
    o[3] = A(A(A(M(b[15], a[3]), M(a[2], b[11])), M(a[0], b[3])), M(b[7], a[1]));
    o[4] = A(A(A(M(a[6], b[8]), M(a[7], b[12])), M(b[4], a[5])), M(a[4], b[0]));
    o[5] = A(A(A(M(a[6], b[9]), M(a[4], b[1])), M(a[5], b[5])), M(a[7], b[13]));
    o[6] = A(A(A(M(a[7], b[14]), M(a[6], b[10])), M(b[6], a[5])), M(a[4], b[2]));
    o[7] = A(A(A(M(b[15], a[7]), M(a[6], b[11])), M(a[4], b[3])), M(b[7], a[5]));
    o[8] = A(A(A(M(a[10], b[8]), M(a[11], b[12])), M(b[4], a[9])), M(a[8], b[0]));
    o[9] = A(A(A(M(a[10], b[9]), M(a[8], b[1])), M(a[9], b[5])), M(a[11], b[13]));
    o[10] = A(A(A(M(a[11], b[14]), M(a[10], b[10])), M(b[6], a[9])), M(a[8], b[2]));
    o[11] = A(A(A(M(b[15], a[11]), M(a[10], b[11])), M(a[8], b[3])), M(b[7], a[9]));
    o[12] = A(A(A(M(a[14], b[8]), M(a[15], b[12])), M(b[4], a[13])), M(a[12], b[0]));
    o[13] = A(A(A(M(a[14], b[9]), M(a[12], b[1])), M(a[13], b[5])), M(a[15], b[13]));
    o[14] = A(A(A(M(a[15], b[14]), M(a[14], b[10])), M(b[6], a[13])), M(a[12], b[2]));
    o[15] = A(A(A(M(b[15], a[15]), M(a[14], b[11])), M(a[12], b[3])), M(b[7], a[13]));
    std::memcpy(outM, o, sizeof o);
}

float BWareMath::TableGetValue(int numEntries, float minArg, float indexMultiplier, const float* data, float arg) {
    float f = M(S(arg, minArg), indexMultiplier);
    int i = X87::FtoL2Lo(f);
    if (i < 0) return data[0];
    if (!X87::ParityEven(X87::Fcom(f, 0.0f) & 0x05)) return data[0];
    if (i >= numEntries - 1) return data[numEntries - 1];
    float fi = static_cast<float>(i);
    if ((X87::Fcom(fi, f) & 0x41) == 0) fi = S(fi, 1.0f);
    float frac = S(f, fi);
    float w0 = S(1.0f, frac);
    return A(M(w0, data[i]), M(frac, data[i + 1]));
}

// ============================================================== BWareMath2

namespace BWareMath2 {

const float K_One = X87::Bits(0x3F800000u);
const float K_Half = X87::Bits(0x3F000000u);
const float K_GraphEps = X87::Bits(0x358637BDu);

void CheckConstants(const Speed2Image& exe) {
    Check(exe, 0x784250, K_One); Check(exe, 0x784260, K_Half); Check(exe, 0x787594, K_GraphEps);
    Check(exe, 0x784240, K_AngToRad); Check(exe, 0x786DA0, K_3PiOver2); Check(exe, 0x784254, K_PiOver2);
    Check(exe, 0x786D9C, K_2Pi); Check(exe, 0x786D98, K_Pi); Check(exe, 0x786D94, K_S3); Check(exe, 0x786D90, K_S5);
    Check(exe, 0x786D8C, K_S7); Check(exe, 0x786D88, K_S9); Check(exe, 0x786DB0, K_C4); Check(exe, 0x786DAC, K_C6);
    Check(exe, 0x786DA8, K_C8); Check(exe, 0x786DA4, K_C10);
}

int Fcom(double a, double b) {
    if (a < b) return X87::C0;
    if (a > b) return 0;
    if (a == b) return X87::C3;
    return X87::C0 | X87::C2 | X87::C3;
}

float Clamp(float x, float lo, float hi) {
    float a = LeOrNaN(x, lo) ? lo : x;
    return GeOrNaN(hi, a) ? a : hi;
}

float Min(float a, float b) { return GeOrNaN(a, b) ? b : a; }

float Lerp576740(float a, float b, float t) {
    F24 r = (F24(K_One) - t) * b + F24(t) * a;
    return r.Store();
}

bool GraphGetValueTo(const float* pts, int count, float x, float& result) {
    if (count <= 1) {
        if (count <= 0) return false;
        result = pts[1];
        return true;
    }
    if (!X87::ParityEven(X87::Fcom(x, pts[0]) & 0x41)) { result = pts[1]; return true; }
    if ((X87::Fcom(x, pts[2 * count - 2]) & X87::C0) == 0) { result = pts[2 * count - 1]; return true; }
    for (int i = 0; i < count - 1; ++i) {
        if ((X87::Fcom(x, pts[2 * i]) & X87::C0) != 0) continue;
        if (X87::ParityEven(X87::Fcom(x, pts[2 * i + 2]) & 0x05)) continue;
        float t = ((F24(x) - pts[2 * i]) / (F24(pts[2 * i + 2]) - pts[2 * i])).Store();
        result = Lerp576740(pts[2 * i + 3], pts[2 * i + 1], t);
        return true;
    }
    return false;
}

void SinCos(uint16_t angle, float& sin, float& cos) {
    F24 x = F24(static_cast<float>(angle)) * K_AngToRad;
    float sign = K_One;
    if ((Fcom(x, F24(K_3PiOver2)) & X87::C0) == 0) x = x - K_2Pi;
    else if ((Fcom(x, F24(K_PiOver2)) & X87::C0) == 0) { x = x - K_Pi; sign = X87::Bits(0xBF800000u); }
    F24 x2 = x * x;
    F24 x3 = x2 * x;
    float x4 = (x2 * x2).Store();
    F24 x5 = x3 * x2;
    float x6 = (F24(x4) * x2).Store();
    F24 x7 = x5 * x2;
    float x8 = (F24(x6) * x2).Store();
    F24 r = x - x3 * K_S3;
    r = r + x5 * K_S5;
    r = r - x7 * K_S7;
    r = r + x7 * x2 * K_S9;
    sin = (r * sign).Store();
    F24 c = F24(K_One) - F24(K_Half) * x2;
    c = c + F24(x4) * K_C4;
    c = c - F24(x6) * K_C6;
    c = c + F24(x8) * K_C8;
    c = c - F24(x8) * x2 * K_C10;
    cos = (c * sign).Store();
}

void ScaleAdd(float* o, const float* a, const float* b, float s) {
    F24 y = F24(s) * b[1] + a[1];
    F24 z = F24(s) * b[2] + a[2];
    F24 x = F24(s) * b[0] + a[0];
    o[0] = x.Store(); o[1] = y.Store(); o[2] = z.Store();
}

void RotateVector(float* o, const float* m, const float* v) {
    o[0] = (F24(m[8]) * v[2] + F24(m[4]) * v[1] + F24(m[0]) * v[0]).Store();
    o[1] = (F24(m[9]) * v[2] + F24(m[1]) * v[0] + F24(m[5]) * v[1]).Store();
    o[2] = (F24(m[10]) * v[2] + F24(m[2]) * v[0] + F24(m[6]) * v[1]).Store();
}

void NormalizeX87(float* o, const float* v) {
    F24 len = F24::Sqrt(F24(v[0]) * v[0] + F24(v[1]) * v[1] + F24(v[2]) * v[2]);
    if (!NeOrNaN(len, F24(0.0f))) { o[0] = 1.0f; o[1] = 0.0f; o[2] = 0.0f; return; }
    F24 inv = F24(K_One) / len;
    float z = v[2], y = v[1];
    o[0] = (inv * v[0]).Store();
    o[1] = (F24(y) * inv).Store();
    o[2] = (F24(z) * inv).Store();
}

void NormalizeLenX87(float* o, const float* v, float len) {
    F24 l = F24::Sqrt(F24(v[0]) * v[0] + F24(v[1]) * v[1] + F24(v[2]) * v[2]) / len;
    if (!NeOrNaN(l, F24(0.0f))) { o[0] = len; o[1] = 0.0f; o[2] = 0.0f; return; }
    F24 inv = F24(K_One) / l;
    float z = v[2], y = v[1];
    o[0] = (inv * v[0]).Store();
    o[1] = (F24(y) * inv).Store();
    o[2] = (F24(z) * inv).Store();
}

void MulVector3X87(float* o, const float* m, const float* v) {
    F24 x = F24(m[8]) * v[2] + F24(m[4]) * v[1] + F24(m[0]) * v[0] + m[12];
    F24 y = F24(m[9]) * v[2] + F24(m[1]) * v[0] + F24(m[5]) * v[1] + m[13];
    F24 z = F24(m[10]) * v[2] + F24(m[2]) * v[0] + F24(m[6]) * v[1] + m[14];
    o[0] = x.Store(); o[1] = y.Store(); o[2] = z.Store();
}

static F24 P(float a, float b) { return F24(a) * b; }

void MulMatrixX87(float* outM, const float* a, const float* b) {
    float o[16];
    o[0] = (P(a[0], b[0]) + P(a[3], b[12]) + P(a[2], b[8]) + P(b[4], a[1])).Store();
    o[1] = (P(b[13], a[3]) + P(a[1], b[5]) + P(b[9], a[2]) + P(b[1], a[0])).Store();
    o[2] = (P(b[10], a[2]) + P(b[2], a[0]) + P(a[3], b[14]) + P(b[6], a[1])).Store();
    o[3] = (P(b[15], a[3]) + P(a[2], b[11]) + P(a[0], b[3]) + P(b[7], a[1])).Store();
    o[4] = (P(a[6], b[8]) + P(a[7], b[12]) + P(b[4], a[5]) + P(a[4], b[0])).Store();
    o[5] = (P(a[6], b[9]) + P(a[4], b[1]) + P(a[5], b[5]) + P(a[7], b[13])).Store();
    o[6] = (P(a[7], b[14]) + P(a[6], b[10]) + P(b[6], a[5]) + P(a[4], b[2])).Store();
    o[7] = (P(b[15], a[7]) + P(a[6], b[11]) + P(a[4], b[3]) + P(b[7], a[5])).Store();
    o[8] = (P(a[10], b[8]) + P(a[11], b[12]) + P(b[4], a[9]) + P(a[8], b[0])).Store();
    o[9] = (P(a[10], b[9]) + P(a[8], b[1]) + P(a[9], b[5]) + P(a[11], b[13])).Store();
    o[10] = (P(a[11], b[14]) + P(a[10], b[10]) + P(b[6], a[9]) + P(a[8], b[2])).Store();
    o[11] = (P(b[15], a[11]) + P(a[10], b[11]) + P(a[8], b[3]) + P(b[7], a[9])).Store();
    o[12] = (P(a[14], b[8]) + P(a[15], b[12]) + P(b[4], a[13]) + P(a[12], b[0])).Store();
    o[13] = (P(a[14], b[9]) + P(a[12], b[1]) + P(a[13], b[5]) + P(a[15], b[13])).Store();
    o[14] = (P(a[15], b[14]) + P(a[14], b[10]) + P(b[6], a[13]) + P(a[12], b[2])).Store();
    o[15] = (P(b[15], a[15]) + P(a[14], b[11]) + P(a[12], b[3]) + P(b[7], a[13])).Store();
    std::memcpy(outM, o, sizeof o);
}

void BMulMatrix(float* o, const float* a, const float* b) { MulMatrixX87(o, b, a); }

void TransposeMatrix(float* o, const float* m) {
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[4 * c + r] = m[4 * r + c];
    std::memcpy(o, t, sizeof t);
}

void CopyMatrix(float* dst, const float* src) { std::memmove(dst, src, 16 * sizeof(float)); }

void InvertMatrix(float* o, const float* m) {
    float m0 = m[0], m1 = m[1], m2 = m[2], m4 = m[4], m5 = m[5], m6 = m[6];
    float m8 = m[8], m9 = m[9], m10 = m[10], m12 = m[12], m13 = m[13], m14 = m[14];
    F24 a = F24(m5) * m10 - F24(m9) * m6;
    float b = (F24(m10) * m4 - F24(m8) * m6).Store();
    float c = (F24(m9) * m4 - F24(m8) * m5).Store();
    F24 det = a * m0 - F24(b) * m1 + F24(c) * m2;
    float inv = (F24(K_One) / det).Store();
    o[3] = 0.0f;
    F24 r0 = a * inv; float v14 = r0.Store(); o[0] = v14;
    F24 r1 = -((F24(m10) * m1 - F24(m9) * m2) * inv); float vC = r1.Store(); o[1] = vC;
    F24 r2 = (F24(m6) * m1 - F24(m5) * m2) * inv; float v4 = r2.Store(); o[2] = v4;
    F24 r4 = -(F24(b) * inv); float v1C = r4.Store(); o[4] = v1C;
    F24 e = F24(m10) * m0 - F24(m8) * m2;
    o[7] = 0.0f; o[11] = 0.0f; o[15] = K_One;
    F24 r5 = e * inv; float v10 = r5.Store(); o[5] = v10;
    F24 r6 = -((F24(m6) * m0 - F24(m4) * m2) * inv); float v8 = r6.Store(); o[6] = v8;
    F24 r8 = F24(c) * inv; float a0 = r8.Store(); o[8] = a0;
    F24 r9 = -((F24(m9) * m0 - F24(m8) * m1) * inv); float v18 = r9.Store(); o[9] = v18;
    F24 r10 = (F24(m0) * m5 - F24(m4) * m1) * inv; o[10] = r10.Store();
    o[12] = (-(F24(a0) * m14 + F24(v1C) * m13 + F24(v14) * m12)).Store();
    o[13] = (-(F24(v18) * m14 + F24(v10) * m13 + F24(vC) * m12)).Store();
    o[14] = (-(r10 * m14 + F24(v8) * m13 + F24(v4) * m12)).Store();
}

void OrthonormalizeMatrix(float* o, const float* m) {
    float x[3], z[3], y[3];
    float row0[3] = {m[0], m[1], m[2]};
    float row1[3] = {m[4], m[5], m[6]};
    NormalizeX87(x, row0);
    BWareMath::CrossX87(z, x, row1);
    NormalizeX87(z, z);
    BWareMath::CrossX87(y, z, x);
    o[0] = x[0]; o[1] = x[1]; o[2] = x[2];
    o[4] = y[0]; o[5] = y[1]; o[6] = y[2];
    o[8] = z[0]; o[9] = z[1]; o[10] = z[2];
    o[3] = 0.0f; o[7] = 0.0f; o[11] = 0.0f; o[12] = 0.0f; o[13] = 0.0f; o[14] = 0.0f; o[15] = K_One;
}

} // namespace BWareMath2

} // namespace bb
