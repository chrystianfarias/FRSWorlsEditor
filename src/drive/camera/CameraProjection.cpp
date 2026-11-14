// Port of NFSU2Unity Core/Camera/CameraProjection.cs.

#include "drive/camera/CameraProjection.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"

#include <cmath>

namespace bb {

CameraProjection CameraProjection::Build(const CameraData& d, const CameraKey& cam, int viewId, int width, int height) {
    CameraProjection p;
    float zn = cam.NearZ;
    float zf = viewId == 1 ? d.PlayerFarZ : cam.FarZ;
    float aspect = (F24::MulDouble(CameraMath::One, height) / F24::MulDouble(CameraMath::One, width)).Store();
    float kx = CameraMath::One;
    if (viewId == 1 || viewId == 4 || viewId == 2 || viewId == 5) {
        kx = d.ProjXScale;
        aspect = (F24(d.ProjYScaleA) / d.ProjYScaleB * aspect).Store();
    }
    if (viewId == 9 || viewId == 10) aspect = d.SpecialAspect;
    F24 fov = F24::MulDouble(CameraMath::One, cam.Fov);
    p.HalfFovX = static_cast<uint16_t>(X87::FtoL2Lo((fov * kx * BWareMath2::K_Half).V));
    float sx = BWareMath::Sin(p.HalfFovX);
    p.XScale = (F24(CameraMath::One) / (F24(sx) / BWareMath::Cos(p.HalfFovX))).Store();
    p.HalfFovY = static_cast<uint16_t>(X87::FtoL2Lo((fov * aspect * BWareMath2::K_Half).V));
    float sy = BWareMath::Sin(p.HalfFovY);
    p.YScale = (F24(CameraMath::One) / (F24(sy) / BWareMath::Cos(p.HalfFovY))).Store();
    p.Q = (F24(zf) / (F24(zf) - zn)).Store();
    p.Near = zn; p.Far = zf;
    return p;
}

std::array<float, 16> CameraProjection::Matrix() const {
    std::array<float, 16> m = {};
    m[0] = XScale; m[5] = -YScale; m[10] = Q; m[11] = CameraMath::One;
    m[14] = -(F24(Q) * Near).Store(); m[15] = 0.0f;
    return m;
}

double CameraProjection::VerticalFovDegrees() const {
    return 2.0 * std::atan(1.0 / YScale) * 180.0 / 3.14159265358979323846;
}

namespace DriveCameraViews {

int PovType(int setting) {
    switch (setting) {
    case 0: return 2;
    case 1: return 4;
    case 2: return 1;
    case 4: return 5;
    default: return 0;
    }
}

int Scroll(int setting, int dir, bool allowHighChase) {
    int last = allowHighChase ? 4 : 3;
    if (dir == 1) { setting++; if (setting > last) setting = 0; }
    else if (dir == -1) { setting--; if (setting < 0) setting = last; }
    return setting;
}

} // namespace DriveCameraViews

} // namespace bb
