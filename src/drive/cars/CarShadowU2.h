#pragma once

// Port of NFSU2Unity Core/Cars/CarShadowU2.cs: the car shadow of CarRenderInfo::Render 0x620AE0, block
// 0x621391..0x622EDE (docs/re/car-lights-shadow.md): the "fancy" projected hull shadow (g_CarShadowEnable >= 2,
// texture SHADOWRAMP) and the 4x4 grid quad textured with <BASE>_SHADOWIG (g_CarShadowEnable != 0). Both are
// drawn at the default level 2, grid first (RenderingOrder 2) then the hull (RenderingOrder 3), alpha blended,
// z write off, as triangle strips. The camera-dependent depth bias (vertices pulled 0.104 m toward the camera,
// sub_48AE30) is not applied here: see DepthBiasScale.

#include <cstdint>
#include <functional>
#include <vector>

namespace bb {

class BWareMath;

// TopologyCoordinate::GetElevation as the shadow uses it: ground height under (x, y) (z selects the level),
// the ground normal (may be null) and whether a surface was found.
using ShadowElevation = std::function<bool(float x, float y, float z, float& elevation, float* normal)>;

// A shadow vertex: world position, uv and colour (D3DCOLOR 0xAARRGGBB as the game passes it).
struct ShadowVertexU2 {
    float X = 0, Y = 0, Z = 0, U = 0, V = 0;
    uint32_t Colour = 0;
};

// One triangle strip queued with a texture (0 = %s_SHADOWIG, 1 = SHADOWRAMP).
struct ShadowStripU2 {
    int Texture = 0;
    std::vector<ShadowVertexU2> Vertices;
};

class CarShadowU2 {
public:
    // .rdata constants (bits checked against the exe in docs/re/car-lights-shadow.md)
    static const float BiasNear;   // flt_7A66C4 0.104
    static const float BiasAlt;    // flt_7A66C8 0.023

    // ------------------------------------------------------------------ inputs
    // g_CarShadowEnable (0x870CC0): 0 off, 1 grid only, 2 grid + hull (default).
    int ShadowLevel = 2;
    bool FrontEnd = false;
    // Car LOD of this frame (hull: carLod <= 2; LOD 2 fades the hull with the pixel size).
    int CarLod = 0;
    int PixelSize = 100;
    // View id (the hull is skipped in view 3, sub_60D4D0).
    int ViewId = 1;
    // CRI+0x10 / +0x20: parts bounding box inflated by 0.1 (CarFxSetupU2::PartsBoundingBox).
    float BBoxMin[3] = {}, BBoxMax[3] = {};
    // CarPhysicsInfo wheel positions (X, Y, RideHeight) in car space, FL FR RR RL.
    float WheelPos[4][3] = {};
    // Collision hull points (Car+0x3C shape, inferred to be the <CAR>_CV hull) in car space.
    std::vector<std::vector<float>> HullPoints;

    // ------------------------------------------------------------------ outputs (last Build)
    float AirFactor = 0;          // var_E00
    uint32_t GridColour = 0, HullColour = 0;
    std::vector<ShadowStripU2> Strips;
    float LightDir[3] = {};
    int HullPointCount = 0;

    // sub_48AE30 as the shadow uses it: every vertex p becomes cam + s (p - cam), with
    // s = (d - 0.104) / d and d = |cam - carPos| (0.023 for a player camera state not identified).
    static float DepthBiasScale(const float* camPos, const float* carPos, bool altBias = false);

    // sub_60D350: vector from the fixed light at world (0, 0, 2300) (flt_7A83E8) to the car; when its elevation
    // (bATan(horizontal length, -Lz)) is under 0x1A90 (37.35 deg) it is rebuilt at that elevation with the same
    // length, scaling x and y by len cos / (|x| + |y|). Also returns 1 / Lz (0 when Lz == 0; dword_89EBBC).
    static float LightVector(const float* pos, float* l, const BWareMath* math);

    // sub_60D1B0: every hull corner P[i] becomes three points, P - k(P - P[i-1]), P + k'(k(P[i+1] - P) - k(P - P[i-1]))
    // and P + k(P[i+1] - P), with k = k' = 0.333 (flt_7A6720, dword_7A6724). Points are (x, y, z).
    static std::vector<std::vector<float>> RoundCorners(const std::vector<std::vector<float>>& P);

    // Builds the strips for one frame. carMatrix is E (car orientation rows, row 3 = render position
    // Car+0x50 = body origin). ground may be empty (front end or no car: flat z = 0.152).
    void Build(const float* carMatrix, const ShadowElevation& ground, const BWareMath* math = nullptr);

private:
    bool NearPlane(const float* p, const float* gpos, const float* gn) const;
    void BuildGrid(const float* E, const float* pos, const float* gpos, const float* gn, const ShadowElevation& ground,
                   bool inGame);
    void BuildHull(const float* E, const float* pos, const float* gpos, const float* gn, const ShadowElevation& ground,
                   const BWareMath* math);
};

} // namespace bb
