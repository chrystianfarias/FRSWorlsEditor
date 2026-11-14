#pragma once

// The game's light and shadow effects around the test drive (and the world's flares in the editor's "Game
// look"), put together for DriveMode: the headlight projector on the world, the car's shadow, the world's light
// flares and the car's light flares, in the order the game draws them (projector with the world, shadow with the
// car, then the flare pass: world flares, car flares, scenery marker flares). See CarFx.h, HeadlightProjector.h
// and WorldFlaresU2.h.

#include "core/Math.h"
#include "drive/cars/CarShadowU2.h"
#include "drive/fx/CarFx.h"
#include "drive/fx/FxCommon.h"
#include "drive/fx/HeadlightProjector.h"
#include "drive/fx/WorldFlaresU2.h"

#include <d3d9.h>

#include <memory>
#include <string>

class WorldRenderer;
namespace nfs { class World; }

namespace bb {

class BWareMath;
class CarModel;
class Speed2Image;

struct DriveFxFrame {
    const nfs::World* world = nullptr;
    std::wstring gameFolder;
    bool worldFlares = false;          // draw the world's flares (Game look or a drive)
    // The test drive's car; null outside a drive.
    const CarModel* car = nullptr;
    Mat4 body;                         // rows forward, left, up, position (Car+0x50)
    bool carVisible = true;            // false in the views that hide the car
    bool braking = false;              // Car+0x944
    bool headlightsOn = true;          // Car+0x93D (always on in game)
    ShadowElevation ground;            // the car's TopologyCoordinate::GetElevation
    const BWareMath* math = nullptr;   // bATan for the shadow's light vector
};

class DriveFx {
public:
    DriveFx();
    ~DriveFx();
    DriveFx(const DriveFx&) = delete;
    DriveFx& operator=(const DriveFx&) = delete;

    // With the world, before the car is drawn: the headlight projector (adds to the world) and the car's shadow.
    void BeforeCar(IDirect3DDevice9* device, WorldRenderer& renderer, const DriveFxFrame& frame);
    // After the car: the flare pass (world flares, car flares).
    void AfterCar(IDirect3DDevice9* device, WorldRenderer& renderer, const DriveFxFrame& frame);

    // The device's resources are going, or the car changed: drop what was uploaded (made again on demand).
    void ReleaseGpu();
    // A region was opened or closed: the world flares are read again.
    void ForgetWorld() { worldFlares_.Clear(); }
    // The car model was replaced (a new CarModel may reuse the old one's address).
    void ForgetCar() { car_.Release(); carFor_ = nullptr; }

    // Effect switches: g_CarShadowEnable (0..2), g_CarHeadlightEnable, g_LightGlowEnable.
    void SetShadowLevel(int level) { shadowLevel_ = level; }
    void SetProjector(bool on) { projectorOn_ = on; }
    void SetFlares(bool on) { flaresOn_ = on; }
    int ShadowLevel() const { return shadowLevel_; }
    bool ProjectorOn() const { return projectorOn_; }
    bool FlaresOn() const { return flaresOn_; }

    // Last frame's counts, for the HUD and the log.
    struct Counts {
        int worldQuads = 0, carQuads = 0, projectorDraws = 0, shadowStrips = 0;
        bool car = false, carVisible = false, carReady = false, projectorReady = false;
    };
    const Counts& LastCounts() const { return counts_; }
    std::string Status() const;

private:
    bool EnsureDevice(IDirect3DDevice9* device, const std::wstring& gameFolder);
    void EnsureCar(IDirect3DDevice9* device, const CarModel* car);
    void EnsureWorld(const nfs::World* world, const std::wstring& gameFolder);
    bool BlinkOn();

    IDirect3DDevice9* device_ = nullptr;
    std::wstring folder_;
    fx::GlobalTextures globals_;
    fx::PolyRenderer polys_;
    HeadlightProjector projector_;
    CarFx car_;
    const CarModel* carFor_ = nullptr;
    WorldFlaresU2 worldFlares_;
    std::unique_ptr<Speed2Image> exe_;
    std::unique_ptr<BWareMath> math_;   // bATan of the shadow's light vector (sub_60D350)
    bool exeTried_ = false;
    int shadowLevel_ = 2;
    bool projectorOn_ = true, flaresOn_ = true;
    Counts counts_;
    double blinkClock_ = 0.0, lastTime_ = 0.0, lastReport_ = 0.0;
    bool blink_ = false;
    std::string log_;
};

} // namespace bb
