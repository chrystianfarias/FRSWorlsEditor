// Port of NFSU2Unity Core/Physics/Collision/GjkSimplexSolver.cs.
#include "drive/physics/collision/GjkSimplexSolver.h"

#include "drive/numerics/BWareMath.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace bb {

void GjkSimplex::CopyFrom(const GjkSimplex& s)
{
    for (int k = 0; k < 8; k++) {
        W[4 * k] = s.W[4 * k];
        W[4 * k + 1] = s.W[4 * k + 1];
        W[4 * k + 2] = s.W[4 * k + 2];
    }
    for (int k = 0x80 / 4; k < Words; k++)
        if (k != 0x8C / 4 && k != 0x9C / 4 && k != 0xAC / 4) W[k] = s.W[k];
}

F24 GjkSimplexSolver::Solve(GjkSimplex& s, float* outDir)
{
    thread_local uint32_t mem[MemWords];
    uint32_t* m = mem;
    std::memset(m, 0, sizeof(mem));
    std::memcpy(m + Base / 4, s.W, sizeof(s.W));
    for (int i = 0; i < 3; i++) m[Out / 4 + i] = X87::Bits(outDir[i]);
    // stack: [esp] = return address, [esp+4] = out (the caller's push)
    int esp = StackTop - 8;
    m[(esp + 4) / 4] = Out;
    m[esp / 4] = 0xDEADBEEFu;
    F24 r = Lifted(m, Base, esp);
    std::memcpy(s.W, m + Base / 4, sizeof(s.W));
    for (int i = 0; i < 3; i++) outDir[i] = X87::Bits(m[Out / 4 + i]);
    return r;
}

void GjkSimplexSolver::Check(int a)
{
    if ((a & 3) != 0 || a < 0 || (a >> 2) >= MemWords) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "lifted code: bad address 0x%X", static_cast<unsigned>(a));
        throw std::runtime_error(buf);
    }
}

int GjkSimplexSolver::Cross(uint32_t* m, int o, int a, int b, int& ecx)
{
    float va[3], vb[3], r[3];
    Vec(m, a, va);
    Vec(m, b, vb);
    BWareMath::CrossX87(r, va, vb);
    Put(m, o, r);
    ecx = b;
    return o;
}

int GjkSimplexSolver::Normalize(uint32_t* m, int o, int v, int& ecx)
{
    float src[3], r[3];
    Vec(m, v, src);
    BWareMath2::NormalizeX87(r, src);
    Put(m, o, r);
    F24 len = F24::Sqrt(F24(src[0]) * src[0] + F24(src[1]) * src[1] + F24(src[2]) * src[2]);
    ecx = BWareMath2::NeOrNaN(len, F24(0.0f)) ? v : 0;
    return o;
}

int GjkSimplexSolver::ScaleAdd(uint32_t* m, int o, int a, int b, int sBits, int& ecx)
{
    float va[3], vb[3], r[3];
    Vec(m, a, va);
    Vec(m, b, vb);
    BWareMath2::ScaleAdd(r, va, vb, X87::Bits(static_cast<uint32_t>(sBits)));
    Put(m, o, r);
    ecx = a;
    return o;
}

} // namespace bb
