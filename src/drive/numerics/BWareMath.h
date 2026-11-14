#pragma once

// Ports of NFSU2Unity Core/Numerics/BWareMath.cs and BWareMath2.cs: the
// SPEED2.EXE "bWare" maths helpers the vehicle physics uses, in the x87
// instruction order of the original. Matrices are bMatrix4 = 16 floats,
// row-major; vectors are 3 (or 4) floats.

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <cstdint>

namespace bb {

class BWareMath {
public:
    // ---- arithmetic primitives: one IEEE binary32 rounding per operation, never fused
    static float A(float x, float y) { return x + y; }
    static float S(float x, float y) { return x - y; }
    static float M(float x, float y) { return x * y; }
    static float D(float x, float y) { return x / y; }

    static constexpr uint32_t VA_FastATanTable = 0x7EACB0;
    static constexpr uint32_t VA_ASinTable = 0x7EA628;

    explicit BWareMath(const Speed2Image& exe);

    static float Sin(uint16_t angle);
    static float Cos(uint16_t angle);
    static float Sin(float radians);
    uint16_t FastATan(float x, float y) const;
    uint16_t ASin(float t) const;
    uint16_t ATan(float x, float y) const;

    static void Normalize(float* o, const float* v);
    static void Cross(float* o, const float* a, const float* b);
    static void CrossX87(float* o, const float* a, const float* b);
    static void MulVector3(float* o, const float* m, const float* v);
    static void MulMatrix(float* outM, const float* a, const float* b);
    static float TableGetValue(int numEntries, float minArg, float indexMultiplier, const float* data, float arg);

private:
    uint8_t fastATan_[257] = {};
    uint16_t asinAngle_[208] = {};
    float asinSlope_[208] = {};
};

namespace BWareMath2 {

extern const float K_One;
extern const float K_Half;
extern const float K_GraphEps;

void CheckConstants(const Speed2Image& exe);

int Fcom(double a, double b);
inline int Fcom(F24 a, F24 b) { return Fcom(a.V, b.V); }
inline int Fcom(float a, float b) { return Fcom(static_cast<double>(a), static_cast<double>(b)); }

// "test ah,41h ; jnz" taken: a <= b or unordered.
inline bool LeOrNaN(F24 a, F24 b) { return (Fcom(a, b) & 0x41) != 0; }
// "test ah,1 ; jnz" taken: a < b or unordered.
inline bool LtOrNaN(F24 a, F24 b) { return (Fcom(a, b) & 0x01) != 0; }
// "test ah,5 ; jp" taken: a >= b or unordered.
inline bool GeOrNaN(F24 a, F24 b) { return X87::ParityEven(Fcom(a, b) & 0x05); }
// "test ah,44h ; jp" taken: a != b or unordered.
inline bool NeOrNaN(F24 a, F24 b) { return X87::ParityEven(Fcom(a, b) & 0x44); }

float Clamp(float x, float lo, float hi);
float Min(float a, float b);
float Lerp576740(float a, float b, float t);
bool GraphGetValueTo(const float* pts, int count, float x, float& result);
void SinCos(uint16_t angle, float& sin, float& cos);
void ScaleAdd(float* o, const float* a, const float* b, float s);
void RotateVector(float* o, const float* m, const float* v);
void NormalizeX87(float* o, const float* v);
void NormalizeLenX87(float* o, const float* v, float len);
void MulVector3X87(float* o, const float* m, const float* v);
void MulMatrixX87(float* outM, const float* a, const float* b);
void BMulMatrix(float* o, const float* a, const float* b);
void TransposeMatrix(float* o, const float* m);
void CopyMatrix(float* dst, const float* src);
void InvertMatrix(float* o, const float* m);
void OrthonormalizeMatrix(float* o, const float* m);

} // namespace BWareMath2

} // namespace bb
