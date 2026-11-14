#pragma once

// Port of NFSU2Unity Core/Camera/CameraMath.cs.
// Matrix/vector helpers used by the SPEED2.EXE camera code (docs/re/camera.md §6). bMatrix4 = 16 floats,
// row-major, row vectors (v' = v * M, translation in row 3). x87 stack values are F24 (PC=24), every
// "fstp dword" is an explicit Store(); the D3DX multiply is the SSE implementation the game selects on
// every SSE-capable CPU, modelled with plain float arithmetic.

#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"

#include <cstdint>

namespace bb {

namespace CameraMath {

inline const float One = X87::Bits(0x3F800000u);

// bIdentity(bMatrix4*) 0x43C780.
void Identity(float* m);

// D3DXMatrixMultiply through the PSGP table (0x63CEDB -> [0x803E74] -> sub_68CE7C, the SSE/SSE2 entry):
// out = a * b. Rows 0 and 3 sum as (p0+p1)+(p2+p3), rows 1 and 2 as ((p0+p1)+p2)+p3 (mulps/addps order).
void MatMulSse(float* o, const float* a, const float* b);

// eRotateX(dst, src, angle) 0x5BACF0: dst = src * [[1,0,0],[0,c,s],[0,-s,c]].
void RotateX(float* dst, const float* src, uint16_t angle);

// eRotateY(dst, src, angle) 0x5BADA0: dst = src * [[c,0,-s],[0,1,0],[s,0,c]].
void RotateY(float* dst, const float* src, uint16_t angle);

// eRotateZ(dst, src, angle) 0x5BAE60 with eCreateRotationZ 0x5BAC90: [[c,s,0],[-s,c,0],[0,0,1]].
void RotateZ(float* dst, const float* src, uint16_t angle);

// eInvertTransformationMatrix(out, m) 0x5BAF90 (rigid transforms only): out = T(-m.row3) * transpose3x3(m),
// multiplied by the SSE D3DX multiply.
void InvertTransformation(float* o, const float* m);

// eMulVector(bVector3*, const bMatrix4*, const bVector3*) 0x441000 (= bMulMatrix 0x43C9C0): v*M + row3.
inline void MulVec3(float* o, const float* m, const float* v) { BWareMath2::MulVector3X87(o, m, v); }

// eMulVector(bVector4*, const bMatrix4*, const bVector4*) 0x486F50 (= bMulMatrix 0x43C9B0).
void MulVec4(float* o, const float* m, const float* v);

// bDistBetween(a, b) 0x43CE10 (st0).
F24 DistBetween(const float* a, const float* b);

// sub_443F70: per-component clamp v = min(max(v, lo), hi) with bClamp's NaN behaviour.
void ClampVec(float* v, const float* lo, const float* hi);

// bScaleAdd(bVector4*) sub_43D100: o = a + s*b (4 components).
void ScaleAdd4(float* o, const float* a, const float* b, float s);

// eCreateLookAtMatrix(out, eye, look, up) 0x5BA9A0: f = |look-eye|, r = |(-up) x f|, d = |f x r|;
// columns (r, d, f) = camera x right, y down, z forward; row 3 = (-eye.r, -eye.d, -eye.f, 1).
// The rotation copy goes through additions with products by 0 (kept: they only affect signed zeros).
void LookAt(float* o, const float* eye, const float* look, const float* up);

} // namespace CameraMath

} // namespace bb
