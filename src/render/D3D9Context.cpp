#include "render/D3D9Context.h"

#include "core/Log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

D3D9Context::~D3D9Context() {
    Destroy();
}

bool D3D9Context::Create(void* windowHandle) {
    Destroy();

    HWND window = (HWND)windowHandle;
    RECT client{};
    ::GetClientRect(window, &client);

    width_  = (u32)std::max<LONG>(client.right - client.left, 1);
    height_ = (u32)std::max<LONG>(client.bottom - client.top, 1);
    window_ = windowHandle;

    d3d_ = ::Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) {
        LOG_ERROR("Direct3DCreate9 failed; is DirectX 9 runtime present?");
        return false;
    }

    present_                        = {};
    present_.BackBufferWidth        = width_;
    present_.BackBufferHeight       = height_;
    present_.BackBufferFormat       = D3DFMT_X8R8G8B8;
    present_.BackBufferCount        = 1;
    present_.MultiSampleType        = D3DMULTISAMPLE_NONE;
    present_.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    present_.hDeviceWindow          = window;
    present_.Windowed               = TRUE;
    present_.EnableAutoDepthStencil = TRUE;
    present_.AutoDepthStencilFormat = D3DFMT_D24S8;
    // Let the editor render as fast as the message loop allows; the viewport is
    // redrawn on demand rather than in a tight loop, so vsync only adds latency.
    present_.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;

    // Hardware vertex processing where available; the software path keeps the
    // editor usable on machines with a minimal driver.
    D3DCAPS9 caps{};
    d3d_->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    DWORD behaviour = (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)
                          ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                          : D3DCREATE_SOFTWARE_VERTEXPROCESSING;

    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                    behaviour | D3DCREATE_FPU_PRESERVE,
                                    &present_, &device_);
    if (FAILED(hr)) {
        LOG_ERROR("CreateDevice failed (hr 0x%08lX)", (unsigned long)hr);
        Destroy();
        return false;
    }

    LOG_INFO("Direct3D 9 device created: %ux%u, %s vertex processing",
             width_, height_,
             behaviour == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software");

    // MaxVertexIndex is the highest vertex a draw may address. When it is
    // 65535, merging a section into one large buffer and reaching past it with
    // BaseVertexIndex silently returns garbage vertices.
    maxVertexIndex_   = caps.MaxVertexIndex;
    maxPrimitiveCount_ = caps.MaxPrimitiveCount;
    LOG_INFO("device limits: MaxVertexIndex %u, MaxPrimitiveCount %u",
             caps.MaxVertexIndex, caps.MaxPrimitiveCount);

    // The depth buffer that was actually created. A silent fall back to 16 bits
    // would make every surface that lies on another one flicker and interleave,
    // which is easy to mistake for duplicated geometry.
    IDirect3DSurface9* depth = nullptr;
    if (SUCCEEDED(device_->GetDepthStencilSurface(&depth)) && depth) {
        D3DSURFACE_DESC desc{};
        depth->GetDesc(&desc);
        LOG_INFO("depth buffer: format %u (D3DFMT_D24S8 is %u), %ux%u, samples %u",
                 (unsigned)desc.Format, (unsigned)D3DFMT_D24S8,
                 desc.Width, desc.Height, (unsigned)desc.MultiSampleType);
        depth->Release();
    } else {
        LOG_ERROR("no depth buffer was created; everything will draw in submission order");
    }
    return true;
}

void D3D9Context::Destroy() {
    if (device_) { device_->Release(); device_ = nullptr; }
    if (d3d_)    { d3d_->Release();    d3d_    = nullptr; }
    window_ = nullptr;
    width_ = height_ = 0;
    needsReset_ = false;
}

void D3D9Context::Resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    width_  = width;
    height_ = height;
    present_.BackBufferWidth  = width;
    present_.BackBufferHeight = height;
    needsReset_ = true;
}

bool D3D9Context::Reset() {
    if (!device_) return false;

    const HRESULT hr = device_->Reset(&present_);
    if (FAILED(hr)) {
        LOG_WARN("device reset failed (hr 0x%08lX)", (unsigned long)hr);
        return false;
    }

    needsReset_ = false;
    return true;
}

bool D3D9Context::BeginFrame(u32 clearColor) {
    if (!device_) return false;

    const HRESULT cooperative = device_->TestCooperativeLevel();
    if (cooperative == D3DERR_DEVICELOST)
        return false;                       // another app owns the device; try again next frame
    if (cooperative == D3DERR_DEVICENOTRESET)
        needsReset_ = true;

    if (needsReset_ && !Reset())
        return false;

    device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                   (D3DCOLOR)clearColor, 1.0f, 0);

    return SUCCEEDED(device_->BeginScene());
}

void D3D9Context::EndFrame() {
    if (!device_) return;

    device_->EndScene();
    if (device_->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST)
        needsReset_ = true;
}

bool D3D9Context::SaveBackBuffer(const std::wstring& path) {
    if (!device_) return false;

    IDirect3DSurface9* back = nullptr;
    if (FAILED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back)
        return false;

    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);

    IDirect3DSurface9* copy = nullptr;
    HRESULT hr = device_->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                      D3DPOOL_SYSTEMMEM, &copy, nullptr);
    if (SUCCEEDED(hr) && copy)
        hr = device_->GetRenderTargetData(back, copy);
    back->Release();

    if (FAILED(hr) || !copy) {
        if (copy) copy->Release();
        LOG_ERROR("could not read the back buffer (hr 0x%08lX)", (unsigned long)hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    bool written = false;
    if (SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        FILE* file = nullptr;
        if (::_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
            const DWORD imageBytes = desc.Width * desc.Height * 4;

            BITMAPFILEHEADER header{};
            header.bfType = 0x4D42;
            header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + imageBytes;

            BITMAPINFOHEADER info{};
            info.biSize = sizeof(info);
            info.biWidth = (LONG)desc.Width;
            info.biHeight = (LONG)desc.Height;   // positive: rows run bottom-up
            info.biPlanes = 1;
            info.biBitCount = 32;
            info.biCompression = BI_RGB;
            info.biSizeImage = imageBytes;

            std::fwrite(&header, sizeof(header), 1, file);
            std::fwrite(&info, sizeof(info), 1, file);
            for (int y = (int)desc.Height - 1; y >= 0; --y)
                std::fwrite((const u8*)locked.pBits + (size_t)y * locked.Pitch, 4,
                            desc.Width, file);
            std::fclose(file);
            written = true;
        }
        copy->UnlockRect();
    }

    copy->Release();
    return written;
}

bool D3D9Context::SupportsTextureFormat(D3DFORMAT format) const {
    if (!d3d_) return false;
    return SUCCEEDED(d3d_->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                             present_.BackBufferFormat, 0,
                                             D3DRTYPE_TEXTURE, format));
}
