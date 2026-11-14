#pragma once

// The pieces of Need for Speed Underground 2's own world rendering that the
// viewport's "Game look" view reuses: the WORLD / WORLDREFLECT / WORLDNOFOG
// effects' vs_1_1 / ps_1_1 bytecode taken from SPEED2.EXE (no D3DX needed),
// and the per-track fog of the region's parameter maps with the constants
// eDisplayFrame derives from it. See NFSU2Unity docs/re/world.md and
// city-drive.md for the reverse engineering this follows.

#include "core/Math.h"
#include "core/Types.h"

#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gamelook {

// One effect's shaders: pass 0 of technique 0 ("world").
struct EffectCode {
    std::vector<uint32_t> vertexShader;
    std::vector<uint32_t> pixelShader;
};

// The three effects the world is drawn with (gEffects[] ids 0, 2 and 7):
//  WORLD        IDI_WORLD_FX:        vs c0..c3 WorldViewProj, c4..c6 WorldView, c7 TextureOffset, c8 FogValue;
//                                    ps out.rgb = 2 * t0 * v0, out.a = t0.a * v0.a
//  WORLDREFLECT IDI_WORLDREFLECT_FX: vs c0..c3, c4..c6, c7 FogValue (no TextureOffset), oT1 = the screen
//                                    position as a projective coordinate; ps out.rgb = 2 * t0 * v0 + t1 *
//                                    (t0.a * v0.a), out.a = 1, s1 = ReflectedTex
//  WORLDNOFOG   IDI_WORLDNOFOG_FX:   vs c0..c3, c4 TextureOffset; ps as WORLD
// Each shader is found by its version token, the byte count the effect stores in front of it and its
// constant table. False with a reason when the exe lacks one of them.
// filter: IDI_FILTER_FX pass p3 (4 taps: vs copies the clip position and four texture coordinates, ps
// out = t0*c0 + t1*c1 + t2*c2 + t3*c3), which sub_5C7E40 blurs the reflection with; empty when absent.
struct WorldEffects {
    EffectCode world, reflect, noFog, filter;
};
bool ExtractWorldEffects(const std::wstring& exePath, WorldEffects& out, std::string& error);

// The instructions of a vs_1_1 / ps_1_1 token stream as text ("dp4 oPos.x, v0, c0"), one per entry,
// to check an extraction against the published disassembly.
std::vector<std::string> Disassemble(const std::vector<uint32_t>& tokens);

// bStringHash: h = 0xFFFFFFFF, h = h * 33 + c.
u32 GameHash(const char* text);

// The fog struct sub_614300 (0x614300) fills, the part eDisplayFrame uses.
struct FogState {
    float start  = 212.0f;   // +0x0C
    float factor = 0.74f;    // +0x10
    float r = 110.0f, g = 105.0f, b = 75.0f;   // +0x14 (bytes)
};

// The constants eDisplayFrame (0x5CB032..0x5CB189) sets from it for a player view:
//   FOGVALUE = (250, 1 / (250 - start), min(1 - factor, 0.6))   flt_7A27A0 = 250, dword_784564 = 0.6
//   FOGCOLOR = each colour byte * 0.75 (flt_785510), truncated
// and the WORLD vertex shader turns them into oFog = clamp((FogValue.x - viewZ) * FogValue.y, FogValue.z, 1).
struct FogConstants {
    float value[4] = { 250.0f, 1.0f / 38.0f, 0.26f, 0.0f };
    D3DCOLOR colour = 0xFF524F38;
};
FogConstants MakeFogConstants(const FogState& fog);

// A region's fog parameter maps: chunk 0x8003B600 of TRACKS\<region>.BUN, one 0x8003B601 layer per
// named map (header 0x3B602, field types 0x3B603, field offsets 0x3B604, parameter sets 0x3B605, an
// 8-bit or 16-bit quad tree 0x3B607 / 0x3B608). LoaderParameterMaps 0x60B810, ParameterMapLayer::
// GetParameterData 0x5FA4E0, GetParameterSetIndexFromQuadData8/16 0x5F0930 / 0x5F0A30.
class FogMap {
public:
    // Reads the "Normal Fog" layer: in free roam without the time-of-day option (dword_870D14 != 1)
    // sub_614300 blends it with weight 1 - flt_8A1C88 = 1, the night layer with 0, and skips the rain
    // layer. False (and the exe's fallback struct 0x800E18 in Sample) when the file has no such layer.
    bool Load(const std::wstring& bunPath);
    bool Loaded() const { return !sets_.empty(); }

    // The game's value at (x, y) game space, eased the way ParameterAccessorBlendByDistance does it
    // (sub_5FA660 with radius 50: blend = min(distance moved since the last sample / 50, 1)).
    FogState Sample(float x, float y);
    void ResetSmoothing() { haveLast_ = false; }

    // The set index the quad tree gives at (x, y), for the log.
    int SetIndexAt(float x, float y) const;

private:
    float minX_ = 0, minY_ = 0, maxX_ = 0, maxY_ = 0;
    std::vector<std::vector<float>> sets_;   // 8 fields each
    std::vector<u16> tree_;                  // 4 children per node (8-bit trees widened)
    bool haveLast_ = false;
    float lastX_ = 0, lastY_ = 0;
    std::vector<float> smoothed_;
};

} // namespace gamelook
