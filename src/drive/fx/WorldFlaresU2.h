#pragma once

// The world's light flares of NFSU2 PC v1.2, read from a region's sections and drawn the way the game's flare
// pass draws them. Reverse engineering (SPEED2.EXE v1.2 IDA listing; data from TRACKS\STREAML4RA.BUN):
//
// A. Street lights: the sections' light flare packs.
//  Each streaming section may carry a chunk 0x80135100, loaded by sub_48E070 (loader of the 0x80135000 /
//  0x80135100 / 0x135200 group; unloader sub_48E270):
//    0x135101  pack header (16-aligned, 0x60 bytes): +0x08 u32 version (only 3 is linked), +0x30 / +0x40 box
//              min / max of the flares (x, y, z, 0), +0x50 u16 flare count, +0x52 u8 "already swapped" flag,
//              +0x54 u32 section number (the pack goes into that section's info, sub_5DB680, at +0x08),
//              +0x58 bTList head (runtime).
//    0x135102  eLightFlare array (16-aligned, 0x30 each, linked into the header's list): +0x08 name hash,
//              +0x0C colour tint (0x00FFFFFF everywhere; eRenderLightFlare never reads it), +0x10 world
//              position, +0x1C ReflectPosZ (999.0 = not computed; the reflection pass raycasts it), +0x20
//              direction, +0x2C type, +0x2D flags, +0x2E the section number's low byte.
//    L4RA: 250 packs, 2121 flares, all type 9, 2113 with flags 2 (no falloff) and 8 with flags 4 (directional),
//    direction (0, 0, -1).
//  (0x80135000 = 0x135001 / 0x135002 / 0x135003 is another pack: the section's eLight point / spot lights,
//  0x60-byte records named after their lamp model (XO_StreetLightC_08...) with colour, position, radius and
//  direction, plus a lookup tree; section info +0x0C. They light the cars and are not drawn.)
//  sub_491860(view, flareMode): returns unless dword_7F3D40 (flares on) and TheGameFlowManager == 6; takes the
//  visible section set of the camera position (TheVisibleSectionManager sub_5E00D0: the preculler zone holding
//  the point, else the nearest within 0.1, else nothing), and for each section number of the set whose info
//  (dword_883EC0[n]) has a flare pack whose box passes sub_5BC300(view, min, max) calls eRenderLightFlare(view,
//  flare, eMathIdentityMatrix, 1.0, 0, flareMode, 0, 0) on every flare. eDisplayFrame's flare pass calls it with
//  flareMode 0 at 0x5CB5F6, after SetRenderState(ZWRITEENABLE, 0) and before the cars' RenderFlares (other
//  calls: 0x5CAC8C mode 2 reflection, 0x5CADD0 mode 1, 0x5CAEE5, 0x5CBC68).
//
// B. Scenery position markers ("emitters" on props): sub_5D6570 (one scenery instance, from sub_5D69B0) draws the
//  instance's model and, in views 1 and 2 only, turns every position marker of the drawn model into a flare of a
//  per-frame pool (sub_48AB20(list, viewMask); pools at 0x82CC30, two lists of 200 and 50 (dword_7F3D44), cleared
//  every frame by sub_48AC00): position = (marker +0x40 - model pivot solid+0x70) * instance matrix, type =
//  marker IParam0 + 10, list 1 for IParam0 == 3 else list 0; IParam0 == 3: flags 4 and direction = the
//  horizontal unit vector from the instance origin to the flare, else flags 2. In L4RA: guard rails
//  (XW_GUARDRAIL_*: IParam 0 -> type 10 amber), post boots (XO_POSTBOOTA: 1 -> type 11 red), cat's eyes
//  (XO_CATSEYEORANGE: 0 -> 10), median poles and XO_TRAFFICLIGHTC (3 -> type 13 amber, blinking).
//  The instance must be visible (eView::GetVisibleState) and big enough: the pixel size sub_5D64D0(view, pos,
//  info+0x3C + 6) must reach 17 (23 in view mode >= 3) for a model to be drawn at all (inferred equivalent:
//  CarLightFlaresU2::GetPixelSize with that radius).
//  sub_494270(view, 0, 0) draws the pools in the same flare pass right after the car flares: list 0 then list 1,
//  flares of view 1 only, type 13 of list 1 only while dword_82DAF0 is set (sub_48AC00 flips it every 0.5 s of
//  WorldTimeElapsed: a 1 Hz blink), each with eRenderLightFlare(view, f, identity, 1.0, 0, flareMode 3, 0, 0).
//  flareMode 3 (0x490D96): for types other than 13 and 14 the player car (RaceParameters::GetDriverNumber(0))
//  must face the flare: d = normalize(flare - car+0x60), c = d . car+0x90; no flare when c < RainInTheHeadlights
//  (0x803A70, 0.96), else intensity = (c - 0.96) / (1 - 0.96) (reflectors lit by the headlights; car+0x60 /
//  car+0x90 taken here as the car's position and forward axis: inferred). Modes other than 0 skip the screen-x
//  rotation of the quad (0x4913BD).
// Not drawn here: sub_433090's type-14 flares (a runtime effect, list 1) and the reflection / mirror passes.

#include "core/Math.h"
#include "core/Types.h"
#include "drive/cars/CarLightFlaresU2.h"
#include "drive/fx/FxCommon.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nfs { class World; }

namespace bb {

class WorldFlaresU2 {
public:
    static constexpr uint32_t PackChunk = 0x80135100, HeaderChunk = 0x135101, FlaresChunk = 0x135102;
    static constexpr int PoolCapacity[2] = { 200, 50 };   // dword_7F3D44

    struct Pack {
        uint32_t section = 0;        // header +0x54
        float min[3] = {}, max[3] = {};
        std::vector<LightFlareU2> flares;
    };

    // One scenery marker turned into a flare (B), with what its instance needs to be drawn.
    struct MarkerFlare {
        LightFlareU2 flare;
        int list = 0;                // pool list (IParam0 == 3 -> 1)
        uint32_t section = 0;        // the section placing the instance
        float origin[3] = {};        // instance position (pixel size)
        float radius = 0;            // SceneryInfo +0x3C
        Aabb bounds;                 // instance bounds (visibility)
    };

    // The player car for flareMode 3 (reflectors): position and forward axis, game space.
    struct PlayerCar {
        float position[3] = {};
        float forward[3] = {};
    };

    // Reads the flare packs of every section of the region and the marker flares of its scenery.
    // Returns the flare count (both kinds).
    size_t Load(const nfs::World& world);
    void Clear();
    bool LoadedFor(const nfs::World* world, size_t loadedSections) const {
        return world_ == world && world != nullptr && loadedSections == loadedSections_;
    }
    const std::vector<Pack>& Packs() const { return packs_; }
    const std::vector<MarkerFlare>& Markers() const { return markers_; }
    size_t FlareCount() const { return flareCount_; }

    // The flare table (0x7F3CC8) and RainInTheHeadlights from the exe; the car-flare defaults until then.
    void SetTable(FlareTableU2 table, float rainInTheHeadlights) {
        table_ = std::move(table);
        rain_ = rainInTheHeadlights;
        haveExeTable_ = true;
    }
    bool HasExeTable() const { return haveExeTable_; }

    // The quads of one frame: A (sub_491860 mode 0) then B (sub_494270, mode 3). visibleSections: the section
    // numbers of the camera's visible set (nullptr: every section, for a camera no zone claims). blinkOn:
    // dword_82DAF0. car: null outside the test drive (then the reflectors stay dark).
    struct Quad {
        FlarePolyU2 poly;
        bool marker = false;
    };
    void Build(const FlareViewU2& view, const std::vector<u32>* visibleSections, bool blinkOn, const PlayerCar* car,
               std::vector<Quad>& out) const;

    // Draws Build's quads. Returns how many.
    int Draw(fx::StateGuard& guard, fx::PolyRenderer& polys, const FlareViewU2& view, fx::GlobalTextures& globals,
             IDirect3DDevice9* device, const std::vector<u32>* visibleSections, bool blinkOn, const PlayerCar* car);

    std::string Describe() const;

private:
    // sub_5BC300 (inferred: a box against the view's planes): false when the box is entirely outside one plane.
    static bool BoxVisible(const FlareViewU2& v, const float* mn, const float* mx);
    bool SectionVisible(uint32_t section, const std::vector<u32>* visible) const;

    const nfs::World* world_ = nullptr;
    size_t loadedSections_ = 0;
    std::vector<Pack> packs_;
    std::vector<MarkerFlare> markers_;
    size_t flareCount_ = 0;
    FlareTableU2 table_ = CarLightFlaresU2::DefaultTable();
    float rain_ = 0.96f;
    bool haveExeTable_ = false;
    mutable std::vector<Quad> scratch_;
};

} // namespace bb
