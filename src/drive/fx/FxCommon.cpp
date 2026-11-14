#include "drive/fx/FxCommon.h"

#include "drive/cars/CarBin.h"
#include "drive/textures/TexturePack.h"
#include "render/RenderExtension.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bb {
namespace fx {

namespace {

constexpr uint32_t kVs11 = 0xFFFE0101u;     // vs_1_1 version token
constexpr uint32_t kPs11 = 0xFFFF0101u;     // ps_1_1 version token
constexpr uint32_t kEndToken = 0x0000FFFFu;

bool Contains(const uint8_t* p, size_t n, const char* text) {
    const size_t k = std::strlen(text);
    for (size_t i = 0; i + k <= n; ++i)
        if (std::memcmp(p + i, text, k) == 0) return true;
    return false;
}

// One shader token stream starting at `at` (a version token): its length in dwords, comments skipped by their
// own length. 0 when it runs off the blob.
size_t ShaderLength(const uint32_t* t, size_t count, size_t at) {
    size_t i = at + 1;
    while (i < count) {
        const uint32_t token = t[i];
        if ((token & 0xFFFFu) == 0xFFFEu) {
            i += 1 + (token >> 16);
            continue;
        }
        if (token == kEndToken) return i + 1 - at;
        ++i;
    }
    return 0;
}

void BuildMips(std::vector<std::vector<uint32_t>>& levels, int w, int h) {
    while (w > 1 || h > 1) {
        const std::vector<uint32_t>& src = levels.back();
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<uint32_t> dst((size_t)nw * nh);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                uint32_t sum[4] = {};
                for (int k = 0; k < 4; ++k) {
                    const int sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
                    const uint32_t c = src[(size_t)sy * w + sx];
                    for (int ch = 0; ch < 4; ++ch) sum[ch] += (c >> (ch * 8)) & 0xFF;
                }
                uint32_t o = 0;
                for (int ch = 0; ch < 4; ++ch) o |= ((sum[ch] + 2) / 4) << (ch * 8);
                dst[(size_t)y * nw + x] = o;
            }
        levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

IDirect3DTexture9* UploadLevels(IDirect3DDevice9* device, std::vector<std::vector<uint32_t>> levels, int w, int h,
                                bool mips) {
    if (levels.size() == 1 && mips) BuildMips(levels, w, h);
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(device->CreateTexture((UINT)w, (UINT)h, (UINT)levels.size(), 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex,
                                     nullptr)))
        return nullptr;
    const UINT n = std::min<UINT>((UINT)levels.size(), tex->GetLevelCount());
    for (UINT level = 0; level < n; ++level) {
        const int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
        if (levels[level].size() < (size_t)lw * lh) break;
        D3DLOCKED_RECT locked{};
        if (FAILED(tex->LockRect(level, &locked, nullptr, 0))) continue;
        for (int y = 0; y < lh; ++y)
            std::memcpy((uint8_t*)locked.pBits + (size_t)y * locked.Pitch, levels[level].data() + (size_t)y * lw,
                        (size_t)lw * 4);
        tex->UnlockRect(level);
    }
    return tex;
}

} // namespace

bool ExtractEffectPass(const std::wstring& exePath, const wchar_t* resource, const char* vsMarker, const char* psMarker,
                       std::vector<uint32_t>& vs, std::vector<uint32_t>& ps, std::string& error) {
    vs.clear();
    ps.clear();
    HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        error = "cannot open SPEED2.EXE";
        return false;
    }
    bool ok = false;
    if (HRSRC info = ::FindResourceW(module, resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */)) {
        const DWORD size = ::SizeofResource(module, info);
        HGLOBAL handle = ::LoadResource(module, info);
        const uint8_t* data = handle ? static_cast<const uint8_t*>(::LockResource(handle)) : nullptr;
        if (data && size >= 8) {
            std::vector<uint32_t> tokens(size / 4);
            std::memcpy(tokens.data(), data, tokens.size() * 4);
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (tokens[i] != kVs11 && tokens[i] != kPs11) continue;
                const size_t n = ShaderLength(tokens.data(), tokens.size(), i);
                if (n == 0) continue;
                if (tokens[i - 1] != n * 4) continue;   // not the size the effect records
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(tokens.data() + i);
                if (tokens[i] == kVs11 && vs.empty() && Contains(bytes, n * 4, vsMarker))
                    vs.assign(tokens.begin() + (std::ptrdiff_t)i, tokens.begin() + (std::ptrdiff_t)(i + n));
                else if (tokens[i] == kPs11 && ps.empty() && (!psMarker || Contains(bytes, n * 4, psMarker)))
                    ps.assign(tokens.begin() + (std::ptrdiff_t)i, tokens.begin() + (std::ptrdiff_t)(i + n));
                i += n - 1;
            }
            ok = !vs.empty() && !ps.empty();
            if (!ok) error = "the effect holds no matching vs_1_1/ps_1_1 pair";
        } else {
            error = "cannot load the effect resource";
        }
    } else {
        error = "SPEED2.EXE has no such effect resource";
    }
    ::FreeLibrary(module);
    return ok;
}

IDirect3DTexture9* CreateArgbTexture(IDirect3DDevice9* device, const uint32_t* texels, int w, int h, bool mips) {
    std::vector<std::vector<uint32_t>> levels(1, std::vector<uint32_t>(texels, texels + (size_t)w * h));
    return UploadLevels(device, std::move(levels), w, h, mips);
}

IDirect3DTexture9* CreateGameTexture(IDirect3DDevice9* device, const Texture& t) {
    if (t.Width <= 0 || t.Height <= 0) return nullptr;
    const bool dxt = t.Format == TextureFormat::Dxt1 || t.Format == TextureFormat::Dxt3 || t.Format == TextureFormat::Dxt5;
    if (!dxt && t.Format != TextureFormat::Argb32) {
        std::vector<std::vector<uint32_t>> levels;
        for (int level = 0; level < t.NumMipMapLevels; ++level) {
            std::vector<uint32_t> px;
            int w = 0, h = 0;
            if (!t.ToArgb32(level, px, w, h)) break;
            levels.push_back(std::move(px));
        }
        if (levels.empty()) return nullptr;
        return UploadLevels(device, std::move(levels), t.Width, t.Height, true);
    }
    const D3DFORMAT format = t.Format == TextureFormat::Dxt1   ? D3DFMT_DXT1
                             : t.Format == TextureFormat::Dxt3 ? D3DFMT_DXT3
                             : t.Format == TextureFormat::Dxt5 ? D3DFMT_DXT5
                                                               : D3DFMT_A8R8G8B8;
    int levels = 0;
    size_t total = 0;
    for (int level = 0; level < t.NumMipMapLevels; ++level) {
        const size_t size = (size_t)t.MipSize(level);
        if (total + size > t.Image.size()) break;
        total += size;
        ++levels;
    }
    if (levels == 0) return nullptr;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(device->CreateTexture((UINT)t.Width, (UINT)t.Height, (UINT)levels, 0, format, D3DPOOL_MANAGED, &tex, nullptr)))
        return nullptr;
    size_t offset = 0;
    for (int level = 0; level < levels && level < (int)tex->GetLevelCount(); ++level) {
        const int w = std::max(1, t.Width >> level), h = std::max(1, t.Height >> level);
        const int rows = dxt ? std::max(1, (h + 3) / 4) : h;
        const size_t rowBytes = dxt ? (size_t)std::max(1, (w + 3) / 4) * (t.Format == TextureFormat::Dxt1 ? 8 : 16)
                                    : (size_t)w * 4;
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(tex->LockRect((UINT)level, &locked, nullptr, 0))) {
            for (int r = 0; r < rows; ++r)
                std::memcpy((uint8_t*)locked.pBits + (size_t)r * locked.Pitch, t.Image.data() + offset + r * rowBytes,
                            rowBytes);
            tex->UnlockRect((UINT)level);
        }
        offset += (size_t)t.MipSize(level);
    }
    return tex;
}

FxTexture DescribeTexture(const Texture& t) {
    FxTexture f;
    const int blend = (int)t.AlphaBlend_;
    f.blended = blend != 0;
    f.punchThrough = t.AlphaUsage_ == AlphaUsage::PunchThrough;
    switch (blend) {
        case 0: f.src = D3DBLEND_ONE; f.dst = D3DBLEND_ZERO; break;
        case 2: f.src = D3DBLEND_SRCALPHA; f.dst = D3DBLEND_ONE; break;
        case 5: f.src = D3DBLEND_DESTALPHA; f.dst = D3DBLEND_INVDESTALPHA; break;
        case 6: f.src = D3DBLEND_DESTALPHA; f.dst = D3DBLEND_ONE; break;
        default: f.src = D3DBLEND_SRCALPHA; f.dst = D3DBLEND_INVSRCALPHA; break; // 1, 3, 4, >= 7
    }
    f.renderingOrder = t.RenderingOrder;
    f.name = t.Name;
    return f;
}

// ---------------------------------------------------------------------------
// GlobalTextures
// ---------------------------------------------------------------------------

GlobalTextures::GlobalTextures() = default;
GlobalTextures::~GlobalTextures() { Release(); }

bool GlobalTextures::Load(const std::wstring& gameFolder) {
    if (!packs_.empty() && folder_ == gameFolder) return true;
    Release();
    packs_.clear();
    auto buf = std::make_shared<std::vector<uint8_t>>();
    if (!carbin::ReadFile(carbin::GamePath(gameFolder, "GLOBAL\\GLOBALB.BUN"), *buf) || buf->empty()) return false;
    packs_ = TexturePack::ReadAll(buf);
    folder_ = gameFolder;
    return !packs_.empty();
}

const Texture* GlobalTextures::Find(uint32_t hash) const {
    for (const auto& p : packs_)
        if (p->Contains(hash))
            if (const Texture* t = p->Get(hash)) return t;
    return nullptr;
}

const FxTexture& GlobalTextures::Get(IDirect3DDevice9* device, uint32_t hash) {
    auto it = made_.find(hash);
    if (it != made_.end()) return it->second;
    FxTexture f;
    if (const Texture* t = Find(hash)) {
        f = DescribeTexture(*t);
        f.texture = CreateGameTexture(device, *t);
    }
    return made_.emplace(hash, f).first->second;
}

void GlobalTextures::Release() {
    for (auto& kv : made_)
        if (kv.second.texture) kv.second.texture->Release();
    made_.clear();
}

// ---------------------------------------------------------------------------
// StateGuard
// ---------------------------------------------------------------------------

StateGuard::StateGuard(IDirect3DDevice9* device) : d_(device) {}

StateGuard::~StateGuard() {
    for (auto it = rs_.rbegin(); it != rs_.rend(); ++it) d_->SetRenderState(it->first, it->second);
    for (auto it = ss_.rbegin(); it != ss_.rend(); ++it) d_->SetSamplerState(it->first.first, it->first.second, it->second);
    for (auto it = tss_.rbegin(); it != tss_.rend(); ++it)
        d_->SetTextureStageState(it->first.first, it->first.second, it->second);
    for (auto it = tex_.rbegin(); it != tex_.rend(); ++it) {
        d_->SetTexture(it->first, it->second);
        if (it->second) it->second->Release();
    }
    if (shaders_) {
        d_->SetVertexShader(vs_);
        d_->SetPixelShader(ps_);
        if (vs_) vs_->Release();
        if (ps_) ps_->Release();
    }
    if (decl_) {
        if (declaration_) {
            d_->SetVertexDeclaration(declaration_);
            declaration_->Release();
        } else if (fvf_) {
            d_->SetFVF(fvf_);
        }
    }
}

void StateGuard::RS(D3DRENDERSTATETYPE state, DWORD value) {
    bool seen = false;
    for (const auto& e : rs_) if (e.first == state) { seen = true; break; }
    if (!seen) {
        DWORD old = 0;
        d_->GetRenderState(state, &old);
        rs_.emplace_back(state, old);
    }
    d_->SetRenderState(state, value);
}

void StateGuard::SS(DWORD sampler, D3DSAMPLERSTATETYPE state, DWORD value) {
    bool seen = false;
    for (const auto& e : ss_) if (e.first.first == sampler && e.first.second == state) { seen = true; break; }
    if (!seen) {
        DWORD old = 0;
        d_->GetSamplerState(sampler, state, &old);
        ss_.push_back({ { sampler, state }, old });
    }
    d_->SetSamplerState(sampler, state, value);
}

void StateGuard::TSS(DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value) {
    bool seen = false;
    for (const auto& e : tss_) if (e.first.first == stage && e.first.second == state) { seen = true; break; }
    if (!seen) {
        DWORD old = 0;
        d_->GetTextureStageState(stage, state, &old);
        tss_.push_back({ { stage, state }, old });
    }
    d_->SetTextureStageState(stage, state, value);
}

void StateGuard::Texture(DWORD stage, IDirect3DBaseTexture9* texture) {
    bool seen = false;
    for (const auto& e : tex_) if (e.first == stage) { seen = true; break; }
    if (!seen) {
        IDirect3DBaseTexture9* old = nullptr;
        d_->GetTexture(stage, &old);   // AddRef'd
        tex_.emplace_back(stage, old);
    }
    d_->SetTexture(stage, texture);
}

void StateGuard::Shaders(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps) {
    if (!shaders_) {
        d_->GetVertexShader(&vs_);
        d_->GetPixelShader(&ps_);
        shaders_ = true;
    }
    d_->SetVertexShader(vs);
    d_->SetPixelShader(ps);
}

void StateGuard::Declaration(IDirect3DVertexDeclaration9* decl) {
    if (!decl_) {
        d_->GetVertexDeclaration(&declaration_);
        d_->GetFVF(&fvf_);
        decl_ = true;
    }
    d_->SetVertexDeclaration(decl);
}

void StateGuard::Fvf(DWORD fvf) {
    if (!decl_) {
        d_->GetVertexDeclaration(&declaration_);
        d_->GetFVF(&fvf_);
        decl_ = true;
    }
    d_->SetFVF(fvf);
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

DeviceCamera ReadDeviceCamera(IDirect3DDevice9* device) {
    DeviceCamera c;
    D3DMATRIX v{}, p{};
    device->GetTransform(D3DTS_VIEW, &v);
    device->GetTransform(D3DTS_PROJECTION, &p);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) {
            c.view.m[r][k] = v.m[r][k];
            c.projection.m[r][k] = p.m[r][k];
        }
    c.viewProjection = c.view * c.projection;
    const Mat4 inv = InverseAffine(c.view);
    c.eye = { inv.m[3][0], inv.m[3][1], inv.m[3][2] };
    c.right = { c.view.m[0][0], c.view.m[1][0], c.view.m[2][0] };
    c.down = { -c.view.m[0][1], -c.view.m[1][1], -c.view.m[2][1] };
    c.forward = { -c.view.m[0][2], -c.view.m[1][2], -c.view.m[2][2] };
    D3DVIEWPORT9 vp{};
    device->GetViewport(&vp);
    c.width = (int)std::max<DWORD>(1, vp.Width);
    c.height = (int)std::max<DWORD>(1, vp.Height);
    return c;
}

FlareViewU2 MakeFlareView(const DeviceCamera& cam, int viewId) {
    FlareViewU2 v;
    v.ViewId = viewId;
    v.Width = cam.width;
    v.Height = cam.height;
    const Vec3 r = cam.right, d = cam.down, f = cam.forward, e = cam.eye;
    float* m = v.CamMatrix;
    m[0] = r.x; m[4] = r.y; m[8] = r.z;
    m[1] = d.x; m[5] = d.y; m[9] = d.z;
    m[2] = f.x; m[6] = f.y; m[10] = f.z;
    m[3] = m[7] = m[11] = 0.0f;
    m[12] = -(e.x * r.x + e.y * r.y + e.z * r.z);
    m[13] = -(e.x * d.x + e.y * d.y + e.z * d.z);
    m[14] = -(e.x * f.x + e.y * f.y + e.z * f.z);
    m[15] = 1.0f;
    v.CamPos[0] = e.x; v.CamPos[1] = e.y; v.CamPos[2] = e.z;
    v.CamDir[0] = f.x; v.CamDir[1] = f.y; v.CamDir[2] = f.z;
    // The editor's right-handed projection: m22 = zf / (zn - zf), m32 = zn zf / (zn - zf).
    const float xs = cam.projection.m[0][0], ys = cam.projection.m[1][1];
    const float m22 = cam.projection.m[2][2], m32 = cam.projection.m[3][2];
    float zn = 0.25f, zf = 15000.0f;
    if (m22 != 0.0f && m22 != -1.0f) {
        zn = m32 / m22;
        zf = m32 / (m22 + 1.0f);
    }
    const float tanH = 1.0f / xs;
    const bool player = viewId == 1 || viewId == 2 || viewId == 4 || viewId == 5;
    const float rawHalf = std::atan(tanH) / (player ? 1.1f : 1.0f);
    v.Focal = 256.0f / std::tan(rawHalf);
    const float q = zf / (zf - zn);
    float p[16] = {};
    p[0] = xs; p[5] = -ys; p[10] = q; p[11] = 1.0f; p[14] = -q * zn;
    float* vp = v.ViewProj;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += m[i * 4 + k] * p[k * 4 + j];
            vp[i * 4 + j] = s;
        }
    // clip = p . VP (row vector): inside when -w < x < w, -w < y < w, 0 < z < w
    static const int combos[6][3] = { { 3, 0, 1 }, { 3, 0, -1 }, { 3, 1, 1 }, { 3, 1, -1 }, { 2, 2, 0 }, { 3, 2, -1 } };
    for (int pl = 0; pl < 6; pl++) {
        const int a = combos[pl][0], b = combos[pl][1], sgn = combos[pl][2];
        for (int row = 0; row < 4; row++)
            v.Planes[pl * 4 + row] = pl == 4 ? vp[row * 4 + 2] : vp[row * 4 + a] + (float)sgn * vp[row * 4 + b];
    }
    return v;
}

// ---------------------------------------------------------------------------
// PolyRenderer
// ---------------------------------------------------------------------------

namespace {

const D3DVERTEXELEMENT9 kPolyDecl[] = {
    { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
    { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END(),
};
constexpr DWORD kPolyFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

} // namespace

bool PolyRenderer::Create(IDirect3DDevice9* device, const std::wstring& exePath) {
    Release();
    device_ = device;
    std::vector<uint32_t> vs, ps;
    std::string why;
    if (!ExtractEffectPass(exePath, L"IDI_WORLDNODEPTH_FX", "TextureOffset", nullptr, vs, ps, why)) {
        status_ = "WORLDNODEPTH: fixed-function fallback (" + why + ")";
        return true;
    }
    if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.data()), &vs_)) ||
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &ps_)) ||
        FAILED(device->CreateVertexDeclaration(kPolyDecl, &decl_))) {
        if (vs_) vs_->Release();
        if (ps_) ps_->Release();
        if (decl_) decl_->Release();
        vs_ = nullptr; ps_ = nullptr; decl_ = nullptr;
        status_ = "WORLDNODEPTH: fixed-function fallback (the device refused vs_1_1/ps_1_1)";
        return true;
    }
    status_ = "WORLDNODEPTH effect from SPEED2.EXE";
    return true;
}

void PolyRenderer::Release() {
    if (vs_) vs_->Release();
    if (ps_) ps_->Release();
    if (decl_) decl_->Release();
    vs_ = nullptr; ps_ = nullptr; decl_ = nullptr;
    device_ = nullptr;
}

void PolyRenderer::Begin(StateGuard& g, const DeviceCamera& cam) {
    IDirect3DDevice9* d = device_;
    // Pass p0 of technique "world" (worldnodepth.txt) and the flare pass's ZWRITEENABLE 0.
    g.RS(D3DRS_ZENABLE, D3DZB_TRUE);
    g.RS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g.RS(D3DRS_ZWRITEENABLE, FALSE);
    g.RS(D3DRS_FOGENABLE, FALSE);
    g.RS(D3DRS_COLORWRITEENABLE, 7);
    g.RS(D3DRS_CULLMODE, D3DCULL_NONE);
    g.RS(D3DRS_LIGHTING, FALSE);
    g.RS(D3DRS_FILLMODE, D3DFILL_SOLID);
    g.RS(D3DRS_ALPHATESTENABLE, FALSE);
    g.RS(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    g.RS(D3DRS_ALPHAREF, 0);
    g.RS(D3DRS_ALPHABLENDENABLE, TRUE);
    g.RS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g.RS(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g.RS(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g.RS(D3DRS_DEPTHBIAS, 0);
    g.RS(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    g.SS(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    g.SS(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    g.SS(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g.SS(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g.SS(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    g.TSS(0, D3DTSS_TEXCOORDINDEX, 0);
    g.TSS(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    if (vs_ && ps_ && decl_) {
        g.Declaration(decl_);
        g.Shaders(vs_, ps_);
        // c0..c3 WorldViewProj (identity l2w: the polys are in world space), as columns for the dp4s;
        // c4 TextureOffset 0.
        float c[5][4] = {};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) c[r][k] = cam.viewProjection.m[k][r];
        d->SetVertexShaderConstantF(0, &c[0][0], 5);
    } else {
        // technique "world_fixed": MODULATE2X texture x diffuse, alpha MODULATE.
        g.Shaders(nullptr, nullptr);
        g.Fvf(kPolyFvf);
        const D3DMATRIX id = ToD3DMatrix(Mat4::Identity());
        d->SetTransform(D3DTS_WORLD, &id);
        g.TSS(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        g.TSS(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g.TSS(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g.TSS(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        g.TSS(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        g.TSS(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        g.TSS(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g.TSS(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

void PolyRenderer::Draw(StateGuard& g, const FxTexture& texture, D3DPRIMITIVETYPE type, const Vertex* v, int count) {
    if (!texture.texture || count < 3) return;
    g.Texture(0, texture.texture);
    // BlendState (sub_5BCC30)
    g.RS(D3DRS_ALPHABLENDENABLE, texture.blended ? TRUE : FALSE);
    if (texture.blended) {
        g.RS(D3DRS_SRCBLEND, texture.src);
        g.RS(D3DRS_DESTBLEND, texture.dst);
    }
    g.RS(D3DRS_ALPHATESTENABLE, texture.punchThrough ? TRUE : FALSE);
    g.RS(D3DRS_ALPHAREF, texture.punchThrough ? 0x5F : 0);
    const int prims = type == D3DPT_TRIANGLELIST ? count / 3 : count - 2;
    device_->DrawPrimitiveUP(type, (UINT)prims, v, sizeof(Vertex));
}

} // namespace fx
} // namespace bb
