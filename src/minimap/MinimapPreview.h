#pragma once

// TEMPORARY: minimap building preview. See MinimapBuildings.h.

#include "minimap/MinimapBuildings.h"

#include <d3d9.h>

namespace minimap {

// The generated buildings on the GPU, drawn the way the minimap would draw
// them: flat colour, lit from one side, with dark edges.
class Preview {
public:
    ~Preview() { Release(); }

    bool Upload(IDirect3DDevice9* device, const std::vector<Building>& buildings);
    void Release();

    bool Empty() const { return triangleCount_ == 0; }

    // Sets its own states and puts back the ones it changes.
    void Draw(IDirect3DDevice9* device, DWORD cullMode, bool wireframe) const;

private:
    IDirect3DVertexBuffer9* buffer_ = nullptr;
    u32 triangleCount_ = 0;
    u32 lineStart_ = 0;
    u32 lineCount_ = 0;
};

} // namespace minimap
