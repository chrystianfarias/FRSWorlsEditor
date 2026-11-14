#pragma once

// Port of NFSU2Unity Core/Camera/CameraProjection.cs.

#include "drive/camera/CameraData.h"
#include "drive/camera/ViewCamera.h"

#include <array>
#include <cstdint>

namespace bb {

// Projection of a view, sub_5C7EC0 (eViewPlatInfo setup, inferido name): D3D left-handed perspective with the
// Y axis negated (camera space is y-down). The FOV word is horizontal; the vertical angle is the FOV angle
// times the aspect H/W (times 1.1/0.8695652 for player views), not derived from the tangent.
struct CameraProjection {
    float XScale = 0;   // proj._11 = 1/tan(fovX/2)
    float YScale = 0;   // -proj._22 = 1/tan(fovY/2)
    float Q = 0;        // proj._33 = zf/(zf-zn); proj._43 = -Q*zn; proj._34 = 1
    float Near = 0, Far = 0;
    uint16_t HalfFovX = 0, HalfFovY = 0;   // bAngle

    // sub_5C7EC0 for view viewId (1 = player 1) and a viewport of width x height pixels.
    static CameraProjection Build(const CameraData& d, const CameraKey& cam, int viewId, int width, int height);

    // Row-vector D3D matrix as the game stores it (eViewPlatInfo+0x40).
    std::array<float, 16> Matrix() const;

    // Equivalent symmetric frustum: vertical FOV in degrees and width/height aspect (for engines that take those).
    double VerticalFovDegrees() const;
    double FrustumAspect() const { return static_cast<double>(YScale) / XScale; }
};

// Player drive-camera settings (PlayerSettings+0x10, ePlayerSettingsCameras) and their POV types:
// GetPOVTypeFromPlayerCamera 0x502500, PlayerSettings::ScrollDriveCam 0x502540.
namespace DriveCameraViews {

inline const char* const Names[5] = { "Bumper", "Hood", "Near chase", "Far chase", "High chase" };

// GetPOVTypeFromPlayerCamera: 0 -> 2 bumper, 1 -> 4 hood, 2 -> 1 near, 3 -> 0 far, 4 -> 5 high, else 0.
int PovType(int setting);

// ScrollDriveCam(dir): +1/-1 with wrap; the last setting is 3, or 4 (high chase) in drift races, when
// byte_89E7D9 is set, or for TheRaceParameters 4174..4179.
int Scroll(int setting, int dir, bool allowHighChase);

} // namespace DriveCameraViews

} // namespace bb
