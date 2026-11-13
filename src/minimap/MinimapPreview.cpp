#include "minimap/MinimapPreview.h"

#include "core/Log.h"

#include <cstring>

namespace minimap {

namespace {
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
}

void Preview::Release() {
    if (buffer_) { buffer_->Release(); buffer_ = nullptr; }
    triangleCount_ = lineStart_ = lineCount_ = 0;
}

bool Preview::Upload(IDirect3DDevice9* device, const std::vector<Building>& buildings) {
    Release();
    if (!device) return false;

    std::vector<PreviewVertex> triangles, lines;
    BuildPreview(buildings, triangles, lines);
    const size_t total = triangles.size() + lines.size();
    if (triangles.empty()) return false;

    const UINT bytes = (UINT)(total * sizeof(PreviewVertex));
    if (FAILED(device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED,
                                          &buffer_, nullptr))) {
        LOG_ERROR("minimap preview: failed to create a %u byte vertex buffer", bytes);
        return false;
    }
    void* data = nullptr;
    if (FAILED(buffer_->Lock(0, 0, &data, 0))) { Release(); return false; }
    std::memcpy(data, triangles.data(), triangles.size() * sizeof(PreviewVertex));
    std::memcpy((PreviewVertex*)data + triangles.size(), lines.data(), lines.size() * sizeof(PreviewVertex));
    buffer_->Unlock();

    triangleCount_ = (u32)(triangles.size() / 3);
    lineStart_ = (u32)triangles.size();
    lineCount_ = (u32)(lines.size() / 2);
    LOG_INFO("minimap preview: %u triangles, %u edges", triangleCount_, lineCount_);
    return true;
}

void Preview::Draw(IDirect3DDevice9* device, DWORD cullMode, bool wireframe) const {
    if (!buffer_ || !triangleCount_) return;

    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetFVF(kFvf);
    device->SetStreamSource(0, buffer_, 0, sizeof(PreviewVertex));
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

    // Pushed back a little, so the edges drawn on the same faces win.
    const float bias = 0.00002f, slope = 1.0f;
    device->SetRenderState(D3DRS_DEPTHBIAS, *(const DWORD*)&bias);
    device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(const DWORD*)&slope);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, triangleCount_);
    device->SetRenderState(D3DRS_DEPTHBIAS, 0);
    device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    if (lineCount_) device->DrawPrimitive(D3DPT_LINELIST, lineStart_, lineCount_);

    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, cullMode);
}

} // namespace minimap
