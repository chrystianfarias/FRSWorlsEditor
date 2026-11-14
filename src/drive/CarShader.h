#pragma once

// The GPU side of NFSU2's CAR effect (IDI_CAR_FX, technique "car", pass 0) for the editor's test
// drive: the game's own vs_1_1 / ps_1_1 bytecode taken from SPEED2.EXE, and the in-race dynamic
// environment cube (gDynEnvCube). See NFSU2Unity docs/re/car-shader-directx.md and car-shading.md.

#include "core/Math.h"

#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

class WorldRenderer;

namespace bb {

// Pass 0 of technique "car" of the binary fx_2_0 resource IDI_CAR_FX (RT_RCDATA) in SPEED2.EXE:
// the vertex shader (vs_1_1, 1792 bytes: HarmonicCoeff c0..c9, WorldViewProj c10..c13,
// WorldView c14..c16, LocalEyePos c17, DiffuseMin/Range c18/c19, SpecularMin/Range c20/c21,
// SpecularPower c22.x, EnvmapPower c23.x) and the pixel shader (ps_1_1, 420 bytes: EnvmapMin c0,
// EnvmapRange c1, envmap cube s0, diffuse s1). Found by their version token and constant table,
// no D3DX needed. False with a reason when the exe has no such resource.
bool ExtractCarEffectShaders(const std::wstring& exePath, std::vector<uint32_t>& vertexShader,
                             std::vector<uint32_t>& pixelShader, std::string& error);

// The race's dynamic car environment map (sub_5BA4B0, eDisplayFrame 0x5CACCC): a 128x128
// A8R8G8B8 render-target cube, rendered from the player car's position + (0, 0, 0.95) with 90
// degree faces, near 0.5 and far 64, sky first then the scenery, no cars.
//
// One difference from the game, the same one the Unity port makes: the game aligns the cube with
// the player camera and samples it with the view-space reflection; this cube is aligned with the
// world (face +X = game +X ...) and sampled with the world-space reflection (CarModel feeds the
// part's l2w instead of l2w * View to the VS's c14..c16). With every face fresh the picture is the
// same; with one face per frame a stale face lags in world rather than camera axes.
class CarEnvironmentMap {
public:
    static constexpr int Size = 128;
    static constexpr float Near = 0.5f, Far = 64.0f, EyeHeight = 0.95f;

    CarEnvironmentMap() = default;
    ~CarEnvironmentMap() { Release(); }
    CarEnvironmentMap(const CarEnvironmentMap&) = delete;
    CarEnvironmentMap& operator=(const CarEnvironmentMap&) = delete;

    // g_CarEnvironmentMapEnable (0x870CB8): 0 off (black cube), 1 one face per frame (round
    // robin), 2 the face ahead of the camera every frame plus one of the others, 3 all six.
    // A new cube, a device reset or a jump of more than 20 m redraws all six once.
    void SetMode(int mode) { mode_ = mode < 0 ? 0 : (mode > 3 ? 3 : mode); }
    int Mode() const { return mode_; }

    // Brings the faces due this frame up to date around `carPosition` (game space; the eye is
    // EyeHeight above it) and returns the cube, or nullptr when the device cannot make one. Draws
    // through renderer.RenderWorldOnly into its own targets and puts the caller's render target,
    // depth buffer and viewport back; other device states are the caller's to restore.
    IDirect3DCubeTexture9* Update(IDirect3DDevice9* device, WorldRenderer& renderer, const Vec3& carPosition,
                                  const Vec3& cameraForward);

    // Drops the render targets (D3DPOOL_DEFAULT: the owner must call this before the device is
    // reset; the next Update makes them again and redraws all six faces).
    void Release();

private:
    bool Create(IDirect3DDevice9* device);

    IDirect3DCubeTexture9* cube_ = nullptr;
    IDirect3DSurface9* depth_ = nullptr;
    bool createFailed_ = false;
    bool filled_ = false;
    int mode_ = 1;
    unsigned frame_ = 0;
    Vec3 lastEye_;
};

} // namespace bb
