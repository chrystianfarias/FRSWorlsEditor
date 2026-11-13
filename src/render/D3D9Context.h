#pragma once

#include "core/Types.h"

#include <d3d9.h>

#include <string>

// Owns the Direct3D 9 object and device for the editor viewport.
//
// Direct3D 9 was chosen because the game itself targets it: the vertex layouts,
// the DXT texture formats and the render states in the track files map onto this
// API one to one, so what the editor draws matches what the game draws.
//
// The device is created against the viewport child window. Resizing or losing it
// (screen lock, RDP, full-screen switch) needs a reset, which invalidates every
// D3DPOOL_DEFAULT resource -- the renderer keeps its buffers in D3DPOOL_MANAGED
// so only the back buffer has to be recreated.
class D3D9Context {
public:
    ~D3D9Context();

    bool Create(void* windowHandle);
    void Destroy();

    bool IsValid() const { return device_ != nullptr; }

    IDirect3DDevice9* Device() const { return device_; }

    // Call when the viewport changes size. A zero dimension is ignored so a
    // minimised window does not destroy the swap chain.
    void Resize(u32 width, u32 height);

    // Prepares the device for a frame. Returns false when the device is lost and
    // could not be restored yet, in which case the caller should skip drawing.
    bool BeginFrame(u32 clearColor);
    void EndFrame();

    u32 Width()  const { return width_; }
    u32 Height() const { return height_; }

    // Highest vertex index a draw call may address on this device.
    u32 MaxVertexIndex() const { return maxVertexIndex_; }
    u32 MaxPrimitiveCount() const { return maxPrimitiveCount_; }

    // Writes the finished frame to a 32-bit BMP. Reading the back buffer
    // rather than the screen means the window does not have to be visible,
    // in focus, or free of anything overlapping it.
    bool SaveBackBuffer(const std::wstring& path);

    // True when the driver supports the given texture format.
    bool SupportsTextureFormat(D3DFORMAT format) const;

private:
    bool Reset();

    IDirect3D9*           d3d_    = nullptr;
    IDirect3DDevice9*     device_ = nullptr;
    D3DPRESENT_PARAMETERS present_{};
    void*                 window_ = nullptr;
    u32                   width_  = 0;
    u32                   height_ = 0;
    bool                  needsReset_ = false;
    u32                   maxVertexIndex_ = 0;
    u32                   maxPrimitiveCount_ = 0;
};
