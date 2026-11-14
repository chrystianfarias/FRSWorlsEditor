#pragma once

// The player car's headlights on the world (NFSU2Unity docs/re/car-lights-shadow.md section 3; port of
// Runtime/Fx/HeadlightProjectorFx.cs): eView 9 from sub_5C2AE0 (eye 0.75 m above the car, looking along
// forward - 0.1 z, FOV 2.5 rad, aspect 0.6, near 1, far 100), HEADLIGHTWVP = l2w . ViewProj9 . bias
// (sub_5BDAA0), the HEADLIGHTS beam texture (GLOBALB), the 64x1 clip ramp (sub_5BC5D0) and HEADLIGHTCOLOR =
// CRI+0x380 / 128.
//
// The game draws the scenery visible in view 9 with WORLDHEADLIGHT instead of WORLD (render flag 0x200, tables
// 0x7FF090 / 0x7FF0B4): rgb = sat(2 t0 (beam ramp colour + vcol)). Here the world is drawn by WorldRenderer and the
// headlight term is added on top in a separate pass over the world's opaque and cut-out surfaces near the car
// (WorldRenderer::VisitSolidDraws): the game's own WORLDHEADLIGHT vs_1_1 (IDI_WORLDHEADLIGHT_FX pass 0) and its
// ps_1_1 with the vertex-colour operand of the `mad` dropped, i.e. out = sat(2 t0 beam ramp colour), blended
// ONE + ONE. With an unsigned 8-bit target sat(a + b) = min(sat(a) + b, 1) for a, b >= 0, so the sum equals the
// game's single pass (up to rounding). Fog is left out of the added term (the lit range ends at about 47 m, well
// inside the fog start), and blended world surfaces get no headlight term.

#include "core/Math.h"
#include "drive/fx/FxCommon.h"

#include <d3d9.h>

#include <array>
#include <cstdint>
#include <string>

class WorldRenderer;

namespace bb {

class HeadlightProjector {
public:
    HeadlightProjector() = default;
    ~HeadlightProjector() { Release(); }
    HeadlightProjector(const HeadlightProjector&) = delete;
    HeadlightProjector& operator=(const HeadlightProjector&) = delete;

    bool Create(IDirect3DDevice9* device, const std::wstring& exePath);
    void Release();
    bool Ready() const { return vs_ && ps_ && decl_ && ramp_; }
    const std::string& Status() const { return status_; }

    // g_CarHeadlightEnable (0x870CC4, default 1 above performance level 1).
    void SetEnabled(bool on) { enabled_ = on; }
    bool Enabled() const { return enabled_; }

    // Adds the headlight term to the world drawn so far, for the body matrix `body` (rows forward, left, up,
    // position Car+0x50) and the headlight colour CRI+0x380 (0x00BBGGRR). Returns the draws issued.
    int Draw(fx::StateGuard& guard, WorldRenderer& renderer, const fx::DeviceCamera& cam, const Mat4& body,
             uint32_t colour, fx::GlobalTextures& globals);

    // The projector matrix (world -> h, row vectors) and the box the lit area fits in, for a body matrix.
    static std::array<float, 16> Matrix(const Mat4& body);
    static Aabb LitBounds(const Mat4& body);

private:
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DPixelShader9* ps_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    IDirect3DTexture9* ramp_ = nullptr;
    bool enabled_ = true;
    std::string status_;
};

} // namespace bb
