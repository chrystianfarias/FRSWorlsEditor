#pragma once

// Shared GPU pieces of the game's light and shadow effects in the editor (car light flares, car shadow,
// headlight projector, world light flares): the game's own WORLDNODEPTH / WORLDHEADLIGHT shaders taken from
// SPEED2.EXE, the GLOBALB textures they sample, the eView data eRenderLightFlare reads built from the
// device's camera, and a small render-state guard. See NFSU2Unity docs/re/car-lights-shadow.md.

#include "core/Math.h"
#include "drive/cars/CarLightFlaresU2.h"

#include <d3d9.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bb {

class Texture;
class TexturePack;

namespace fx {

// Pass 0 of technique 0 of one of SPEED2.EXE's binary effects (RT_RCDATA resource `resource`): the vs_1_1
// whose constant table names `vsMarker` and the ps_1_1 whose table names `psMarker` (nullptr: the first
// ps_1_1). Found by version token and the byte count the effect stores in front of each shader.
bool ExtractEffectPass(const std::wstring& exePath, const wchar_t* resource, const char* vsMarker, const char* psMarker,
                       std::vector<uint32_t>& vs, std::vector<uint32_t>& ps, std::string& error);

// The game texture as a managed D3D texture (DXT / ARGB uploaded as they are, P8 expanded), or nullptr.
IDirect3DTexture9* CreateGameTexture(IDirect3DDevice9* device, const Texture& t);
// 0xAARRGGBB texels; mips built by box filter when `mips`.
IDirect3DTexture9* CreateArgbTexture(IDirect3DDevice9* device, const uint32_t* texels, int w, int h, bool mips);

// A texture with the blend state BlendState (sub_5BCC30) gives it: AlphaBlendType 0 opaque, 2 additive
// (SRCALPHA, ONE), 5 / 6 the dest-alpha forms, others SRCALPHA / INVSRCALPHA; PUNCHTHRU alpha-tests at 0x5F.
struct FxTexture {
    IDirect3DTexture9* texture = nullptr;   // owned by whoever made it
    bool blended = true;
    DWORD src = D3DBLEND_SRCALPHA, dst = D3DBLEND_INVSRCALPHA;
    bool punchThrough = false;
    int renderingOrder = 0;
    std::string name;
};
FxTexture DescribeTexture(const Texture& t);

// GLOBAL\GLOBALB.BUN's texture packs (flare, glow, shadow ramp, headlight textures), read once.
class GlobalTextures {
public:
    GlobalTextures();
    ~GlobalTextures();
    bool Load(const std::wstring& gameFolder);
    bool Loaded() const { return !packs_.empty(); }
    const Texture* Find(uint32_t hash) const;
    // The D3D texture of `hash` (made on first use, kept until Release), or an empty FxTexture.
    const FxTexture& Get(IDirect3DDevice9* device, uint32_t hash);
    void Release();   // drops the D3D textures (the packs stay)

private:
    std::wstring folder_;
    std::vector<std::unique_ptr<TexturePack>> packs_;
    std::unordered_map<uint32_t, FxTexture> made_;
};

// Remembers every state it changes and puts it back when it goes (render states, sampler and texture stage
// states, textures, shaders, declaration / FVF, the world transform). No state block: those would have to
// be released around a device reset.
class StateGuard {
public:
    explicit StateGuard(IDirect3DDevice9* device);
    ~StateGuard();
    StateGuard(const StateGuard&) = delete;
    StateGuard& operator=(const StateGuard&) = delete;

    void RS(D3DRENDERSTATETYPE state, DWORD value);
    void SS(DWORD sampler, D3DSAMPLERSTATETYPE state, DWORD value);
    void TSS(DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value);
    void Texture(DWORD stage, IDirect3DBaseTexture9* texture);
    void Shaders(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps);
    void Declaration(IDirect3DVertexDeclaration9* decl);
    void Fvf(DWORD fvf);

private:
    IDirect3DDevice9* d_;
    std::vector<std::pair<D3DRENDERSTATETYPE, DWORD>> rs_;
    std::vector<std::pair<std::pair<DWORD, D3DSAMPLERSTATETYPE>, DWORD>> ss_;
    std::vector<std::pair<std::pair<DWORD, D3DTEXTURESTAGESTATETYPE>, DWORD>> tss_;
    std::vector<std::pair<DWORD, IDirect3DBaseTexture9*>> tex_;
    bool shaders_ = false, decl_ = false;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DPixelShader9* ps_ = nullptr;
    IDirect3DVertexDeclaration9* declaration_ = nullptr;
    DWORD fvf_ = 0;
};

// The camera the world was drawn with (the device's VIEW and PROJECTION transforms, the editor's
// right-handed view) as the game's eView data: the eCamera matrix (columns right, down, forward; row 3 =
// -eye . axis), position and direction, the projection of sub_5C7EC0 (left-handed, y negated,
// Q = far / (far - near)) times that matrix, its six inward planes, the viewport size and the focal
// eView+0x0C = 256 / tan(fov / 2) of the FOV word, i.e. the horizontal FOV without the 1.1 widening the game
// applies to the player views 1, 2, 4 and 5 (the same derivation as the Unity port's CarFxCommon.ViewOf).
struct DeviceCamera {
    Mat4 view, projection, viewProjection;   // the editor's, as set on the device
    Vec3 eye, right, down, forward;
    int width = 1, height = 1;
};
DeviceCamera ReadDeviceCamera(IDirect3DDevice9* device);
FlareViewU2 MakeFlareView(const DeviceCamera& cam, int viewId = 1);

// Draws ePolys the way eViewPlatInterface::Render (0x5C7E80 -> sub_5C5580) does in the flare and shadow
// passes: effect 13 WORLDNODEPTH (worldnodepth.txt: vs oPos = v0 . WorldViewProj, oT0 = uv + TextureOffset,
// oD0 = colour; ps rgb = 2 tex vcol, a = tex.a vcol.a; FOGENABLE 0, COLORWRITE RGB, ZFUNC LESSEQUAL, CULL NONE,
// WRAP, mip LINEAR), blend from the texture, z write off (both passes turn it off). The vertices are in world
// space (the identity l2w). Falls back to the same combiner in fixed function when the exe's shaders are
// missing.
class PolyRenderer {
public:
    struct Vertex {
        float x, y, z;
        D3DCOLOR colour;   // 0xAARRGGBB
        float u, v;
    };

    PolyRenderer() = default;
    ~PolyRenderer() { Release(); }
    PolyRenderer(const PolyRenderer&) = delete;
    PolyRenderer& operator=(const PolyRenderer&) = delete;

    bool Create(IDirect3DDevice9* device, const std::wstring& exePath);
    void Release();
    bool Ready() const { return device_ != nullptr; }
    const std::string& Status() const { return status_; }

    // Sets the pass up (states recorded in `guard`) for the camera `cam`.
    void Begin(StateGuard& guard, const DeviceCamera& cam);
    // One poly / strip with its texture's blend state (a null texture draws nothing).
    void Draw(StateGuard& guard, const FxTexture& texture, D3DPRIMITIVETYPE type, const Vertex* v, int count);

private:
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DPixelShader9* ps_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    std::string status_;
};

// ePoly colour bytes (R, G, B, A as 0xAABBGGRR) as a D3DCOLOR.
inline D3DCOLOR PolyColour(uint32_t aabbggrr) {
    return (aabbggrr & 0xFF00FF00u) | ((aabbggrr & 0xFFu) << 16) | ((aabbggrr >> 16) & 0xFFu);
}

// Matrix as 16 floats, row-major (the drive code's bMatrix4).
inline void ToFloats(const Mat4& m, float* o) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) o[r * 4 + c] = m.m[r][c];
}

} // namespace fx
} // namespace bb
