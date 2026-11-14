#include "drive/CarModel.h"

#include "drive/CarShader.h"
#include "drive/cars/CarBin.h"
#include "drive/cars/CarPartDatabaseU2.h"
#include "drive/cars/CarRenderLayoutU2.h"
#include "drive/cars/CarSkinCompositorU2.h"
#include "drive/cars/CarTexturesU2.h"
#include "drive/cars/CarTypeInfoU2.h"
#include "drive/cars/RideInfoU2.h"
#include "drive/geometry/CarLightingU2.h"
#include "drive/geometry/LightMaterialU2.h"
#include "drive/geometry/SolidList.h"
#include "drive/textures/TexturePack.h"
#include "render/D3D9Context.h"
#include "render/WorldRenderer.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace bb {

namespace {

// Shared streaming/resident geometry packs loaded at boot (car-render-models.md 6.2).
const char* const sharedGeometry[] = {
    "EXHAUST", "BRAKES", "MIRRORS_POST", "MIRRORS_BODY", "MIRRORS_SUV", "MIRRORS_HUMMER",
    "SPOILER", "SPOILER_HATCH", "SPOILER_SUV", "ROOF", "AUDIO", "PLATES",
};

// Texture lookup order after the car's own packs: the shared packs, then GLOBALB.
const char* const sharedTextures[] = {
    "CARS\\TEXTURES.BIN", "CARS\\WHEELS\\TEXTURES.BIN", "CARS\\BRAKES\\TEXTURES.BIN", "CARS\\PLATES\\TEXTURES.BIN",
};

// Light materials CarRenderInfo::Render replaces per car (ReplaceLightMaterial, car-render-models.md
// 4.2): CARSKIN and CARSKIN_CARBONFIBRE get the paint's (CRI+0x384, stock: no carbon parts),
// WINDSHIELD the window tint's (CRI+0x38C).
constexpr uint32_t kLightCarSkin = 0xD6D6080A, kLightCarSkinCarbon = 0xA7366AE6, kLightWindshield = 0x471A1DCA;

// Effect ids (gEffects, car-shading.md 2.1): what a mesh is drawn with, from the OR of its
// material flags (sub_5C58A0): 0x01 WORLDBONE, 0x40 WORLDREFLECT, 0x80 CAR, else WORLD. The
// world-like ones are drawn as WORLD (2 * texture * vertex colour) here.
constexpr int kEffectWorld = 0, kEffectCar = 5;

// CAR vertex declaration (descriptor id 5): FLOAT3 POSITION, FLOAT3 NORMAL, D3DCOLOR COLOR0,
// FLOAT2 TEXCOORD0, stride 0x24. Same bytes as the fixed-function FVF below.
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
const D3DVERTEXELEMENT9 kCarDecl[] = {
    { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
    { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
    { 0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END(),
};
#pragma pack(push, 1)
struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t colour;
    float u, v;
};
#pragma pack(pop)
static_assert(sizeof(Vertex) == 36, "Vertex must match kFvf");

D3DMATRIX ToD3D(const Mat4& m) {
    D3DMATRIX out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = m.m[r][c];
    return out;
}

Vec3 Row(const Mat4& m, int r) { return { m.m[r][0], m.m[r][1], m.m[r][2] }; }

Mat4 FromRows(const Vec3& f, const Vec3& l, const Vec3& u, const Vec3& p) {
    Mat4 m = Mat4::Identity();
    const Vec3 rows[4] = { f, l, u, p };
    for (int r = 0; r < 4; ++r) {
        m.m[r][0] = rows[r].x;
        m.m[r][1] = rows[r].y;
        m.m[r][2] = rows[r].z;
    }
    return m;
}

// How a texture is drawn (CarMaterialState.Apply, sub_5BCC30).
struct TextureState {
    IDirect3DTexture9* texture = nullptr;
    bool blended = false;
    bool punchThrough = false;
    DWORD src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
    bool sorted = false;       // ApplyAlphaSorting (+0x54): the second bucket group
    int renderingOrder = 0;    // RenderingOrder (+0x51)
};

// One material range (strip) of one solid, with the texture it ends up with for one draw item.
struct DrawRange {
    uint32_t texture = 0;      // resolved hash
    int firstIndex = 0, primitiveCount = 0;
    bool specular = false;     // fixed-function fallback only
    int effect = kEffectCar;
    uint32_t lightMaterial = 0;                 // resolved light material hash (for Describe)
    LightMaterialU2::ShaderParams constants;    // SetLightMaterial (0x5BD4A0) for this strip
};

struct GpuSolid {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    int vertexCount = 0;
};

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

IDirect3DTexture9* CreateArgbTexture(IDirect3DDevice9* device, std::vector<std::vector<uint32_t>> levels, int w, int h) {
    if (levels.size() == 1) BuildMips(levels, w, h);
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

IDirect3DTexture9* CreateTexture(IDirect3DDevice9* device, const Texture& t) {
    if (t.Width <= 0 || t.Height <= 0) return nullptr;
    const bool dxt = t.Format == TextureFormat::Dxt1 || t.Format == TextureFormat::Dxt3 || t.Format == TextureFormat::Dxt5;
    if (!dxt && t.Format != TextureFormat::Argb32) {
        // P8 (and anything else ToArgb32 knows): expand every level.
        std::vector<std::vector<uint32_t>> levels;
        for (int level = 0; level < t.NumMipMapLevels; ++level) {
            std::vector<uint32_t> px;
            int w = 0, h = 0;
            if (!t.ToArgb32(level, px, w, h)) break;
            levels.push_back(std::move(px));
        }
        if (levels.empty()) return nullptr;
        return CreateArgbTexture(device, std::move(levels), t.Width, t.Height);
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

} // namespace

struct CarModel::Impl {
    // CPU side
    const CarTypeInfoU2* carType = nullptr;
    CarTypeInfoU2 carCopy;
    std::shared_ptr<std::vector<uint8_t>> globalB;
    std::unique_ptr<CarPartDatabaseU2> db;
    std::unique_ptr<RideInfoU2> ride;
    std::vector<std::unique_ptr<SolidList>> solidPacks;
    std::vector<std::unique_ptr<TexturePack>> packs;
    std::unique_ptr<CarTexturesU2> swaps;
    std::unique_ptr<CarRenderLayoutU2> layout;
    std::vector<uint32_t> compositeSkin;   // DUMMY_SKINn texels, empty when not composited
    int compositeWidth = 0, compositeHeight = 0;
    int lod = 0;
    std::vector<CarDrawItem> items;                 // body-relative (identity body)
    std::vector<std::vector<DrawRange>> ranges;     // per item
    LightMaterialU2::Library lights;                // GLOBALB's light materials
    std::wstring gameFolder;
    bool cpuLoaded = false;

    // GPU side
    IDirect3DDevice9* device = nullptr;
    std::unordered_map<const Solid*, GpuSolid> solids;
    std::unordered_map<uint32_t, TextureState> textures;
    IDirect3DStateBlock9* saved = nullptr;
    bool gpuLoaded = false;
    // The game's CAR effect (pass 0); null when unavailable, and then the fixed-function fallback.
    IDirect3DVertexShader9* carVs = nullptr;
    IDirect3DPixelShader9* carPs = nullptr;
    IDirect3DVertexDeclaration9* carDecl = nullptr;
    IDirect3DTexture9* missingTexture = nullptr;    // 1x1 grey, gloss 0x80
    // Bound when there is no environment cube: an empty sampler reads white on some drivers.
    IDirect3DCubeTexture9* blackCube = nullptr;
    CarEnvironmentMap environment;
    std::string shaderStatus;

    // A device reset fails while state blocks or default-pool objects exist: the state block and
    // the environment cube go in D3D9Context::beforeReset (chained onto whatever was there) and
    // come back on the next Draw.
    D3D9Context* hookedContext = nullptr;
    std::function<void()> previousBeforeReset;

    void BeforeReset() {
        if (saved) saved->Release();
        saved = nullptr;
        environment.Release();
    }

    void HookReset(D3D9Context* context) {
        if (hookedContext || !context) return;
        hookedContext = context;
        previousBeforeReset = context->beforeReset;
        std::function<void()> previous = previousBeforeReset;
        context->beforeReset = [this, previous] {
            BeforeReset();
            if (previous) previous();
        };
    }

    void UnhookReset() {
        if (hookedContext) hookedContext->beforeReset = previousBeforeReset;
        hookedContext = nullptr;
        previousBeforeReset = nullptr;
    }

    // Light material of a strip after CarRenderInfo::Render's ReplaceLightMaterial calls (stock
    // car), as the Unity port's CarModelBuilder.ResolveLightMaterial: CARSKIN and
    // CARSKIN_CARBONFIBRE get the paint's, WINDSHIELD the window tint's (or WINDSHIELD itself).
    // Index 0xFF = DefaultLightMaterial. Rim/exhaust/caliper/... (CRI+0x390..0x3A8) are not
    // replaced: their names come by usage and are not traced, so the solid's own stays.
    const LightMaterialU2& ResolveLightMaterial(const Solid& s, const SolidMaterial& m, uint32_t& hash) const {
        const int idx = m.LightMaterialIndex & 0xFF;
        if (idx == 0xFF || idx >= (int)s.LightMaterialHashes.size()) {
            hash = lights.Default().NameHash;
            return lights.Default();
        }
        uint32_t h = s.LightMaterialHashes[(size_t)idx];
        if (h == kLightCarSkin || h == kLightCarSkinCarbon) {
            h = ride->Parts[63] >= 0 ? ride->Db->GetLightMaterialName(ride->Parts[63]) : 0;
        } else if (h == kLightWindshield) {
            const uint32_t tint = ride->Parts[153] >= 0 ? ride->Db->GetLightMaterialName(ride->Parts[153]) : 0;
            h = tint != 0 ? tint : kLightWindshield;
        }
        const LightMaterialU2& lm = lights.Get(h);
        hash = lm.NameHash;
        return lm;
    }

    const Solid* FindSolid(uint32_t hash) const {
        for (const auto& p : solidPacks)
            if (const Solid* s = p->Find(hash)) return s;
        return nullptr;
    }

    const Texture* FindTexture(uint32_t hash) const {
        for (const auto& p : packs)
            if (p->Contains(hash))
                if (const Texture* t = p->Get(hash)) return t;
        return nullptr;
    }

    std::string TextureName(uint32_t hash) const {
        if (const Texture* t = FindTexture(hash)) return t->Name;
        char text[24];
        std::snprintf(text, sizeof text, "MISSING_%08X", hash);
        return text;
    }

    void BuildItems() {
        items = layout->Build(lod, lod, false);
        ranges.clear();
        ranges.reserve(items.size());
        for (const CarDrawItem& item : items) {
            const Solid& s = *item.Solid_;
            std::vector<DrawRange> list;
            uint32_t meshFlags = 0;   // [mesh+0x0C]: the OR of the material flags
            for (const SolidMaterial& m : s.Materials) meshFlags |= m.Flags;
            const int effect = (meshFlags & 0x01) || (meshFlags & 0x40) || !(meshFlags & 0x80) ? kEffectWorld : kEffectCar;
            for (const SolidMaterial& m : s.Materials) {
                DrawRange r;
                uint32_t texHash = m.TextureIndex >= 0 && m.TextureIndex < (int)s.TextureHashes.size()
                                       ? s.TextureHashes[(size_t)m.TextureIndex] : 0;
                texHash = swaps->Resolve(texHash, item.Slot, item.Slot == 27, item.LeftBrake);
                r.texture = texHash;
                r.firstIndex = m.IndexOffset;
                r.primitiveCount = m.NumIndices / 3;
                const int lm = m.LightMaterialIndex & 0xFF;
                if (lm != 0xFF && lm < (int)s.LightMaterialHashes.size()) {
                    const uint32_t h = s.LightMaterialHashes[(size_t)lm];
                    r.specular = h == kLightCarSkin || h == kLightCarSkinCarbon || h == kLightWindshield;
                }
                r.effect = effect;
                r.constants = ResolveLightMaterial(s, m, r.lightMaterial).ToShaderParams(m.Flags, true);
                if (r.primitiveCount > 0 && m.IndexOffset + m.NumIndices <= (int)s.Indices.size()) list.push_back(r);
            }
            ranges.push_back(std::move(list));
        }
    }

    bool LoadCpu(const std::wstring& folder, const CarTypeInfoU2& car, std::wstring& error) {
        carCopy = car;
        carType = &carCopy;
        globalB = std::make_shared<std::vector<uint8_t>>();
        if (!carbin::ReadFile(carbin::GamePath(folder, "GLOBAL\\GLOBALB.BUN"), *globalB)) {
            error = L"cannot read GLOBAL\\GLOBALB.BUN";
            return false;
        }
        try {
            db = std::make_unique<CarPartDatabaseU2>(CarPartDatabaseU2::Read(*globalB));
        } catch (const std::exception&) {
            error = L"GLOBALB.BUN holds no car part database";
            return false;
        }
        gameFolder = folder;
        lights = LightMaterialU2::Library(LightMaterialU2::ReadAll(*globalB));
        ride = std::make_unique<RideInfoU2>(db.get(), carType, true);
        ride->SetStockParts();

        auto loadGeometry = [&](const std::string& rel) {
            std::vector<uint8_t> buf;
            if (!carbin::ReadFile(carbin::GamePath(folder, rel), buf) || buf.empty()) return false;
            try {
                auto list = SolidList::Read(buf);
                if (!list->Solids.empty()) solidPacks.push_back(std::move(list));
                return true;
            } catch (const std::exception&) {
                return false;
            }
        };
        if (!loadGeometry(carType->GeometryFilename)) {
            error = L"cannot read the car's geometry";
            return false;
        }
        for (const char* g : sharedGeometry) loadGeometry(std::string("CARS\\") + g + "\\GEOMETRY.BIN");
        // Aftermarket rims live in CARS\WHEELS\GEOMETRY_<WHEEL_MANUFACTURER name>.BIN.
        const int maker = ride->Parts[168];
        if (maker >= 0) loadGeometry("CARS\\WHEELS\\GEOMETRY_" + db->NameOf(maker) + ".BIN");

        auto loadTextures = [&](const std::string& rel) {
            auto buf = std::make_shared<std::vector<uint8_t>>();
            if (!carbin::ReadFile(carbin::GamePath(folder, rel), *buf) || buf->empty()) return;
            for (auto& p : TexturePack::ReadAll(buf)) packs.push_back(std::move(p));
        };
        loadTextures("CARS\\" + carType->CollectionName + "\\TEXTURES.BIN");
        if (carType->UsageType != 2) loadTextures("CARS\\" + carType->CollectionName + "\\VINYLS.BIN");
        for (const char* t : sharedTextures) loadTextures(t);
        for (auto& p : TexturePack::ReadAll(globalB)) packs.push_back(std::move(p));

        // Composite slot 1 for the driven car; traffic never calls SetCompositeNameHash.
        swaps = std::make_unique<CarTexturesU2>(ride.get(), carType->UsageType == 2 ? 0 : 1, false,
                                                [this](uint32_t h) { return FindTexture(h) != nullptr; });
        if (swaps->CompositeSkinHash != 0) {
            const Texture* dst = FindTexture(swaps->CompositeSkinHash);
            compositeSkin = CarSkinCompositorU2::Composite(*ride, dst, [this](uint32_t h) { return FindTexture(h); });
            if (dst && !compositeSkin.empty()) {
                compositeWidth = dst->Width;
                compositeHeight = dst->Height;
            }
        }

        layout = std::make_unique<CarRenderLayoutU2>(ride.get(), [this](uint32_t h) { return FindSolid(h); });
        layout->FrontEnd = false;
        BuildItems();
        if (items.empty()) {
            error = L"the car has no drawable parts";
            return false;
        }
        cpuLoaded = true;
        return true;
    }

    bool UploadSolid(const Solid& s) {
        if (solids.count(&s)) return true;
        GpuSolid g;
        const int n = s.VertexCount;
        if (n <= 0 || s.Indices.empty()) return false;
        std::vector<Vertex> verts((size_t)n);
        const bool hasNormals = !s.Normals.empty();
        std::vector<Vec3> normals;
        if (!hasNormals) {
            normals.assign((size_t)n, Vec3{});
            for (size_t t = 0; t + 2 < s.Indices.size(); t += 3) {
                const int a = s.Indices[t], b = s.Indices[t + 1], c = s.Indices[t + 2];
                if (a >= n || b >= n || c >= n) continue;
                auto P = [&](int i) { return Vec3{ s.Positions[i * 3], s.Positions[i * 3 + 1], s.Positions[i * 3 + 2] }; };
                const Vec3 nrm = Cross(P(b) - P(a), P(c) - P(a));
                normals[a] += nrm;
                normals[b] += nrm;
                normals[c] += nrm;
            }
        }
        for (int i = 0; i < n; ++i) {
            Vertex& v = verts[(size_t)i];
            v.x = s.Positions[i * 3];
            v.y = s.Positions[i * 3 + 1];
            v.z = s.Positions[i * 3 + 2];
            Vec3 nrm = hasNormals ? Vec3{ s.Normals[i * 3], s.Normals[i * 3 + 1], s.Normals[i * 3 + 2] }
                                  : Normalize(normals[(size_t)i]);
            v.nx = nrm.x;
            v.ny = nrm.y;
            v.nz = nrm.z;
            v.colour = s.Colors[(size_t)i];
            v.u = s.UVs[i * 2];
            v.v = s.UVs[i * 2 + 1];
        }
        if (FAILED(device->CreateVertexBuffer((UINT)(verts.size() * sizeof(Vertex)), D3DUSAGE_WRITEONLY, kFvf,
                                              D3DPOOL_MANAGED, &g.vb, nullptr)))
            return false;
        void* p = nullptr;
        if (SUCCEEDED(g.vb->Lock(0, 0, &p, 0))) {
            std::memcpy(p, verts.data(), verts.size() * sizeof(Vertex));
            g.vb->Unlock();
        }
        if (FAILED(device->CreateIndexBuffer((UINT)(s.Indices.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                             D3DPOOL_MANAGED, &g.ib, nullptr))) {
            g.vb->Release();
            return false;
        }
        if (SUCCEEDED(g.ib->Lock(0, 0, &p, 0))) {
            std::memcpy(p, s.Indices.data(), s.Indices.size() * 2);
            g.ib->Unlock();
        }
        g.vertexCount = n;
        solids[&s] = g;
        return true;
    }

    void UploadTexture(uint32_t hash) {
        if (textures.count(hash)) return;
        TextureState st;
        const Texture* src = FindTexture(hash);
        if (src) {
            if (swaps && hash == swaps->CompositeSkinHash && !compositeSkin.empty())
                st.texture = CreateArgbTexture(device, { compositeSkin }, compositeWidth, compositeHeight);
            else
                st.texture = CreateTexture(device, *src);
            const int blend = (int)src->AlphaBlend_;
            st.blended = blend != 0;
            st.punchThrough = src->AlphaUsage_ == AlphaUsage::PunchThrough;
            switch (blend) {
                case 0: st.src = D3DBLEND_ONE; st.dst = D3DBLEND_ZERO; break;
                case 2: st.src = D3DBLEND_SRCALPHA; st.dst = D3DBLEND_ONE; break;
                case 5: st.src = D3DBLEND_DESTALPHA; st.dst = D3DBLEND_INVDESTALPHA; break;
                case 6: st.src = D3DBLEND_DESTALPHA; st.dst = D3DBLEND_ONE; break;
                default: st.src = D3DBLEND_SRCALPHA; st.dst = D3DBLEND_INVSRCALPHA; break; // 1, 3, 4, >= 7
            }
            st.sorted = src->ApplyAlphaSorting;
            st.renderingOrder = src->RenderingOrder;
        }
        if (!st.texture) st.texture = MissingTexture();
        textures[hash] = st;
    }

    IDirect3DTexture9* MissingTexture() {
        if (!missingTexture) missingTexture = CreateArgbTexture(device, { { 0x809A9A9Au } }, 1, 1);
        if (missingTexture) missingTexture->AddRef();   // one reference per TextureState
        return missingTexture;
    }

    // The game's own CAR shaders from SPEED2.EXE in the game folder.
    void CreateShaders() {
        std::vector<uint32_t> vs, ps;
        std::string why;
        std::wstring exe = gameFolder;
        if (!exe.empty() && exe.back() != L'\\' && exe.back() != L'/') exe += L'\\';
        exe += L"SPEED2.EXE";
        if (!ExtractCarEffectShaders(exe, vs, ps, why)) {
            shaderStatus = "fixed-function fallback: " + why;
            return;
        }
        if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.data()), &carVs)) ||
            FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &carPs)) ||
            FAILED(device->CreateVertexDeclaration(kCarDecl, &carDecl))) {
            ReleaseShaders();
            shaderStatus = "fixed-function fallback: the device refused the CAR vs_1_1/ps_1_1";
            return;
        }
        if (SUCCEEDED(device->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &blackCube, nullptr)))
            for (int f = 0; f < 6; ++f) {
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(blackCube->LockRect((D3DCUBEMAP_FACES)f, 0, &locked, nullptr, 0))) {
                    *static_cast<uint32_t*>(locked.pBits) = 0xFF000000u;
                    blackCube->UnlockRect((D3DCUBEMAP_FACES)f, 0);
                }
            }
        char text[96];
        std::snprintf(text, sizeof text, "CAR effect from SPEED2.EXE (vs_1_1 %zu bytes, ps_1_1 %zu bytes)", vs.size() * 4,
                      ps.size() * 4);
        shaderStatus = text;
    }

    void ReleaseShaders() {
        if (carVs) carVs->Release();
        if (carPs) carPs->Release();
        if (carDecl) carDecl->Release();
        if (blackCube) blackCube->Release();
        blackCube = nullptr;
        carVs = nullptr;
        carPs = nullptr;
        carDecl = nullptr;
    }

    bool Upload(IDirect3DDevice9* dev, std::wstring& error) {
        device = dev;
        for (size_t i = 0; i < items.size(); ++i) {
            UploadSolid(*items[i].Solid_);
            for (const DrawRange& r : ranges[i]) UploadTexture(r.texture);
        }
        if (solids.empty()) {
            error = L"could not upload the car's geometry";
            return false;
        }
        CreateShaders();
        gpuLoaded = true;
        return true;
    }

    void ReleaseGpu() {
        for (auto& kv : solids) {
            if (kv.second.vb) kv.second.vb->Release();
            if (kv.second.ib) kv.second.ib->Release();
        }
        solids.clear();
        for (auto& kv : textures)
            if (kv.second.texture) kv.second.texture->Release();
        textures.clear();
        if (missingTexture) missingTexture->Release();
        missingTexture = nullptr;
        ReleaseShaders();
        BeforeReset();
        UnhookReset();
        gpuLoaded = false;
        device = nullptr;
    }

    // World matrix of one item for a body pose and live wheel matrices.
    Mat4 ItemMatrix(const CarDrawItem& item, const Mat4& body, const Mat4* wheels) const {
        if (item.Wheel < 0 || !wheels) return item.Matrix * body;
        const int i = item.Wheel;
        const Vec3 f = Row(body, 0), l = Row(body, 1), u = Row(body, 2), pos = Row(body, 3);
        // The game discards the caller's Y and uses its render Y (UpdateWheelYRenderOffset).
        const Vec3 rel = Row(wheels[i], 3) - pos;
        const Vec3 local{ Dot(rel, f), layout->WheelCentre(i).y, Dot(rel, u) };
        const Vec3 centre = pos + f * local.x + l * local.y + u * local.z;
        if (!item.Brake) {
            const Mat4 w = FromRows(Row(wheels[i], 0), Row(wheels[i], 1), Row(wheels[i], 2), centre);
            return item.Local * w;
        }
        // Brakes keep steering but not the spin: rebuild the frame around the wheel's axle.
        const Vec3 axle = Normalize(Row(wheels[i], 1));
        const Vec3 up = Normalize(u - axle * Dot(u, axle));
        const Vec3 fwd = Cross(axle, up);
        return item.Local * FromRows(fwd, axle, up, centre);
    }
};

CarModel::CarModel() : impl_(std::make_unique<Impl>()) {}
CarModel::~CarModel() { Release(); }

bool CarModel::Load(IDirect3DDevice9* device, const std::wstring& gameFolder, const CarTypeInfoU2& car, std::wstring& error) {
    Release();
    if (!impl_->LoadCpu(gameFolder, car, error)) {
        Release();
        return false;
    }
    if (device && !impl_->Upload(device, error)) {
        Release();
        return false;
    }
    return true;
}

void CarModel::Release() {
    impl_->ReleaseGpu();
    impl_ = std::make_unique<Impl>();
}

bool CarModel::Loaded() const { return impl_->cpuLoaded; }

void CarModel::SetLod(int lod) {
    Impl& m = *impl_;
    lod = std::clamp(lod, 0, 3);
    if (!m.cpuLoaded || lod == m.lod) return;
    m.lod = lod;
    m.BuildItems();
    if (m.device)
        for (size_t i = 0; i < m.items.size(); ++i) {
            m.UploadSolid(*m.items[i].Solid_);
            for (const DrawRange& r : m.ranges[i]) m.UploadTexture(r.texture);
        }
}

Vec3 CarModel::WheelCentre(int i) const { return impl_->layout ? impl_->layout->WheelCentre(i) : Vec3{}; }
float CarModel::WheelRadius(int i) const { return impl_->carType ? impl_->carType->Wheels[i].Radius : 0.0f; }
float CarModel::RestBodyZ() const { return impl_->layout ? impl_->layout->RestBodyZ() : 0.0f; }

Mat4 CarModel::WheelMatrix(int i, const Mat4& body, float steer, float spin, float lift) const {
    // Spin about the axle (+Y), then steer about up (+Z), at the wheel centre, then the body.
    const float cs = std::cos(spin), ss = std::sin(spin), ct = std::cos(steer), st = std::sin(steer);
    Mat4 spinM = Mat4::Identity();
    spinM.m[0][0] = cs;
    spinM.m[0][2] = -ss;
    spinM.m[2][0] = ss;
    spinM.m[2][2] = cs;
    Mat4 steerM = Mat4::Identity();
    steerM.m[0][0] = ct;
    steerM.m[0][1] = st;
    steerM.m[1][0] = -st;
    steerM.m[1][1] = ct;
    Vec3 c = WheelCentre(i);
    c.z += lift;
    return spinM * steerM * Mat4::Translation(c) * body;
}

namespace {

Mat4 GetTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE type) {
    D3DMATRIX d{};
    device->GetTransform(type, &d);
    Mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.m[r][c] = d.m[r][c];
    return m;
}

DWORD Opposite(DWORD cull) { return cull == D3DCULL_CW ? D3DCULL_CCW : cull == D3DCULL_CCW ? D3DCULL_CW : cull; }

} // namespace

void CarModel::SetEnvironmentMapMode(int mode) { impl_->environment.SetMode(mode); }

std::string CarModel::ShaderStatus() const { return impl_->shaderStatus; }

void CarModel::Draw(IDirect3DDevice9* device, WorldRenderer& renderer, const Mat4& body, const Mat4 wheels[4]) {
    Impl& m = *impl_;
    if (!m.gpuLoaded || !device || device != m.device) return;

    m.HookReset(renderer.Context());
    if (!m.saved && FAILED(device->CreateStateBlock(D3DSBT_ALL, &m.saved))) m.saved = nullptr;
    if (m.saved) m.saved->Capture();

    // The world's back-face cull (D3DCULL_CW with the editor's right-handed view; NONE when the
    // viewport's back-face culling is off) stands for the game's CULL_CW = "cull back" (inferred).
    DWORD worldCull = D3DCULL_CW;
    device->GetRenderState(D3DRS_CULLMODE, &worldCull);
    const bool wireframe = renderer.Mode() == RenderMode::Wireframe;
    const bool shaders = m.carVs && m.carPs && m.carDecl;

    // The world's camera, as WorldRenderer::Render set it.
    const Mat4 view = GetTransform(device, D3DTS_VIEW);
    const Mat4 projection = GetTransform(device, D3DTS_PROJECTION);
    const Mat4 viewProjection = view * projection;
    const Mat4 cameraToWorld = InverseAffine(view);
    const Vec3 eye{ cameraToWorld.m[3][0], cameraToWorld.m[3][1], cameraToWorld.m[3][2] };
    // Camera axes in world (= game) space: the columns of the view's rotation. The game reads
    // them as right, down, forward (transpose3x3 of eCamera's matrix, car-shading.md 4.2).
    const Vec3 right{ view.m[0][0], view.m[1][0], view.m[2][0] };
    const Vec3 down{ -view.m[0][1], -view.m[1][1], -view.m[2][1] };
    const Vec3 forward{ -view.m[0][2], -view.m[1][2], -view.m[2][2] };

    // The race's dynamic environment cube around the car, drawn before anything of the car.
    IDirect3DBaseTexture9* cube = nullptr;
    if (shaders) {
        cube = m.environment.Update(device, renderer, Row(body, 3), forward);
        if (!cube) cube = m.blackCube;
        // RenderWorldOnly set its own camera: put the world's back for the WORLD-effect meshes.
        const D3DMATRIX d3dView = ToD3D(view), d3dProjection = ToD3D(projection);
        device->SetTransform(D3DTS_VIEW, &d3dView);
        device->SetTransform(D3DTS_PROJECTION, &d3dProjection);
    }

    // elSetupLights (0x48ED60) with ShaperLightsCarsInGame, world space, once per car and frame.
    // World point/spot lights are not added: the editor does not load the track light packs.
    float shWorld[CarLightingU2::NumCoeffs][4];
    CarLightingU2::Compute(ShaperLightRigU2::CarsInGame(), right, down, forward, shWorld);

    // Draw order (sub_5CEB30 / sub_5CF420): buckets in address order -- not alpha-sorted before
    // alpha-sorted, then RenderingOrder 0..7, then effect id (WORLD 0 before CAR 5) -- and inside
    // a bucket one node per (texture, effect) in order of first submission, each node's items in
    // submission order. No depth sorting.
    struct Job {
        size_t item;
        const DrawRange* range;
        const TextureState* state;
        int bucket, node;
    };
    std::vector<Job> jobs;
    std::map<std::pair<uint32_t, int>, int> nodes;
    static const TextureState missing{};
    for (size_t i = 0; i < m.items.size(); ++i)
        for (const DrawRange& r : m.ranges[i]) {
            const auto t = m.textures.find(r.texture);
            const TextureState* st = t != m.textures.end() ? &t->second : &missing;
            const int order = std::clamp(st->renderingOrder, 0, 7);
            const int bucket = ((st->sorted ? 1 : 0) * 8 + order) * 23 + r.effect;
            const int node = nodes.emplace(std::make_pair(r.texture, r.effect), (int)nodes.size()).first->second;
            jobs.push_back({ i, &r, st, bucket, node });
        }
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
        return a.bucket != b.bucket ? a.bucket < b.bucket : a.node < b.node;
    });

    // States every draw shares (pass 0 of "car": FOGENABLE 0, ZFUNC LESSEQUAL, TEXCOORDINDEX 0..2).
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_FILLMODE, wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    // ALPHAFUNC is not traced in the game; the alpha test keeps alpha >= ref (as the Unity port).
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF9A9A9A);   // missing texture (fallback path)
    for (DWORD s = 0; s < 3; ++s) {
        device->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
        device->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    if (shaders) {
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        int boundEffect = -1;
        size_t boundItem = (size_t)-1;
        bool mirrored = false;
        for (const Job& job : jobs) {
            const CarDrawItem& item = m.items[job.item];
            const auto g = m.solids.find(item.Solid_);
            if (g == m.solids.end()) continue;
            const DrawRange& range = *job.range;
            const TextureState& st = *job.state;

            if (range.effect != boundEffect) {
                boundEffect = range.effect;
                if (boundEffect == kEffectCar) {
                    device->SetVertexDeclaration(m.carDecl);
                    device->SetVertexShader(m.carVs);
                    device->SetPixelShader(m.carPs);
                    device->SetRenderState(D3DRS_COLORWRITEENABLE, 7);   // RGB: pass 0 keeps alpha
                    // envmap_sampler: CLAMP, LINEAR; samp (diffuse): WRAP, mip LINEAR, min/mag
                    // from the filtering option (linear here).
                    device->SetTexture(0, cube);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
                    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                    device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                } else {
                    // WORLD effect (meshes without flag 0x80): out = 2 * texture * vertex colour,
                    // alpha = texture * vertex colour; the fixed-function stage does exactly that.
                    device->SetVertexShader(nullptr);
                    device->SetPixelShader(nullptr);
                    device->SetFVF(kFvf);
                    device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
                    device->SetTexture(1, nullptr);
                    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
                    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                }
            }

            if (job.item != boundItem) {
                boundItem = job.item;
                const Mat4 world = m.ItemMatrix(item, body, wheels);
                mirrored = Determinant3x3(world) < 0.0f;
                device->SetStreamSource(0, g->second.vb, 0, sizeof(Vertex));
                device->SetIndices(g->second.ib);
                const D3DMATRIX d3dWorld = ToD3D(world);
                device->SetTransform(D3DTS_WORLD, &d3dWorld);
                // sub_5BDDE0 + sub_5BD140, per item, in the VS's registers: c0..c9 HarmonicCoeff
                // (part space, elCloneLightContext), c10..c13 WorldViewProj (as columns: the VS
                // does dp4 oPos.x, v0, c10), c14..c16 the reflection's transform, c17 LocalEyePos.
                float c[18][4];
                CarLightingU2::RotateToLocal(shWorld, world, c);
                const Mat4 wvp = world * viewProjection;
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) c[10 + r][k] = wvp.m[k][r];
                // The game sends WorldView (reflection in view space, camera-aligned cube); the
                // editor's cube is world aligned, so the part's own rotation: oT0 = R * l2w.
                for (int r = 0; r < 3; ++r) {
                    c[14 + r][0] = world.m[0][r];
                    c[14 + r][1] = world.m[1][r];
                    c[14 + r][2] = world.m[2][r];
                    c[14 + r][3] = 0.0f;
                }
                CarLightingU2::LocalEyePos(world, eye, false, c[17]);
                device->SetVertexShaderConstantF(0, &c[0][0], 18);
            }

            device->SetTexture(boundEffect == kEffectCar ? 1 : 0, st.texture ? st.texture : m.missingTexture);
            if (boundEffect == kEffectCar) {
                // sub_5BD4A0 (SetLightMaterial), per strip: c18..c23 and the PS's c0, c1.
                const LightMaterialU2::ShaderParams& p = range.constants;
                float v[6][4];
                std::memcpy(v[0], p.DiffuseMin, sizeof v[0]);
                std::memcpy(v[1], p.DiffuseRange, sizeof v[1]);
                std::memcpy(v[2], p.SpecularMin, sizeof v[2]);
                std::memcpy(v[3], p.SpecularRange, sizeof v[3]);
                v[4][0] = p.SpecularPower;
                v[4][1] = v[4][2] = v[4][3] = 0.0f;
                v[5][0] = p.EnvmapPower;
                v[5][1] = v[5][2] = v[5][3] = 0.0f;
                device->SetVertexShaderConstantF(18, &v[0][0], 6);
                float e[2][4];
                std::memcpy(e[0], p.EnvmapMin, sizeof e[0]);
                std::memcpy(e[1], p.EnvmapRange, sizeof e[1]);
                device->SetPixelShaderConstantF(0, &e[0][0], 2);
            }

            // BlendState (sub_5BCC30): alpha test with PUNCHTHRU (ref 0x5F, on the output alpha),
            // blend from AlphaBlendType; CAR: opaque = CULL NONE + ZWRITE, blended = CULL CW (back)
            // + no ZWRITE. WORLD: its .fxo culls nothing, ZWRITE = opaque.
            device->SetRenderState(D3DRS_ALPHATESTENABLE, st.punchThrough ? TRUE : FALSE);
            device->SetRenderState(D3DRS_ALPHAREF, st.punchThrough ? 0x5F : 0);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, st.blended ? TRUE : FALSE);
            if (st.blended) {
                device->SetRenderState(D3DRS_SRCBLEND, st.src);
                device->SetRenderState(D3DRS_DESTBLEND, st.dst);
            }
            device->SetRenderState(D3DRS_ZWRITEENABLE, st.blended ? FALSE : TRUE);
            DWORD cull = D3DCULL_NONE;
            if (boundEffect == kEffectCar && st.blended) cull = mirrored ? Opposite(worldCull) : worldCull;
            device->SetRenderState(D3DRS_CULLMODE, cull);
            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, (UINT)g->second.vertexCount, (UINT)range.firstIndex,
                                         (UINT)range.primitiveCount);
        }
        if (m.saved) m.saved->Apply();
        return;
    }

    // ---- Fixed-function fallback (no CAR shaders): one directional light standing in for the rig.
    device->SetFVF(kFvf);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);   // the wheel width scale
    device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(110, 110, 118));
    device->SetRenderState(D3DRS_COLORVERTEX, FALSE);
    device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    device->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    device->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
    D3DLIGHT9 light{};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = { 0.75f, 0.74f, 0.70f, 1.0f };
    light.Specular = { 1.0f, 1.0f, 1.0f, 1.0f };
    const Vec3 dir = Normalize(Vec3{ 0.35f, 0.45f, -0.82f });
    light.Direction = { dir.x, dir.y, dir.z };
    device->SetLight(0, &light);
    device->LightEnable(0, TRUE);
    for (DWORD i = 1; i < 8; ++i) device->LightEnable(i, FALSE);
    D3DMATERIAL9 plain{}, glossy{};
    plain.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
    plain.Ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
    glossy = plain;
    glossy.Specular = { 0.55f, 0.55f, 0.55f, 1.0f };
    glossy.Power = 24.0f;

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

    size_t boundItem = (size_t)-1;
    bool mirrored = false;
    for (const Job& job : jobs) {
        const CarDrawItem& item = m.items[job.item];
        const auto g = m.solids.find(item.Solid_);
        if (g == m.solids.end()) continue;
        if (job.item != boundItem) {
            const Mat4 world = m.ItemMatrix(item, body, wheels);
            const D3DMATRIX d3d = ToD3D(world);
            device->SetTransform(D3DTS_WORLD, &d3d);
            device->SetStreamSource(0, g->second.vb, 0, sizeof(Vertex));
            device->SetIndices(g->second.ib);
            mirrored = Determinant3x3(world) < 0.0f;
            boundItem = job.item;
        }
        const TextureState& st = *job.state;
        device->SetTexture(0, st.texture);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, st.texture ? D3DTA_TEXTURE : D3DTA_TFACTOR);
        const bool alphaFromTexture = st.texture && (st.blended || st.punchThrough);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, alphaFromTexture ? D3DTA_TEXTURE : D3DTA_TFACTOR);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, st.blended ? TRUE : FALSE);
        if (st.blended) {
            device->SetRenderState(D3DRS_SRCBLEND, st.src);
            device->SetRenderState(D3DRS_DESTBLEND, st.dst);
        }
        device->SetRenderState(D3DRS_ZWRITEENABLE, st.blended ? FALSE : TRUE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, alphaFromTexture ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ALPHAREF, st.punchThrough && !st.blended ? 0x5F : 1);
        // CAR effect: opaque parts are double-sided; blended parts cull their back faces.
        DWORD cull = D3DCULL_NONE;
        if (st.blended) cull = mirrored ? Opposite(worldCull) : worldCull;
        device->SetRenderState(D3DRS_CULLMODE, cull);
        device->SetRenderState(D3DRS_SPECULARENABLE, job.range->specular ? TRUE : FALSE);
        device->SetMaterial(job.range->specular ? &glossy : &plain);
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, (UINT)g->second.vertexCount, (UINT)job.range->firstIndex,
                                     (UINT)job.range->primitiveCount);
    }

    if (m.saved) m.saved->Apply();
}

std::string CarModel::Describe() const {
    const Impl& m = *impl_;
    if (!m.cpuLoaded) return "not loaded\n";
    std::string out;
    char line[512];
    std::snprintf(line, sizeof line, "%s: %zu parts, LOD %c, composite skin %dx%d, rest body z %.4f\n",
                  m.carType->CollectionName.c_str(), m.items.size(), 'A' + m.lod, m.compositeWidth, m.compositeHeight,
                  m.layout->RestBodyZ());
    out += line;
    out += "  shading: " + (m.shaderStatus.empty() ? std::string("(not uploaded)") : m.shaderStatus) + ", " +
           std::to_string(m.lights.Count()) + " light materials\n";
    if (!m.compositeSkin.empty()) {
        std::map<uint32_t, int> counts;
        for (uint32_t c : m.compositeSkin) counts[c]++;
        const auto top = std::max_element(counts.begin(), counts.end(),
                                          [](const auto& a, const auto& b) { return a.second < b.second; });
        std::snprintf(line, sizeof line, "  composite skin: most common texel %08X (%d texels)\n", top->first, top->second);
        out += line;
    }
    for (int i = 0; i < 4; ++i) {
        const Vec3 c = m.layout->WheelCentre(i);
        std::snprintf(line, sizeof line, "  wheel %d centre (%.4f, %.4f, %.4f) radius %.4f width %.4f\n", i, c.x, c.y, c.z,
                      m.carType->Wheels[i].Radius, m.layout->WheelWidth(i));
        out += line;
    }
    for (size_t i = 0; i < m.items.size(); ++i) {
        const CarDrawItem& it = m.items[i];
        std::snprintf(line, sizeof line, "  %-16s [%d] slot %2d %-34s tris %5d%s", it.Label.c_str(), it.Row, it.Slot,
                      it.Solid_->Name.c_str(), (int)it.Solid_->Indices.size() / 3,
                      it.Wheel >= 0 ? (" wheel " + std::to_string(it.Wheel)).c_str() : "");
        out += line;
        std::map<std::string, int> names;
        for (const DrawRange& r : m.ranges[i]) {
            std::string name = m.TextureName(r.texture);
            if (const Texture* t = m.FindTexture(r.texture)) {
                // (format, alpha usage, alpha blend, rendering order)
                char info[48];
                std::snprintf(info, sizeof info, "(f%d u%d b%d o%d)", (int)t->Format, (int)t->AlphaUsage_,
                              (int)t->AlphaBlend_, (int)t->RenderingOrder);
                name += info;
            }
            // effect and light material
            name += r.effect == kEffectCar ? "/CAR:" : "/WORLD:";
            name += m.lights.Get(r.lightMaterial).Name;
            names[name]++;
        }
        out += "  tex:";
        for (const auto& kv : names) out += " " + kv.first;
        out += "\n";
    }
    return out;
}

bool CarModel::DumpObj(const std::wstring& path, const Mat4* body, const Mat4* wheels) const {
    const Impl& m = *impl_;
    if (!m.cpuLoaded) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return false;
    std::fprintf(f, "# %s, game space (X forward, Y left, Z up)\n", m.carType->CollectionName.c_str());
    // The rest pose comes from the reference layout (body at RestBodyZ + bias, FE wheel pose).
    const std::vector<CarDrawItem> rest = m.layout->Build(m.lod, m.lod, true);
    const std::vector<CarDrawItem>& items = body ? m.items : rest;
    size_t base = 1;
    for (size_t i = 0; i < items.size(); ++i) {
        const CarDrawItem& it = items[i];
        const Solid& s = *it.Solid_;
        const Mat4 world = body ? m.ItemMatrix(it, *body, wheels) : it.Matrix;
        const bool flip = Determinant3x3(world) < 0.0f;
        std::fprintf(f, "o %s_%d_%s\n", it.Label.c_str(), it.Row, s.Name.c_str());
        for (int v = 0; v < s.VertexCount; ++v) {
            const Vec3 p = world.TransformPoint({ s.Positions[v * 3], s.Positions[v * 3 + 1], s.Positions[v * 3 + 2] });
            std::fprintf(f, "v %.5f %.5f %.5f\n", p.x, p.y, p.z);
        }
        for (int v = 0; v < s.VertexCount; ++v) std::fprintf(f, "vt %.5f %.5f\n", s.UVs[v * 2], 1.0f - s.UVs[v * 2 + 1]);
        for (const SolidMaterial& mat : s.Materials) {
            uint32_t texHash = mat.TextureIndex >= 0 && mat.TextureIndex < (int)s.TextureHashes.size()
                                   ? s.TextureHashes[(size_t)mat.TextureIndex] : 0;
            texHash = m.swaps->Resolve(texHash, it.Slot, it.Slot == 27, it.LeftBrake);
            std::fprintf(f, "usemtl %s\n", m.TextureName(texHash).c_str());
            for (int k = 0; k + 2 < mat.NumIndices; k += 3) {
                const size_t a = base + s.Indices[(size_t)mat.IndexOffset + k];
                size_t b = base + s.Indices[(size_t)mat.IndexOffset + k + 1];
                size_t c = base + s.Indices[(size_t)mat.IndexOffset + k + 2];
                if (flip) std::swap(b, c);
                std::fprintf(f, "f %zu/%zu %zu/%zu %zu/%zu\n", a, a, b, b, c, c);
            }
        }
        base += (size_t)s.VertexCount;
    }
    std::fclose(f);
    return true;
}

} // namespace bb
