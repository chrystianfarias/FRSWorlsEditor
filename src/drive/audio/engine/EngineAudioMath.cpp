// Port of NFSU2Unity Core/Audio/Engine/EngineAudioMath.cs.

#include "drive/audio/engine/EngineAudioMath.h"

#include <algorithm>

namespace bb {
namespace EngineAudioMath {

int CurveT(const int* t, int x)
{
    x = ClampI(x, 0, 0x7FFF);
    int i = x >> 6;
    int frac = ((x & 0x1E) << 9) | 0x3FF;
    int a = t[i], b = t[std::min(i + 1, 512)];
    return a + (((b - a) * frac) >> 15);
}

int MixerCurve(const int* t, int type, int x)
{
    switch (type) {
    case 0: return CurveT(t, x);
    case 1: return CurveT(t, 0x7FFF - x);
    case 2: { int c = CurveT(t, x); return c * c >> 15; }
    case 3: { int c = CurveT(t, 0x7FFF - x); return c * c >> 15; }
    case 4: return 0x7FFF - CurveT(t, 0x7FFF - x);
    case 5: return 0x7FFF - CurveT(t, x);
    case 6: { int y = 0x7FFF - CurveT(t, 0x7FFF - x); return y * y >> 15; }
    case 7: { int y = 0x7FFF - CurveT(t, x); return y * y >> 15; }
    case 8: return x;
    default: return 0x7FFF - x;
    }
}

std::vector<int> SyntheticCosTable()
{
    std::vector<int> t(513);
    for (int i = 0; i < 513; i++)
        t[i] = static_cast<int>(std::round(32767 * std::cos(3.14159265358979323846 / 2 * i / 512)));
    return t;
}

} // namespace EngineAudioMath
} // namespace bb
