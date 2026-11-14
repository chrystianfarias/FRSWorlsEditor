#pragma once

// Port of NFSU2Unity Core/Camera/ViewCamera.cs.

#include <cstdint>

namespace bb {

// Camera::CameraParams key (0xD0 bytes; the current key lives at Camera+0x00, the previous one at +0xD0).
class CameraKey {
public:
    float Matrix[16] = {};    // +0x00 world -> camera (x right, y down, z forward)
    float Position[4] = {};   // +0x40
    float Direction[4] = {};  // +0x50
    float Target[4] = {};     // +0x60
    float Noise[16] = {};     // +0x70 freq1, +0x80 amp1, +0x90 freq2, +0xA0 amp2 (TerrainVelocityNoise)
    float TargetDistance = 0;                       // +0xB0
    float FocalDistance = 0;                        // +0xB4 (15, or 0 when the mover's +0x53 flag is set)
    float DepthOfField = 0;                         // +0xB8 (65, or 0)
    float NearZ = 0;                                // +0xBC
    float FarZ = 0;                                 // +0xC0
    uint16_t Fov = 0;                               // +0xC4 bAngle (65536 = 360 deg), horizontal (see CameraProjection)
    uint16_t FieldC6 = 0;                           // +0xC6
    uint32_t FieldC8 = 0;                           // +0xC8
    float SimTimeMultiplier = 0;                    // +0xCC

    void CopyFrom(const CameraKey& k) { *this = k; }
};

// The game's Camera object ([eView+0x40]; player 1 = 0x8760E0). Only the fields the drive camera reads or
// writes are modelled; offsets in the comments.
class ViewCamera {
public:
    CameraKey Current;                          // +0x000
    CameraKey Previous;                         // +0x0D0
    float PositionRate[4] = {};                 // +0x1E0 (w = 0)
    float DirectionRate[4] = {};                // +0x1F0
    float TargetRate[4] = {};                   // +0x200
    float NoiseRate[16] = {};                   // +0x210 (+0x70), +0x220 (+0x80), +0x230 (+0x90), +0x240 (+0xA0)
    float ScalarRate[5] = {};                   // +0x250..+0x260 rates of +0xB0..+0xC0
    uint16_t FovRate = 0;                       // +0x264 (u16, wraps)
    bool ResetVelocities = false;               // +0x270 (ResetState sets it)
    float ElapsedTime = 0;                      // +0x274
    int ResetFrame = 0;                         // +0x27C RealTimeFrames at ResetState
    const void* HiddenCar = nullptr;            // +0x280 car not rendered by this view (bumper/hood)
    const void* Field284 = nullptr;             // +0x284

    // Camera::StopUpdating (0x828640): debug freeze, 0 in the game.
    static inline bool StopUpdating = false;

    // Defaults written by the Camera constructor sub_44F5A0 (inferido) and CameraMover::CameraMover 0x44AB90
    // (far = 10000) / CameraMover::Enable 0x444040 (near = 0.5).
    static ViewCamera CreateDefault();

    // Camera::SetCameraMatrix(const bMatrix4&, float) 0x447280 (the JollyRancher remote link is not modelled).
    void SetCameraMatrix(const float* m, float elapsed);
};

} // namespace bb
