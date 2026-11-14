#pragma once

// Port of NFSU2Unity Core/Cars/CarHeadlightsU2.cs: headlights on the world (docs/re/car-lights-shadow.md):
// the player-car projector (eView 9 set by sub_5C2AE0, projection sub_5C7EC0, per-draw constants sub_5BDAA0,
// WORLDHEADLIGHT effect), its clip ramp texture (sub_5BC5D0), the headlight colour (CRI+0x380,
// CarRenderInfo::UpdateCarParts 0x635BE8) and the textured splash quad drawn only into the road reflection
// (RenderCarsTexturedHeadlights 0x615490, CarRenderInfo::RenderTextureHeadlights 0x60D5D0).

#include <array>
#include <cstdint>

namespace bb {

class CarHeadlightsU2 {
public:
    static constexpr uint32_t HeadlightsTexture = 0x28EEFA9C;    // "HEADLIGHTS" (GLOBALB, 128x64 ARGB32)
    static constexpr uint32_t SplashTexture = 0x45374719;        // "2PLAYERHEADLIGHT1" (GLOBALB, 128x128 DXT3, additive)
    static constexpr uint32_t DefaultColour = 0x0090AA90;        // no HEADLIGHT_BULB part
    static constexpr int BulbSlot = 159;                         // HEADLIGHT_BULB (ride+0x80C)

    static const float Near;          // flt_7A27AC
    static const float Far;           // dword_7FEE58
    static const uint16_t Fov;        // 2.5 rad -> 26075
    // HEADLIGHTCOLOR = (R, G, B, 0) * (1/128) (flt_787AF0).
    static const float ColourScale;

    // The projector view (sub_5C2AE0 + sub_5C7EC0 for view 9): eye = pos + 0.75 up, looking along
    // normalize(forward - 0.1 z), world up (0, 0, 1). carMatrix rows: 0 forward, 2 up, 3 position.
    // Writes View (world -> camera, x right, y down, z forward) and Proj (row vectors).
    static void ProjectorView(const float* carMatrix, float* view, float* proj);

    // HEADLIGHTWVP without the local-to-world part: World -> ViewProj9 -> bias (x += 0.5 w, y += 0.5 w;
    // dword_80052C/0x800530). The shader divides xy by w: u = x/w + 0.5, v = y/w + 0.5 (no 0.5 scale).
    static std::array<float, 16> ProjectorMatrix(const float* carMatrix);

    // Clip ramp [0x86BD18] built by sub_5BC5D0 (64 x 1 A8R8G8B8, D3DPOOL_MANAGED): texels 33..47 =
    // g * 0x01010101 with g = ftol(sqrt(1 - (i - 33) * flt_7844EC) * 255) (alpha too), the others 0.
    // Sampled at u = 0.5 + 0.005 * view depth (oT2). Returns 0xAARRGGBB texels.
    static std::array<uint32_t, 64> ClipRamp();

    // RenderTextureHeadlights 0x60D5D0: drawn only when M[2][2] >= 0.707 (flt_78A7D4).
    static bool SplashVisible(const float* carMatrix);

    // The splash quad in car space at z = 0 (x 13 +- 10, y +- 4.9; flt_8A1C4C y offset is never written), uv
    // (0,0) (1,0) (1,1) (0,1); colour = CRI+0x380 | 0x80000000 (bytes R, G, B, A). Only drawn into the view-4
    // road reflection with the WORLD effect (rgb = 2 tex vcol), texture 2PLAYERHEADLIGHT1 (additive).
    static std::array<float, 12> SplashQuad();

private:
    static void Mul(const float* a, const float* b, float* o);
};

} // namespace bb
