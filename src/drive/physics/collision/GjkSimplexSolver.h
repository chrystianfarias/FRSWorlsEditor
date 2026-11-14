#pragma once

// Port of NFSU2Unity Core/Physics/Collision/GjkSimplexSolver.cs (+ GjkSimplexSolver.Lifted.cs, the machine
// converted literal lift in GjkSimplexSolver.Lifted.cpp).

#include "drive/numerics/X87.h"

#include <cstdint>

namespace bb {

/// The GJK simplex block of a CollisionBodyWitness (witness+0x40, 0xDC bytes) and the solver that reduces it,
/// sub_595040 (thiscall on the block, one argument: bVector3* out; returns the squared distance in st0).
/// Layout (byte offsets in the block): +0x00 the A support points (4 x 16 bytes), +0x40 the B support points,
/// +0x80 closest point on A, +0x90 closest point on B, +0xA0 separating direction, +0xB0 A vertex ids (int[4]),
/// +0xC0 A count, +0xC4 B vertex ids, +0xD4 B count, +0xD8 the witness's "initialised" flag (witness+0x118).
/// The solver itself is a literal lift of the original (GjkSimplexSolver.Lifted.cpp).
class GjkSimplex {
public:
    static constexpr int Size = 0xDC, Words = Size / 4;
    /// The block as stored by the game (float and int fields share it).
    uint32_t W[Words] = {};

    float F(int offset) const { return X87::Bits(W[offset >> 2]); }
    void SetF(int offset, float v) { W[offset >> 2] = X87::Bits(v); }
    int I(int offset) const { return static_cast<int>(W[offset >> 2]); }
    void SetI(int offset, int v) { W[offset >> 2] = static_cast<uint32_t>(v); }

    int CountA() const { return I(0xC0); }
    void SetCountA(int v) { SetI(0xC0, v); }
    int CountB() const { return I(0xD4); }
    void SetCountB(int v) { SetI(0xD4, v); }
    int IdA(int i) const { return I(0xB0 + 4 * i); }
    int IdB(int i) const { return I(0xC4 + 4 * i); }
    bool Initialised() const { return I(0xD8) != 0; }
    void SetInitialised(bool v) { SetI(0xD8, v ? 1 : 0); }

    void PointA(int i, float* v) const { v[0] = F(16 * i); v[1] = F(16 * i + 4); v[2] = F(16 * i + 8); }
    void SetPointA(int i, float x, float y, float z) { SetF(16 * i, x); SetF(16 * i + 4, y); SetF(16 * i + 8, z); }
    void SetPointB(int i, float x, float y, float z) { SetF(0x40 + 16 * i, x); SetF(0x44 + 16 * i, y); SetF(0x48 + 16 * i, z); }

    /// sub_598130 / sub_598240: copies the block (x, y, z of the 8 support points, the 3 result vectors, both id
    /// lists with their counts and the flag; the w components of the support points are not copied).
    void CopyFrom(const GjkSimplex& s);
};

/// sub_595040: the closest points of the A and B simplices, reduction of both to the supporting features.
class GjkSimplexSolver {
public:
    /// Runs sub_595040 on the block; writes the direction to outDir and returns the squared distance.
    static F24 Solve(GjkSimplex& s, float* outDir);

private:
    static constexpr int Base = 0x100, Out = 0x200, StackTop = 0x1000, MemWords = StackTop / 4 + 4;

    /// The lifted code (GjkSimplexSolver.Lifted.cpp).
    static F24 Lifted(uint32_t* M, int ecx, int esp);

    // ------------------------------------------------------------------ memory of the lifted code
    static float F(uint32_t* m, int a) { Check(a); return X87::Bits(m[a >> 2]); }
    static int I(uint32_t* m, int a) { Check(a); return static_cast<int>(m[a >> 2]); }
    static void W(uint32_t* m, int a, int v) { Check(a); m[a >> 2] = static_cast<uint32_t>(v); }
    static void WF(uint32_t* m, int a, float v) { Check(a); m[a >> 2] = X87::Bits(v); }
    static void Check(int a);

    static void Vec(uint32_t* m, int a, float* o) { o[0] = F(m, a); o[1] = F(m, a + 4); o[2] = F(m, a + 8); }
    static void Put(uint32_t* m, int a, const float* v) { WF(m, a, v[0]); WF(m, a + 4, v[1]); WF(m, a + 8, v[2]); }

    /// bCross 0x43D1E0 (all reads before the writes: out may alias an input). Leaves eax = out, ecx = b.
    static int Cross(uint32_t* m, int o, int a, int b, int& ecx);
    /// bNormalize 0x43CF10. Leaves eax = out, ecx = v (0 for a zero vector).
    static int Normalize(uint32_t* m, int o, int v, int& ecx);
    /// bScaleAdd 0x43D0C0: out = a + s*b (s passed by value). Leaves eax = out, ecx = a.
    static int ScaleAdd(uint32_t* m, int o, int a, int b, int sBits, int& ecx);
};

} // namespace bb
