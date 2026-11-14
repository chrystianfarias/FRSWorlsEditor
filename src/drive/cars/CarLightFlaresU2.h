#pragma once

// Port of NFSU2Unity Core/Cars/CarLightFlaresU2.cs: the car light flares of NFSU2 PC v1.2
// (docs/re/car-lights-shadow.md): CarRenderInfo::CreateCarLightFlares 0x6204D0, RenderFlares 0x61AE90
// (per-view table tweaks), CarRenderInfo::RenderFlaresOnCar 0x615630 (intensities) and eRenderLightFlare
// 0x490920 (normal mode). eRenderLightFlare is also what draws the world's flares (sub_491860, see
// drive/fx/WorldFlaresU2.h), so the editor addition TableFromExe reads all 15 types.

#include <cstdint>
#include <vector>

namespace bb {

class Speed2Image;

// eLightFlare (0x30 bytes), built by CarRenderInfo::CreateCarLightFlares 0x6204D0 (cars) or read from a
// section's 0x135102 chunk (world).
struct LightFlareU2 {
    uint32_t NameHash = 0;        // +0x08 marker name
    uint32_t ColourTint = 0;      // +0x0C (0 for car flares; never read by eRenderLightFlare)
    float X = 0, Y = 0, Z = 0;    // +0x10 marker position - body pivot (CRI+0x30), car space
    float ReflectPosZ = 0;        // +0x1C (0; reflection pass only)
    float DirX = 0, DirY = 0, DirZ = 0; // +0x20 marker matrix row 2 (its Z axis)
    int8_t Type = 0;              // +0x2C flare type: 0 headlight, 1 brake light
    int8_t Flags = 0;             // +0x2D 4 = directional (falloff pow(cos, power))
};

// One layer of a flare type (0x2C bytes, table 0x7F3CC8: 15 types x 2 layer pointers).
struct FlareLayerU2 {
    float MinSize = 0, MaxSize = 0;     // +0x00/+0x04 half size in "512-wide" pixels, lerped by intensity
    float R = 0, G = 0, B = 0, A = 0;   // +0x08..+0x14 (0..255, times the intensity)
    float Power = 0;                    // +0x18 exponent of the directional falloff (flags & 4)
    float Offset = 0;                   // +0x1C metres the quad is pulled toward the camera
    float MinScale = 0, MaxScale = 0;   // +0x20/+0x24 clamp of (view depth / focal) = metres per pixel
    int TexIndex = 0;                   // +0x28 0 HEADLIGHTFLAREINNER, 1 HEADLIGHTFLAREOUTER, 2 HEADLIGHTGLOW
};

// C# FlareLayerU2[][]: per type, its layers (a null layer pointer of the exe's table is left out).
using FlareTableU2 = std::vector<std::vector<FlareLayerU2>>;

// The view data eRenderLightFlare reads: eView+0x0C (focal: 256 / tan(fov/2), "512-pixel" units), the
// eCamera at eView+0x40 (matrix +0x00 = world->camera with camera x right, y down, z forward; position +0x40),
// the platform info at [eView] (+0x80 ViewProj, +0xC0 six frustum planes) and the back buffer size
// (dword_870980/84). Everything in game space, row vectors.
class FlareViewU2 {
public:
    int ViewId = 1;
    float Focal = 0;
    float CamMatrix[16] = {};
    float CamPos[3] = {};
    float CamDir[3] = {};    // eCamera+0x50 (GetPixelSize)
    float ViewProj[16] = {};
    float Planes[24] = {};   // (a, b, c, d) x 6; visible when a*x+b*y+c*z+d > 0 for all
    int Width = 640, Height = 480;
};

// One textured quad queued by eRenderLightFlare (ePoly: 4 positions, default uvs, one colour).
struct FlarePolyU2 {
    float X0 = 0, Y0 = 0, Z0 = 0, X1 = 0, Y1 = 0, Z1 = 0, X2 = 0, Y2 = 0, Z2 = 0, X3 = 0, Y3 = 0, Z3 = 0; // world space; uv (0,0) (1,0) (1,1) (0,1)
    // ePoly colour bytes R, G, B, A as one little-endian dword (0xAABBGGRR).
    uint32_t Colour = 0;
    int TexIndex = 0;
    int Layer = 0;
};

// Car light flares of NFSU2 PC v1.2 (docs/re/car-lights-shadow.md):
// CarRenderInfo::CreateCarLightFlares 0x6204D0, RenderFlares 0x61AE90 (per-view table tweaks),
// CarRenderInfo::RenderFlaresOnCar 0x615630 (intensities) and eRenderLightFlare 0x490920 (normal mode).
class CarLightFlaresU2 {
public:
    static constexpr uint32_t LeftHeadlight = 0x9DB90133, RightHeadlight = 0xD09091C6, CentreHeadlight = 0x7A5BCF69;
    static constexpr uint32_t LeftBrakelight = 0x31A66786, RightBrakelight = 0xBF700A79, CentreBrakelight = 0xA2A2FC7C;
    // Flare textures (dword_82CC10[0..2], hashes at 0x7F39CC).
    static constexpr uint32_t TextureHashes[3] = { 0x8868656F, 0x88D8E8A2, 0x3394FE62 };

    // Flare layer table as stored in .data (0x7F3A30 type 0 layer 0, 0x7F3A5C type 0 layer 1, 0x7F3A88 type 1),
    // before RenderFlares patches it per view. Only the types the car flares use are transcribed.
    static FlareTableU2 DefaultTable();

    // Editor addition: all 15 types as the exe's .data holds them (table 0x7F3CC8, two layer pointers per
    // type, 0x2C-byte layers), unpatched. The world flares (type 9, street lights) use it.
    static constexpr uint32_t TableVa = 0x7F3CC8;
    static constexpr int NumTypes = 15;
    static FlareTableU2 TableFromExe(const Speed2Image& exe);

    // RenderFlares 0x61AE90, before the car loop: headlight inner layer power = 1.45 in a race
    // (TheGameFlowManager == 6, copied from 0x7F39F0) else 2.8; view 3 (rear-view mirror): power x 0.75 and
    // min/max size x 2.7 (from the pristine copy at 0x7F39D8); brake layer offset = 0.34 in views 4/5
    // (reflections) else 0.2 (0x7F3A20).
    static FlareTableU2 TableForView(int viewId, bool inRace);
    // Editor addition: the same patch applied to any table holding types 0 and 1 (e.g. TableFromExe).
    static void PatchForView(FlareTableU2& t, int viewId, bool inRace);

    // RemoveCentreBrakeWithCustomSpoiler 0x60C8A0: car types 5, 11, 12, 24, 25, 30.
    static bool RemoveCentreBrakeWithCustomSpoiler(int carType) {
        return carType == 5 || carType == 11 || carType == 12 || carType == 24 || carType == 25 || carType == 30;
    }

    // Light state read by RenderFlaresOnCar.
    struct LightState {
        bool HasCar = false;          // CRI+0 != NULL (false in the front end: headlights always drawn)
        bool Traffic = false;         // Car+0x14 -> +4 == 3: base headlight intensity 0 instead of 1
        bool HeadlightsOn = false;    // Car+0x93D
        bool Braking = false;         // Car+0x944
        int ForceFlags = 0;           // arg + ForceHeadlightsOn (1) / ForceBrake (2) / ForceReverse (4, unused); 8 = headlights off
        bool CustomSpoiler = false;   // ride Parts[12] level > 0
        int CarType = 0;              // CarTypeInfo index (RemoveCentreBrakeWithCustomSpoiler)
        int FeSelectedSlot = 0;       // RideInfo+0x8E4 -> +4 (FE part being browsed; 14 hides headlights, 15 brake lights); 0x45 = none
    };

    // Intensity of one flare as RenderFlaresOnCar 0x615860..0x61594C picks it, clamped to (0, 1]; 0 = not drawn.
    // Also returns the colour override (CRI+0x380 for the headlights, 0 for the brake lights).
    static float Intensity(const LightFlareU2& f, const LightState& st, uint32_t headlightColour, float scale,
                           uint32_t& colourOverride);

    // eView::GetPixelSize 0x48ACB0: projected size in pixels of a sphere, 0 when behind the camera.
    static int GetPixelSize(const FlareViewU2& v, const float* pos, float radius);

    // RenderFlaresOnCar 0x6156B9..0x6156E2: no flares when the car is under 4 pixels (x0.7 in view mode 3).
    static bool CarBigEnough(const FlareViewU2& v, const float* carPos, bool viewMode3);

    // sub_5BC450: a point is visible when it is strictly inside the six planes.
    static bool PointInFrustum(const FlareViewU2& v, const float* p);

    // sub_401020: degrees to a 16-bit angle, (int)(deg * 65536) / 360 with C truncation.
    static uint16_t DegToAngle(float deg);

    // eCreateAxisRotationMatrix 0x5BA8B0 (rotation about a unit axis, row vectors; row 3 = 0, M33 = 1).
    static void AxisRotation(float* o, const float* axis, uint16_t angle);

    // sub_5BC4A0 (x only): screen x in pixels of a world point through ViewProj.
    static float ScreenX(const FlareViewU2& v, const float* p);

    // eRenderLightFlare 0x490920 in normal mode (reflexAction 0, flareMode 0, reflOverride 0): returns the quads
    // queued (0..2, one per layer of the flare type). `table` comes from TableForView.
    // `localWorld` is the car matrix (rotation rows + position row 3). outPolys may be null; at most
    // outCapacity polygons are written.
    // Editor addition: screenRotation false = flareModes 1, 3 and 4 (0x4913BD): the camera axes are only scaled,
    // not normalized and rotated with the screen x (the scenery marker flares of sub_494270 use mode 3).
    static int Render(const FlareViewU2& v, const LightFlareU2& f, const float* localWorld, float intensity,
                      uint32_t colourOverride, const FlareTableU2& table, FlarePolyU2* outPolys, int outCapacity,
                      bool screenRotation = true);

private:
    // 0x49143C..0x49157B: v' = v * M (row vector, with the translation row).
    static void Rotate(float* v, const float* m);
};

// Small copy of eMulVector(bVector4) 0x486F50 with float intermediates (same order).
namespace CameraMathLite {
void MulVec4(float* o, const float* m, const float* v);
}

} // namespace bb
