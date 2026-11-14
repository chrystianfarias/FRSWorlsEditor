#pragma once

// The player's car as NFSU2 draws it, for the editor's test drive: the stock parts of a
// CarTypeInfo (RideInfo::SetStockParts), placed as CarRenderInfo::Render places them
// (drive/cars/CarRenderLayoutU2), textured through the car's replacement tables
// (drive/cars/CarTexturesU2) with the composited stock paint (CarSkinCompositorU2).
//
// Coordinates are game space (X forward, Y left, Z up), row vectors, as src/core/Math.h.

#include "core/Math.h"

#include <d3d9.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WorldRenderer;

namespace bb {

class CarTypeInfoU2;
class CarRenderLayoutU2;
class RideInfoU2;
class SolidList;
class Texture;

class CarModel {
public:
    CarModel();
    ~CarModel();
    CarModel(const CarModel&) = delete;
    CarModel& operator=(const CarModel&) = delete;

    // Loads the stock car (stock parts, stock paint) and uploads it. device may be nullptr to
    // load the CPU side only (DumpObj / Describe still work; Draw does nothing).
    bool Load(IDirect3DDevice9* device, const std::wstring& gameFolder, const CarTypeInfoU2& car, std::wstring& error);
    void Release();                 // device lost / editor closing / car changed: drops everything
    bool Loaded() const;

    // Called from RenderExtension::DrawWithWorld(device, renderer): view/projection are the world's.
    // body: world matrix of the car model's origin (CarState geometry position), rows forward, left,
    //       up, position.
    // wheels[i]: world matrix of wheel i (0 FL, 1 FR, 2 RR, 3 RL) at the wheel centre, rows
    //       forward/left/up/centre, spin and steering included. Only the centre's forward and up
    //       components (in the body frame) are used: the lateral one is the game's render Y
    //       (WheelCentre(i).y), as Render discards the caller's Y. Left wheels are turned 180
    //       degrees about Z as the game does; pass the same kind of matrix for all four.
    // Restores every device state it changes (state block), and the render target, depth buffer
    // and viewport the environment cube borrows. Called inside WorldRenderer::Render, it may call
    // back into renderer.RenderWorldOnly (which skips the extensions, so no recursion).
    void Draw(IDirect3DDevice9* device, WorldRenderer& renderer, const Mat4& body, const Mat4 wheels[4]);

    // ---- Extras
    // Body-space centre of wheel i's tyre: (X, render Y -/+ width/2, RideHeight). With the body at
    // its rest pose the tyre touches z = -RestBodyZ().
    Vec3 WheelCentre(int i) const;
    float WheelRadius(int i) const;
    // mean(radius - RideHeight): height of the model origin above flat ground at rest.
    float RestBodyZ() const;
    // Convenience: wheel i's matrix for a body pose, steer (radians about up, positive = left),
    // spin (radians about the axle) and the wheel centre's up offset from rest (suspension).
    Mat4 WheelMatrix(int i, const Mat4& body, float steer, float spin, float lift = 0.0f) const;

    // Car LOD 0..3 (A..D); default 0.
    void SetLod(int lod);

    // Shading. Draw uses the game's own CAR effect (vs_1_1 + ps_1_1 from SPEED2.EXE, technique
    // "car" pass 0) with the in-game rig (ShaperLightsCarsInGame, camera-relative: the camera is
    // the world's, read from the device's VIEW transform), per-part harmonics and clamped eye,
    // per-strip light materials, and the race's dynamic 128x128 environment cube drawn around
    // the car through WorldRenderer::RenderWorldOnly. Without the shaders (no exe, device
    // refuses vs_1_1/ps_1_1) it falls back to a fixed-function approximation.
    // Environment cube update (g_CarEnvironmentMapEnable): 0 off, 1 one face per frame
    // (default), 2 two faces, 3 all six every frame.
    void SetEnvironmentMapMode(int mode);
    // Which path Draw takes, for logs.
    std::string ShaderStatus() const;

    // ---- For the car's light and shadow effects (drive/fx/CarFx.h); null until loaded.
    const CarTypeInfoU2* CarType() const;
    const CarRenderLayoutU2* Layout() const;
    const RideInfoU2* Ride() const;
    const std::vector<std::unique_ptr<SolidList>>& SolidPacks() const;
    // A texture of the car's packs (its own, the shared car packs, GLOBALB), or null.
    const Texture* FindTexture(uint32_t hash) const;

    // ---- Checks
    // One line per drawn part: label, row, solid, triangles and resolved textures (MISSING_ when absent).
    std::string Describe() const;
    // Writes the car as an OBJ (game space). Without body/wheels: the static rest pose of the
    // reference layout; with them: the matrices Draw would use.
    bool DumpObj(const std::wstring& path, const Mat4* body = nullptr, const Mat4* wheels = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bb
