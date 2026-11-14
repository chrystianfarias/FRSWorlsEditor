#include "drive/CarShader.h"

#include "render/Camera.h"
#include "render/WorldRenderer.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace bb {

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

// One shader token stream starting at `at` (a version token): its length in dwords, comments
// skipped by their own length so a CTAB never ends it early. 0 when it runs off the blob.
size_t ShaderLength(const uint32_t* t, size_t count, size_t at) {
    size_t i = at + 1;
    while (i < count) {
        const uint32_t token = t[i];
        if ((token & 0xFFFFu) == 0xFFFEu) {   // comment: length in dwords in the high word
            i += 1 + (token >> 16);
            continue;
        }
        if (token == kEndToken) return i + 1 - at;
        ++i;
    }
    return 0;
}

} // namespace

bool ExtractCarEffectShaders(const std::wstring& exePath, std::vector<uint32_t>& vertexShader,
                             std::vector<uint32_t>& pixelShader, std::string& error) {
    vertexShader.clear();
    pixelShader.clear();
    HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        error = "cannot open SPEED2.EXE";
        return false;
    }
    bool ok = false;
    // IDI_CAR_FX: RT_RCDATA, rva 0x4D6C60, 18032 bytes in v1.2 (car-shading.md 1).
    if (HRSRC info = ::FindResourceW(module, L"IDI_CAR_FX", MAKEINTRESOURCEW(10) /* RT_RCDATA */)) {
        const DWORD size = ::SizeofResource(module, info);
        HGLOBAL handle = ::LoadResource(module, info);
        const uint8_t* data = handle ? static_cast<const uint8_t*>(::LockResource(handle)) : nullptr;
        if (data && size >= 8) {
            // The effect stores each shader as a dword byte count followed by the token stream,
            // dword-aligned within the resource.
            std::vector<uint32_t> tokens(size / 4);
            std::memcpy(tokens.data(), data, tokens.size() * 4);
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (tokens[i] != kVs11 && tokens[i] != kPs11) continue;
                const size_t n = ShaderLength(tokens.data(), tokens.size(), i);
                if (n == 0) continue;
                if (tokens[i - 1] != n * 4) continue;   // not the size the effect records
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(tokens.data() + i);
                // Pass 0's shaders, by their constant tables (pass 1 is the DOF mask).
                if (tokens[i] == kVs11 && Contains(bytes, n * 4, "HarmonicCoeff"))
                    vertexShader.assign(tokens.begin() + (std::ptrdiff_t)i, tokens.begin() + (std::ptrdiff_t)(i + n));
                else if (tokens[i] == kPs11 && Contains(bytes, n * 4, "EnvmapMin"))
                    pixelShader.assign(tokens.begin() + (std::ptrdiff_t)i, tokens.begin() + (std::ptrdiff_t)(i + n));
                i += n - 1;
            }
            ok = !vertexShader.empty() && !pixelShader.empty();
            if (!ok) error = "IDI_CAR_FX holds no vs_1_1/ps_1_1 pair for technique car";
        } else {
            error = "cannot load IDI_CAR_FX";
        }
    } else {
        error = "SPEED2.EXE has no IDI_CAR_FX resource";
    }
    ::FreeLibrary(module);
    return ok;
}

// ---------------------------------------------------------------------------
// Environment map
// ---------------------------------------------------------------------------

namespace {

// D3D cube face f: texel (s, t) in [-1, 1] (s right, t down) looks along
//   +X (1, -t, -s)  -X (-1, -t, s)  +Y (s, 1, t)  -Y (s, -1, -t)  +Z (s, -t, 1)  -Z (-s, -t, -1).
// With the editor's right-handed views (x right, y up, looking down -z) that is: forward F,
// screen up U = -t, screen right R = s. These views mirror the world (R x U = -F), which
// Camera::SetExplicitView notices and WorldRenderer answers with the opposite cull mode.
struct FaceBasis { Vec3 f, u, r; };
const FaceBasis kFaces[6] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } },    // D3DCUBEMAP_FACE_POSITIVE_X
    { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },    // NEGATIVE_X
    { { 0, 1, 0 }, { 0, 0, -1 }, { 1, 0, 0 } },    // POSITIVE_Y
    { { 0, -1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } },    // NEGATIVE_Y
    { { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 } },     // POSITIVE_Z (game up: the sky)
    { { 0, 0, -1 }, { 0, 1, 0 }, { -1, 0, 0 } },   // NEGATIVE_Z
};

Mat4 FaceView(int face, const Vec3& eye) {
    const FaceBasis& b = kFaces[face];
    Mat4 v = Mat4::Identity();
    v.m[0][0] = b.r.x; v.m[0][1] = b.u.x; v.m[0][2] = -b.f.x;
    v.m[1][0] = b.r.y; v.m[1][1] = b.u.y; v.m[1][2] = -b.f.y;
    v.m[2][0] = b.r.z; v.m[2][1] = b.u.z; v.m[2][2] = -b.f.z;
    v.m[3][0] = -Dot(b.r, eye);
    v.m[3][1] = -Dot(b.u, eye);
    v.m[3][2] = Dot(b.f, eye);
    return v;
}

// The face a direction mostly points at.
int DominantFace(const Vec3& d) {
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) return d.x >= 0 ? 0 : 1;
    if (ay >= az) return d.y >= 0 ? 2 : 3;
    return d.z >= 0 ? 4 : 5;
}

} // namespace

bool CarEnvironmentMap::Create(IDirect3DDevice9* device) {
    if (cube_) return true;
    if (createFailed_) return false;
    if (FAILED(device->CreateCubeTexture(Size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube_,
                                         nullptr))) {
        cube_ = nullptr;
        createFailed_ = true;
        return false;
    }
    if (FAILED(device->CreateDepthStencilSurface(Size, Size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth_,
                                                 nullptr)) &&
        FAILED(device->CreateDepthStencilSurface(Size, Size, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &depth_,
                                                 nullptr))) {
        depth_ = nullptr;
        cube_->Release();
        cube_ = nullptr;
        createFailed_ = true;
        return false;
    }
    filled_ = false;
    return true;
}

void CarEnvironmentMap::Release() {
    if (cube_) cube_->Release();
    if (depth_) depth_->Release();
    cube_ = nullptr;
    depth_ = nullptr;
    filled_ = false;
    createFailed_ = false;
}

IDirect3DCubeTexture9* CarEnvironmentMap::Update(IDirect3DDevice9* device, WorldRenderer& renderer,
                                                 const Vec3& carPosition, const Vec3& cameraForward) {
    if (!device || !Create(device)) return nullptr;
    const Vec3 eye = carPosition + Vec3{ 0.0f, 0.0f, EyeHeight };

    // Which faces this frame (eDisplayFrame draws the views whose flag the update mode set).
    unsigned mask = 0;
    const Vec3 moved = eye - lastEye_;
    if (!filled_ || Dot(moved, moved) > 20.0f * 20.0f) {
        mask = 0x3F;
    } else {
        switch (mode_) {
            case 0: mask = 0; break;
            case 1: mask = 1u << (frame_ % 6); break;
            case 2: {
                const int ahead = DominantFace(cameraForward);
                int other = (int)(frame_ % 5);
                if (other >= ahead) ++other;
                mask = (1u << ahead) | (1u << other);
                break;
            }
            default: mask = 0x3F; break;
        }
    }
    ++frame_;
    if (mode_ == 0 && filled_) return cube_;

    IDirect3DSurface9* oldTarget = nullptr;
    IDirect3DSurface9* oldDepth = nullptr;
    D3DVIEWPORT9 oldViewport{};
    device->GetRenderTarget(0, &oldTarget);
    device->GetDepthStencilSurface(&oldDepth);
    device->GetViewport(&oldViewport);
    // The cube must not be bound as a texture while it is drawn into.
    for (DWORD s = 0; s < 4; ++s) device->SetTexture(s, nullptr);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);

    const Mat4 projection = PerspectiveRH(kPi * 0.5f, 1.0f, Near, Far);
    for (int face = 0; face < 6; ++face) {
        if (!(mask & (1u << face))) continue;
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(cube_->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &surface)) || !surface) continue;
        device->SetRenderTarget(0, surface);
        device->SetDepthStencilSurface(depth_);
        const D3DVIEWPORT9 viewport{ 0, 0, (DWORD)Size, (DWORD)Size, 0.0f, 1.0f };
        device->SetViewport(&viewport);
        // Mode 0: the cube stays black (no reflection), as with the option off.
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (mode_ != 0) {
            Camera camera;
            camera.SetClipPlanes(Near, Far);
            camera.SetExplicitView(FaceView(face, eye), projection);
            camera.SetPosition(eye);
            renderer.RenderWorldOnly(camera);
        }
        surface->Release();
    }
    filled_ = true;
    lastEye_ = eye;

    device->SetRenderTarget(0, oldTarget);
    device->SetDepthStencilSurface(oldDepth);
    device->SetViewport(&oldViewport);
    if (oldTarget) oldTarget->Release();
    if (oldDepth) oldDepth->Release();
    return cube_;
}

} // namespace bb
