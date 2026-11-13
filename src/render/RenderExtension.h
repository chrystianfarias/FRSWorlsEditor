#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <d3d9.h>

class Camera;
class GpuSection;
class WorldRenderer;
struct GpuObject;

// Things the viewport draws and picks that are not part of the region: an
// editor extension's own (ui/EditorExtension.h). The renderer calls each one
// at two points of the frame and asks it for picks; what it selects is a
// selection of its own, an index only it knows the meaning of
// (WorldRenderer::SelectExtension).
class RenderExtension {
public:
    virtual ~RenderExtension() = default;

    // With the world: after its blended surfaces, before the selection is
    // outlined. For things drawn as the world's own models are.
    virtual void DrawWithWorld(IDirect3DDevice9* device, WorldRenderer& renderer) {
        (void)device; (void)renderer;
    }

    // Over the world: after the collision layer. For beacons, boxes, lines.
    virtual void DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) {
        (void)device; (void)camera; (void)renderer;
    }

    // The nearest of its things under the ray: its index and the distance.
    virtual bool Pick(const Ray& ray, int& index, float& distance) const {
        (void)ray; (void)index; (void)distance;
        return false;
    }

    // How far behind the world's nearest surface a pick may be and still win.
    virtual float PickTolerance() const { return 0.5f; }

    // A model of its own, by hash, for thumbnails of the Library tab.
    virtual bool FindModel(u32 hash, const GpuSection*& gpu, const GpuObject*& object) {
        (void)hash; (void)gpu; (void)object;
        return false;
    }

    // The device's resources are going: drop whatever was uploaded.
    virtual void ReleaseGpu() {}
};

inline D3DMATRIX ToD3DMatrix(const Mat4& source) {
    D3DMATRIX out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = source.m[r][c];
    return out;
}
