#pragma once

// Port of NFSU2Unity Core/Camera/CameraData.cs.

#include "drive/numerics/BWareMath.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bb {

// POV entry of the per-car CameraInfo (CarTypeInfo+0x720: int count[2]; POV set0[6] at +0x10, set1[6] at +0x70).
struct Pov {
    int16_t Type = 0;     // +0x0 0 far chase, 1 near chase, 2 bumper, 3 in-car (unused), 4 hood, 5 high chase (drift/drag)
    uint16_t Angle = 0;   // +0x2 look-down angle (bAngle)
    float X = 0;          // +0x4 eye offset forward (car space)
    float Z = 0;          // +0x8 eye height
    float Y = 0;          // +0xC eye offset left

    std::string ToString() const;
};

// CameraInfo at CarTypeInfo+0x720 (CameraInfo::GetPov 0x441140, Car::GetPov 0x5ECCA0).
class CameraInfo {
public:
    int Count[2] = {};
    Pov Sets[2][6] = {};

    // From the raw CarTypeInfo bytes (CarTypeInfoU2.Raw). Throws std::runtime_error when out of range.
    static CameraInfo FromCarType(const std::vector<uint8_t>& cti, int offset = 0);

    // Car::GetPov(type): CameraInfo::GetPov(type), falling back to GetPov(0). Set 1 is used when
    // eGetCurrentViewMode() == 3 (split screen, inferido). Returns false when even type 0 is missing.
    bool GetPov(int type, bool splitScreen, Pov& pov) const;
};

// CubicPovData (0xB0 bytes) = 11 x 16-byte blocks (docs/re/camera.md §3.3).
class CubicPovData {
public:
    float F[44] = {};

    float EyeDuration() const { return F[0]; }    // +0x00
    float LookDuration() const { return F[1]; }   // +0x04
    float FovDuration() const { return F[2]; }    // +0x08
    float UpDuration() const { return F[3]; }     // +0x0C
    // +0x10 up gain (x, y, z), +0x20 up min, +0x30 up max, +0x40 eye lag gain, +0x50 eye min, +0x60 eye max,
    // +0x70 look lag gain, +0x80 look min, +0x90 look max, +0xA0 forward durations (x, y, z)
    std::array<float, 3> Vec(int byteOffset) const { return { F[byteOffset / 4], F[byteOffset / 4 + 1], F[byteOffset / 4 + 2] }; }

    CubicPovData Clone() const { return *this; }

    // sub_445750(out, next, cur, t): out = (1-t)*cur + t*next with the original operation order.
    static CubicPovData Lerp(const CubicPovData& next, const CubicPovData& cur, float t);
};

// Per-POV interpolation table (0x7EC610 + type*20: {int count; float x0; float x1; float invStep; CubicPovData*}),
// looked up by sub_446DC0. The camera always passes the constant 0.5 (dword_787154).
class PovDataTable {
public:
    int Count = 0; float X0 = 0, X1 = 0, InvStep = 0;
    std::vector<CubicPovData> Entries;

    // sub_446DC0(out, x).
    CubicPovData Lookup(float x) const;
};

// Every constant and static table the drive camera reads from SPEED2.EXE (.rdata/.data), loaded from the
// installed exe. Addresses in docs/re/camera.md.
class CameraData {
public:
    const Speed2Image* Exe;
    BWareMath Math;
    PovDataTable PovTables[6];                                    // 0x7EC610
    uint16_t PovFov[6] = {};                                      // word_7EB5B4
    float NoiseAmpByPov[6] = {};                                  // dword_7EBE90
    float NoiseFreqByPov[6] = {};                                 // dword_7EBEA8
    float NearClipInCar = 0, NearClip = 0;                        // 0x7EB5B0 (0.4), flt_784260 (0.5)
    float PovLookupX = 0;                                         // dword_787154 0.5
    float LookUpOffset = 0;                                       // dword_78715C 2.0 (topology probe height)
    float HighliteScale = 0;                                      // flt_787210 1.3
    float EyeLookScale = 0;                                       // flt_78721C 1.0
    float AspectFovScale = 0;                                     // flt_78721C 1.0 (bATan x argument)
    float FocalDistance = 0, DepthOfField = 0;                    // dword_78707C 15, dword_787078 65
    float NoiseScaleNotDrag = 0;                                  // flt_7870B8 0.2
    float ReflectScale = 0;                                       // flt_7855D0 -2.0
    float ForwardFadeTime = 0;                                    // flt_787158 2.0
    float TickToSec = 0;                                          // flt_784268 0.00025
    float CubicTMax = 0;                                          // dword_787150 10.0
    float SeedSnapDistance = 0;                                   // flt_78714C 20.0
    float DriftSpeedGraph[4] = {};                                // gDriftSpeed = Graph(0x7EC420, 2)
    float BirdsEye[8] = {};                                       // dword_7EC400 (2 x vec4)
    uint16_t BirdsEyeAngle[2] = {};                               // word_787080
    float BurnoutEye[3] = {};                                     // flt_7EC5F0
    float DriverFlagEye[3] = {};                                  // flt_7EC600
    uint16_t BurnoutAngle = 0, DriverFlagAngle = 0;               // word_82A4D0 / word_82A4DC (.bss, 0 unless tweaked)
    // TerrainVelocityNoise
    float NoiseDefaultMaxSpeed = 0, NoiseTableSpan = 0, Quarter = 0, TerrainAmpScale = 0, NosAmpAdd = 0, NosFreqMul = 0;
    float NoiseSpeedTable[20] = {};                               // flt_7EC430 5 x vec4
    float NoiseK[16] = {};                                        // flt_7EC480
    float Half = 0, Inv2p30 = 0, K65536 = 0;                      // flt_786F6C, flt_787580, flt_784238
    uint16_t NoiseRefFov = 0;                                     // word_7EC1F0
    static constexpr uint32_t VA_TerrainTypeInfo = 0x800EE0;      // stride 0x98: +0x18, +0x1C used here
    // shake
    std::vector<float> ShakeCurve;                                // off_7FB7A0 -> 0x7FB36C, 5 points
    float ShakeRotX = 0, ShakeRotY = 0, ShakeRotZ = 0;            // flt_7FB740/744/748
    float ShakeAccelScale[3] = {};                                // flt_7FB750..758
    float ShakeMin = 0, ShakeMax = 0, ShakeAmpLo = 0, ShakeAmpHi = 0, ShakeDurLo = 0, ShakeDurHi = 0;   // flt_79DCD4..E8
    float ShakeFlicker = 0;                                       // dword_79DC68 17.0
    float ShakeCineAmp = 0, ShakeCineDur = 0;                     // flt_7850EC 0.8, flt_78A0EC 1.2
    // projection (sub_5C7EC0)
    float ProjXScale = 0, ProjYScaleA = 0, ProjYScaleB = 0, PlayerFarZ = 0, SpecialAspect = 0;

    // The image must outlive this object.
    explicit CameraData(const Speed2Image& exe);

    // TerrainTypeInfoTable[t] fields +0x18 / +0x1C (camera noise contributions).
    float TerrainA(int t) const { return Exe->F32(VA_TerrainTypeInfo + static_cast<uint32_t>(t) * 0x98 + 0x18); }
    float TerrainB(int t) const { return Exe->F32(VA_TerrainTypeInfo + static_cast<uint32_t>(t) * 0x98 + 0x1C); }

    // (C# CameraData.Shared, a lazily loaded instance for Speed2Image.DefaultPath, is not ported: the
    // editor passes its own image.)
};

} // namespace bb
