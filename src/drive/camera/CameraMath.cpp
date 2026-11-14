// Port of NFSU2Unity Core/Camera/CameraMath.cs.

#include "drive/camera/CameraMath.h"

#include <cstring>

namespace bb {

namespace CameraMath {

void Identity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = One; m[5] = One; m[10] = One; m[15] = One;
}

void MatMulSse(float* o, const float* a, const float* b) {
    float t[16];
    for (int j = 0; j < 4; j++) {
        float b0 = b[j], b1 = b[4 + j], b2 = b[8 + j], b3 = b[12 + j];
        t[j] = static_cast<float>(static_cast<float>(static_cast<float>(a[0] * b0) + static_cast<float>(a[1] * b1)) + static_cast<float>(static_cast<float>(a[2] * b2) + static_cast<float>(a[3] * b3)));
        t[4 + j] = static_cast<float>(static_cast<float>(static_cast<float>(static_cast<float>(a[4] * b0) + static_cast<float>(a[5] * b1)) + static_cast<float>(a[6] * b2)) + static_cast<float>(a[7] * b3));
        t[8 + j] = static_cast<float>(static_cast<float>(static_cast<float>(static_cast<float>(a[8] * b0) + static_cast<float>(a[9] * b1)) + static_cast<float>(a[10] * b2)) + static_cast<float>(a[11] * b3));
        t[12 + j] = static_cast<float>(static_cast<float>(static_cast<float>(a[12] * b0) + static_cast<float>(a[13] * b1)) + static_cast<float>(static_cast<float>(a[14] * b2) + static_cast<float>(a[15] * b3)));
    }
    std::memcpy(o, t, sizeof(t));
}

void RotateX(float* dst, const float* src, uint16_t angle) {
    float c = BWareMath::Cos(angle), s = BWareMath::Sin(angle);
    float r[16] = {};
    r[0] = One; r[5] = c; r[6] = s; r[9] = -s; r[10] = c; r[15] = One;
    MatMulSse(dst, src, r);
}

void RotateY(float* dst, const float* src, uint16_t angle) {
    float c = BWareMath::Cos(angle), s = BWareMath::Sin(angle);
    float r[16] = {};
    r[0] = c; r[2] = -s; r[5] = One; r[8] = s; r[10] = c; r[15] = One;
    MatMulSse(dst, src, r);
}

void RotateZ(float* dst, const float* src, uint16_t angle) {
    float c = BWareMath::Cos(angle), s = BWareMath::Sin(angle);
    float r[16] = {};
    r[0] = c; r[1] = s; r[4] = -s; r[5] = c; r[10] = One; r[15] = One;
    MatMulSse(dst, src, r);
}

void InvertTransformation(float* o, const float* m) {
    float rt[16] = {};
    rt[0] = m[0]; rt[1] = m[4]; rt[2] = m[8];
    rt[4] = m[1]; rt[5] = m[5]; rt[6] = m[9];
    rt[8] = m[2]; rt[9] = m[6]; rt[10] = m[10];
    rt[15] = One;
    float t[16] = {};
    t[0] = One; t[5] = One; t[10] = One; t[15] = One;
    t[12] = -m[12]; t[13] = -m[13]; t[14] = -m[14];
    MatMulSse(o, t, rt);
}

void MulVec4(float* o, const float* m, const float* v) {
    F24 x = F24(m[12]) * v[3] + F24(m[8]) * v[2] + F24(m[4]) * v[1] + F24(m[0]) * v[0];
    F24 y = F24(m[13]) * v[3] + F24(m[1]) * v[0] + F24(m[9]) * v[2] + F24(m[5]) * v[1];
    F24 z = F24(m[14]) * v[3] + F24(m[2]) * v[0] + F24(m[10]) * v[2] + F24(m[6]) * v[1];
    F24 w = F24(m[15]) * v[3] + F24(m[3]) * v[0] + F24(m[11]) * v[2] + F24(m[7]) * v[1];
    o[3] = w.Store(); o[0] = x.Store(); o[1] = y.Store(); o[2] = z.Store();
}

F24 DistBetween(const float* a, const float* b) {
    F24 dx = F24(a[0]) - b[0], dy = F24(a[1]) - b[1], dz = F24(a[2]) - b[2];
    return F24::Sqrt(dz * dz + dy * dy + dx * dx);
}

void ClampVec(float* v, const float* lo, const float* hi) {
    for (int i = 0; i < 3; i++) v[i] = BWareMath2::Clamp(v[i], lo[i], hi[i]);
}

void ScaleAdd4(float* o, const float* a, const float* b, float s) {
    float y = (F24(s) * b[1] + a[1]).Store();
    float z = (F24(s) * b[2] + a[2]).Store();
    float w = (F24(s) * b[3] + a[3]).Store();
    float x = (F24(s) * b[0] + a[0]).Store();
    o[3] = w; o[0] = x; o[1] = y; o[2] = z;
}

void LookAt(float* o, const float* eye, const float* look, const float* up) {
    float f[3] = { (F24(look[0]) - eye[0]).Store(), (F24(look[1]) - eye[1]).Store(), (F24(look[2]) - eye[2]).Store() };
    BWareMath2::NormalizeX87(f, f);
    float nu[3] = { -up[0], -up[1], -up[2] };
    float r[3];
    BWareMath::CrossX87(r, nu, f);
    float d[3];
    BWareMath::CrossX87(d, f, r);
    BWareMath2::NormalizeX87(r, r);
    BWareMath2::NormalizeX87(d, d);
    F24 ex = -F24(eye[0]), ey = -F24(eye[1]), ez = -F24(eye[2]);
    F24 Z = 0.0f;
    F24 z0r = F24(r[2]) * Z, y0r = F24(r[1]) * Z;
    float m0 = (F24(r[0]) + y0r + z0r).Store();
    F24 z0d = F24(d[2]) * Z;
    float y0d = (F24(d[1]) * Z).Store();
    float m1 = (F24(d[0]) + y0d + z0d).Store();
    F24 z0f = F24(f[2]) * Z;
    float y0f = (F24(f[1]) * Z).Store();
    float m2 = (F24(f[0]) + y0f + z0f).Store();
    float x0r = (F24(r[0]) * Z).Store();
    float m4 = (F24(r[1]) + x0r + z0r).Store();
    float x0d = (F24(d[0]) * Z).Store();
    float m5 = (F24(d[1]) + x0d + z0d).Store();
    float x0f = (F24(f[0]) * Z).Store();
    float m6 = (F24(f[1]) + x0f + z0f).Store();
    float m8 = (F24(r[2]) + x0r + y0r).Store();
    float m9 = (F24(d[2]) + x0d + y0d).Store();
    float m10 = (F24(f[2]) + x0f + y0f).Store();
    float m12 = (F24(r[2]) * ez + ey * r[1] + ex * r[0]).Store();
    float m13 = (F24(d[2]) * ez + ey * d[1] + ex * d[0]).Store();
    float m14 = (F24(f[2]) * ez + ey * f[1] + ex * f[0]).Store();
    float m15 = (ez * Z + ey * Z + ex * Z + One).Store();
    o[0] = m0; o[1] = m1; o[2] = m2; o[3] = 0.0f;
    o[4] = m4; o[5] = m5; o[6] = m6; o[7] = 0.0f;
    o[8] = m8; o[9] = m9; o[10] = m10; o[11] = 0.0f;
    o[12] = m12; o[13] = m13; o[14] = m14; o[15] = m15;
}

} // namespace CameraMath

} // namespace bb
