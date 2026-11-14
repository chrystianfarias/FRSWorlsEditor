#include "drive/fx/HeadlightProjector.h"

#include "drive/cars/CarHeadlightsU2.h"
#include "nfs/World.h"
#include "render/WorldRenderer.h"

#include <cstring>

namespace bb {

namespace {

// WORLD vertex buffers (kWorldVertexFvf, stride 36): POSITION 0, NORMAL 12, D3DCOLOR 24, TEXCOORD0 28. The
// WORLDHEADLIGHT vs reads v0 position, v1 colour, v2 texcoord.
const D3DVERTEXELEMENT9 kWorldDecl[] = {
    { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
    { 0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END(),
};

int RegisterType(uint32_t param) { return (int)(((param >> 28) & 7) | ((param >> 8) & 0x18)); }

// The WORLDHEADLIGHT pixel shader without the vertex colour: its `mad r0.xyz, r0, c0, v0` (headlight times
// colour plus the baked lighting) becomes `mul r0.xyz, r0, c0`. ps_1_x instructions carry no length, so the
// source token is simply dropped. False when the shader is not the expected one.
bool DropVertexColour(std::vector<uint32_t>& ps) {
    for (size_t i = 1; i + 4 < ps.size(); ++i) {
        const uint32_t op = ps[i];
        if ((op & 0xFFFFu) != 4 /* D3DSIO_MAD */ || (op & 0x80000000u)) continue;
        const uint32_t src2 = ps[i + 4];
        if (!(ps[i + 1] & 0x80000000u) || !(src2 & 0x80000000u)) continue;
        if (RegisterType(src2) != 1 /* D3DSPR_INPUT */ || (src2 & 0x7FF) != 0) continue;
        ps[i] = (op & ~0xFFFFu) | 5u;   // D3DSIO_MUL
        ps.erase(ps.begin() + (std::ptrdiff_t)(i + 4));
        return true;
    }
    return false;
}

void Columns(const Mat4& m, float (*c)[4]) {
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) c[r][k] = m.m[k][r];
}

Mat4 FromFloats(const float* f) {
    Mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.m[r][c] = f[r * 4 + c];
    return m;
}

} // namespace

bool HeadlightProjector::Create(IDirect3DDevice9* device, const std::wstring& exePath) {
    Release();
    device_ = device;
    std::vector<uint32_t> vs, ps;
    std::string why;
    if (!fx::ExtractEffectPass(exePath, L"IDI_WORLDHEADLIGHT_FX", "HeadlightWVP", "HeadlightColor", vs, ps, why)) {
        status_ = "headlight projector off: " + why;
        return false;
    }
    if (!DropVertexColour(ps)) {
        status_ = "headlight projector off: unexpected WORLDHEADLIGHT pixel shader";
        return false;
    }
    if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.data()), &vs_)) ||
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &ps_)) ||
        FAILED(device->CreateVertexDeclaration(kWorldDecl, &decl_))) {
        Release();
        status_ = "headlight projector off: the device refused the WORLDHEADLIGHT shaders";
        return false;
    }
    const std::array<uint32_t, 64> ramp = CarHeadlightsU2::ClipRamp();
    ramp_ = fx::CreateArgbTexture(device, ramp.data(), 64, 1, false);
    if (!ramp_) {
        Release();
        status_ = "headlight projector off: no clip ramp texture";
        return false;
    }
    status_ = "WORLDHEADLIGHT from SPEED2.EXE (additive pass)";
    return true;
}

void HeadlightProjector::Release() {
    if (vs_) vs_->Release();
    if (ps_) ps_->Release();
    if (decl_) decl_->Release();
    if (ramp_) ramp_->Release();
    vs_ = nullptr; ps_ = nullptr; decl_ = nullptr; ramp_ = nullptr;
    device_ = nullptr;
}

std::array<float, 16> HeadlightProjector::Matrix(const Mat4& body) {
    float E[16];
    fx::ToFloats(body, E);
    return CarHeadlightsU2::ProjectorMatrix(E);
}

Aabb HeadlightProjector::LitBounds(const Mat4& body) {
    float E[16], V[16], P[16];
    fx::ToFloats(body, E);
    CarHeadlightsU2::ProjectorView(E, V, P);
    // camera -> world: eye + x r + y d + z f, with r, d, f the columns of V's rotation.
    const Vec3 r{ V[0], V[4], V[8] }, d{ V[1], V[5], V[9] }, f{ V[2], V[6], V[10] };
    const Vec3 eye{ -(V[12] * r.x + V[13] * d.x + V[14] * f.x), -(V[12] * r.y + V[13] * d.y + V[14] * f.y),
                    -(V[12] * r.z + V[13] * d.z + V[14] * f.z) };
    Aabb box;
    box.Add(eye);
    // The clip ramp is lit for view depths of about 3..47 m (texels 33..47 at u = 0.5 + 0.005 z).
    const float zs[2] = { 2.5f, 48.0f };
    for (float z : zs)
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                const float x = sx * z / P[0], y = sy * z / -P[5];
                box.Add(eye + r * x + d * y + f * z);
            }
    return box;
}

int HeadlightProjector::Draw(fx::StateGuard& g, WorldRenderer& renderer, const fx::DeviceCamera& cam, const Mat4& body,
                             uint32_t colour, fx::GlobalTextures& globals) {
    if (!enabled_ || !Ready() || !device_) return 0;
    const fx::FxTexture& beam = globals.Get(device_, CarHeadlightsU2::HeadlightsTexture);
    if (!beam.texture) return 0;
    IDirect3DDevice9* dev = device_;
    const Mat4 projector = FromFloats(Matrix(body).data());

    g.Declaration(decl_);
    g.Shaders(vs_, ps_);
    g.RS(D3DRS_ZENABLE, D3DZB_TRUE);
    g.RS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g.RS(D3DRS_ZWRITEENABLE, FALSE);
    // A hair toward the camera: the world was drawn through other transforms (fixed function or the game look).
    const float bias = -2.0e-6f;
    DWORD biasBits;
    std::memcpy(&biasBits, &bias, 4);
    g.RS(D3DRS_DEPTHBIAS, biasBits);
    g.RS(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    g.RS(D3DRS_FOGENABLE, FALSE);
    g.RS(D3DRS_COLORWRITEENABLE, 7);
    g.RS(D3DRS_CULLMODE, D3DCULL_NONE);   // the WORLD effects cull nothing
    g.RS(D3DRS_FILLMODE, D3DFILL_SOLID);
    g.RS(D3DRS_ALPHABLENDENABLE, TRUE);
    g.RS(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g.RS(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g.RS(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g.RS(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    g.RS(D3DRS_ALPHATESTENABLE, FALSE);
    g.RS(D3DRS_ALPHAREF, 0x5F);
    // Pass p0 of IDI_WORLDHEADLIGHT_FX: projected oT1 / oT2, samplers texture (WRAP), headlight and clip (CLAMP,
    // LINEAR).
    for (DWORD s = 0; s < 3; ++s) {
        g.TSS(s, D3DTSS_TEXCOORDINDEX, s);
        g.TSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, s == 0 ? D3DTTFF_DISABLE : D3DTTFF_PROJECTED);
        const DWORD address = s == 0 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        g.SS(s, D3DSAMP_ADDRESSU, address);
        g.SS(s, D3DSAMP_ADDRESSV, address);
        g.SS(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g.SS(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g.SS(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    }
    g.Texture(1, beam.texture);
    g.Texture(2, ramp_);
    g.Texture(0, nullptr);
    // HEADLIGHTCOLOR = (R, G, B, 0) / 128 (flt_787AF0), CRI+0x380 packed 0x00BBGGRR.
    const float k = CarHeadlightsU2::ColourScale;
    const float c0[4] = { (float)(colour & 0xFF) * k, (float)((colour >> 8) & 0xFF) * k,
                          (float)((colour >> 16) & 0xFF) * k, 0.0f };
    dev->SetPixelShaderConstantF(0, c0, 1);

    int draws = 0;
    const Mat4* lastTransform = nullptr;
    renderer.VisitSolidDraws(dev, LitBounds(body), [&](const WorldRenderer::SolidVisit& v) {
        if (v.transform != lastTransform) {
            lastTransform = v.transform;
            // c0..c3 WorldViewProj, c4..c7 HeadlightWVP, c8..c10 WorldView, c11 TextureOffset, c12 FogValue.
            float c[13][4] = {};
            Columns(*v.transform * cam.viewProjection, &c[0]);
            Columns(*v.transform * projector, &c[4]);
            float wv[4][4];
            Columns(*v.transform * cam.view, wv);
            for (int r = 0; r < 3; ++r)
                for (int q = 0; q < 4; ++q) c[8 + r][q] = wv[r][q];
            c[12][0] = 0.0f; c[12][1] = 0.0f; c[12][2] = 1.0f;   // oFog = 1 (fog is off in this pass)
            dev->SetVertexShaderConstantF(0, &c[0][0], 13);
        }
        dev->SetTexture(0, v.texture);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, v.cutOut ? TRUE : FALSE);
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, v.baseVertex, v.draw->minVertex, v.draw->vertexCount,
                                  v.draw->startIndex, v.draw->primitiveCount);
        ++draws;
    });
    return draws;
}

} // namespace bb
