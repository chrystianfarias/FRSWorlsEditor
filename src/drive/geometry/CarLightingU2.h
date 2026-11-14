#pragma once

// Port of NFSU2Unity Core/Geometry/CarLightingU2.cs: the shaper light rig (eShaperLightRig,
// chunk 0x3B700) and the car's lighting polynomial (elSetupLights 0x48ED60, sub_489AF0,
// sub_4895E0). Coefficients are float4 (r, g, b, luminance) in GAME world space (X forward,
// Y left, Z up); evaluate them with game-space normals (the yz quirk makes the projection
// frame-dependent). See docs/re/car-shading.md section 4.
//
// Editor additions (not in the C#, which evaluates in world space): the per-part steps the game
// does in elCloneLightContext (0x48F660) for the shader path, because the editor feeds the game's
// own vs_1_1 bytecode, which evaluates in part-local space:
//   RotateToLocal  = sub_489BA0(SH, transpose3x3(l2w))   (car-shading.md 4.5)
//   LocalEyePos    = sub_48A6A0                          (car-shading.md 4.6)
// and AnglesToDir follows sub_48A310 exactly (16-bit bAngle + bSinCos 0x43C390) instead of the C#'s
// exact sin/cos (a difference of ~1e-4).

#include "core/Math.h"

#include <cstdint>
#include <vector>

namespace bb {

class ShaperLightRigU2 {
public:
    static constexpr uint32_t ChunkId = 0x0003B700;

    struct Light {
        bool CameraRelative = false; // +0x00 flag
        float Azimuth = 0;           // degrees
        float Elevation = 0;         // degrees
        float R = 0, G = 0, B = 0;
        float Intensity = 0;
    };

    uint32_t NameHash = 0;
    Light Lights[3];

    // ShaperLightsCarsInGame (0x7F3F00), used by RenderCars in game.
    static const ShaperLightRigU2& CarsInGame();
    // ShaperLightsDefault (0x7F3E10), fallback when a named rig is not loaded.
    static const ShaperLightRigU2& Default();

    // All rigs in a file (FRONTEND\FrontB.lzc holds the FE ones: SHOWROOM, CARLOT01...).
    static std::vector<ShaperLightRigU2> ReadAll(const std::vector<uint8_t>& buf);
};

class CarLightingU2 {
public:
    static constexpr int NumCoeffs = 10;
    static constexpr float EyeMinDistance = 2.78f;  // flt_78D788 / flt_78D790
    static constexpr float EyeMaxFrontEnd = 8.0f;   // dword_78D794 (in game the max is 2.78 too)

    // sub_48A310 as elSetupLights calls it, (az + 180, 90 - el, 1):
    // (-cos el cos az, -cos el sin az, sin el) through 16-bit bAngles and bSinCos.
    static ::Vec3 AnglesToDir(float azDeg, float elDeg);

    // sub_4895E0: adds one directional light (unit dir towards the light, colour s) to c.
    static void Project(float c[NumCoeffs][4], const ::Vec3& l, const float s[4]);

    // sub_489AF0: the light plus a -5% bounce light from the opposite side.
    static void AddLight(float c[NumCoeffs][4], const ::Vec3& dir, const float col[4]);

    // Coefficients for a car lit by a rig (no world lights). Camera axes are in game space:
    // right, down (inferred: the camera matrix's second row points down) and forward.
    static void Compute(const ShaperLightRigU2& rig, const ::Vec3& camRight, const ::Vec3& camDown,
                        const ::Vec3& camForward, float c[NumCoeffs][4]);

    // sub_489BA0(SH, transpose3x3(l2w)): world coefficients -> the part's local space, so that
    // local(n) == world(n * l2w) (row vectors) for any local normal n. Exact polynomial
    // substitution; l2w's scale and mirroring are part of it, as in the game.
    static void RotateToLocal(const float world[NumCoeffs][4], const Mat4& l2w, float local[NumCoeffs][4]);

    // sub_48A6A0 (LOCALEYEPOS): the camera in part space through the transposed rotation of
    // l2w, its distance clamped to [EyeMinDistance, frontEnd ? EyeMaxFrontEnd : EyeMinDistance].
    // w = 1.
    static void LocalEyePos(const Mat4& l2w, const ::Vec3& eyeWorld, bool frontEnd, float out[4]);

    // The polynomial at a normal (the VS's c0..c9 basis), for checks.
    static void Evaluate(const float c[NumCoeffs][4], const ::Vec3& n, float out[4]);
};

} // namespace bb
