// C++ counterpart of NFSU2Unity Runtime/GameCamera.cs (no Unity).

#include "drive/camera/GameCamera.h"

#include "drive/cars/CarTypeInfoU2.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/VehicleSim.h"
#include "drive/physics/World.h"

#include <algorithm>
#include <cmath>

namespace bb {

namespace {

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Cross3(float* o, const float* a, const float* b) {
    float x = a[1] * b[2] - a[2] * b[1];
    float y = a[2] * b[0] - a[0] * b[2];
    float z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}

bool Normalize3(float* v) {
    float l = std::sqrt(Dot3(v, v));
    if (!(l > 1e-12f) || !std::isfinite(l)) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

// Rotation (columns right, down, forward in world space) to a unit quaternion (x, y, z, w).
// Returns false when the basis is degenerate.
bool BasisToQuat(const float* right, const float* down, const float* forward, float* q) {
    float f[3] = { forward[0], forward[1], forward[2] };
    if (!Normalize3(f)) return false;
    float d[3] = { down[0], down[1], down[2] };
    float k = Dot3(d, f);
    d[0] -= f[0] * k; d[1] -= f[1] * k; d[2] -= f[2] * k;
    if (!Normalize3(d)) {
        // down parallel to forward: fall back to right x forward
        float r[3] = { right[0], right[1], right[2] };
        Cross3(d, f, r);
        if (!Normalize3(d)) return false;
    }
    float r[3];
    Cross3(r, d, f);   // (r, d, f) right-handed: r x d = f, d x f = r
    // R[i][j] = column j, row i
    double m00 = r[0], m01 = d[0], m02 = f[0];
    double m10 = r[1], m11 = d[1], m12 = f[1];
    double m20 = r[2], m21 = d[2], m22 = f[2];
    double tr = m00 + m11 + m22;
    double x, y, z, w;
    if (tr > 0.0) {
        double s = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
    } else if (m11 > m22) {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
    } else {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
    }
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0.0)) return false;
    q[0] = static_cast<float>(x / n); q[1] = static_cast<float>(y / n);
    q[2] = static_cast<float>(z / n); q[3] = static_cast<float>(w / n);
    return true;
}

void QuatToBasis(const float* q, float* right, float* down, float* forward) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - z * w), m02 = 2 * (x * z + y * w);
    double m10 = 2 * (x * y + z * w), m11 = 1 - 2 * (x * x + z * z), m12 = 2 * (y * z - x * w);
    double m20 = 2 * (x * z - y * w), m21 = 2 * (y * z + x * w), m22 = 1 - 2 * (x * x + y * y);
    right[0] = static_cast<float>(m00); right[1] = static_cast<float>(m10); right[2] = static_cast<float>(m20);
    down[0] = static_cast<float>(m01); down[1] = static_cast<float>(m11); down[2] = static_cast<float>(m21);
    forward[0] = static_cast<float>(m02); forward[1] = static_cast<float>(m12); forward[2] = static_cast<float>(m22);
}

void Slerp(const float* a, const float* b, float t, float* o) {
    double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    double dot = ax * bx + ay * by + az * bz + aw * bw;
    if (dot < 0.0) { bx = -bx; by = -by; bz = -bz; bw = -bw; dot = -dot; }
    double wa, wb;
    if (dot > 0.9995) { wa = 1.0 - t; wb = t; }   // nearly equal: normalized lerp
    else {
        double th = std::acos(std::min(1.0, dot));
        double s = std::sin(th);
        wa = std::sin((1.0 - t) * th) / s;
        wb = std::sin(t * th) / s;
    }
    double x = wa * ax + wb * bx, y = wa * ay + wb * by, z = wa * az + wb * bz, w = wa * aw + wb * bw;
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0.0)) { o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; o[3] = b[3]; return; }
    o[0] = static_cast<float>(x / n); o[1] = static_cast<float>(y / n);
    o[2] = static_cast<float>(z / n); o[3] = static_cast<float>(w / n);
}

} // namespace

void GameCamera::Init(VehicleSim& sim, const Speed2Image& exe, const CarTypeInfoU2& carType) {
    sim_ = &sim;
    if (!data_) data_ = std::make_unique<CameraData>(exe);
    ctx_ = std::make_unique<CameraContext>();
    cameraInfo_ = std::make_unique<CameraInfo>(CameraInfo::FromCarType(carType.Raw));
    car_ = std::make_unique<CameraCar>();
    auto v0 = sim.Velocity();
    car_->FromSim(sim, v0.data(), 0.0f, cameraInfo_.get());
    World& w = sim.Env.World;
    ctx_->WorldTimer = w.WorldTimer;
    ctx_->WorldTimeElapsed = 0.0f;
    ctx_->Flags = &w.Flags;
    view_ = std::make_unique<ViewCamera>(ViewCamera::CreateDefault());
    Mover.reset();
    Mover = std::make_unique<CubicCameraMover>(*data_, *ctx_, *view_, 1, *car_, DriveCameraViews::PovType(ViewSetting), false, false, true);
    hasPrev_ = false;
    Step();
}

void GameCamera::SetView(int setting) {
    ViewSetting = setting;
    int pov = DriveCameraViews::PovType(ViewSetting);
    Pov p;
    if (Mover && cameraInfo_->GetPov(pov, false, p) && p.Type == pov) Mover->SetPovType(pov);
}

void GameCamera::CycleView() {
    if (!Mover) return;
    // Player::CycleDriveCameraPovType: scroll the setting, then SetPovType if the car has that POV.
    ViewSetting = DriveCameraViews::Scroll(ViewSetting, 1, AllowHighChase);
    int pov = DriveCameraViews::PovType(ViewSetting);
    Pov p;
    if (cameraInfo_->GetPov(pov, false, p) && p.Type == pov) Mover->SetPovType(pov);
}

void GameCamera::SetLookBack(bool held) {
    if (!Mover) return;
    if (held != Mover->LookBack) Mover->SetLookBack(held);
}

void GameCamera::BeforeSimFrame() {
    if (sim_ == nullptr) return;
    const float* v = sim_->Car.State.Vel;
    velAtBegin_[0] = v[0]; velAtBegin_[1] = v[1]; velAtBegin_[2] = v[2];
}

void GameCamera::AfterSimFrame() {
    if (!Mover) return;
    float dt = sim_->LastDt();
    car_->FromSim(*sim_, velAtBegin_, dt, cameraInfo_.get());
    ctx_->WorldTimer = sim_->Env.World.WorldTimer;
    ctx_->WorldTimeElapsed = dt;
    if (dt > 0.0f) ctx_->Shakers[0].Maybe(*data_, car_->Accel, false, ctx_->PlayerCinematic);   // inside World::DoTimestep
    Step();
    ctx_->Shakers[0].Update(*data_, dt);
    ctx_->Shakers[1].Update(*data_, dt);
}

void GameCamera::Step() {
    Mover->Update();
    const CameraKey& k = Mover->Camera->Current;
    const float* m = k.Matrix;
    // camera axes in world = columns of the view matrix: x right, y down, z forward
    const float right[3] = { m[0], m[4], m[8] };
    const float down[3] = { m[1], m[5], m[9] };
    const float fwd[3] = { m[2], m[6], m[10] };
    std::copy(curPos_, curPos_ + 3, prevPos_);
    std::copy(curRot_, curRot_ + 4, prevRot_);
    curPos_[0] = k.Position[0]; curPos_[1] = k.Position[1]; curPos_[2] = k.Position[2];
    float q[4];
    if (BasisToQuat(right, down, fwd, q)) std::copy(q, q + 4, curRot_);   // else keep the last good rotation
    if (!hasPrev_) {
        std::copy(curPos_, curPos_ + 3, prevPos_);
        std::copy(curRot_, curRot_ + 4, prevRot_);
        hasPrev_ = true;
    }
}

void GameCamera::Pose(float alpha, float eye[3], float right[3], float down[3], float forward[3]) const {
    float t = std::isfinite(alpha) ? std::min(1.0f, std::max(0.0f, alpha)) : 1.0f;
    for (int i = 0; i < 3; i++) eye[i] = prevPos_[i] + (curPos_[i] - prevPos_[i]) * t;
    float q[4];
    Slerp(prevRot_, curRot_, t, q);
    QuatToBasis(q, right, down, forward);
}

void GameCamera::Projection(int width, int height, float& verticalFovRadians, float& aspect, float& nearZ, float& farZ) const {
    if (!Mover) {
        verticalFovRadians = 1.0f; aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;
        nearZ = 0.5f; farZ = 10000.0f;
        return;
    }
    // sub_5C7EC0: horizontal FOV word (x1.1 for player views), vertical angle = FOV * H/W * 1.265
    CameraProjection p = CameraProjection::Build(*data_, Mover->Camera->Current, 1, std::max(1, width), std::max(1, height));
    verticalFovRadians = static_cast<float>(2.0 * std::atan(1.0 / p.YScale));
    aspect = static_cast<float>(p.FrustumAspect());
    nearZ = p.Near;
    farZ = p.Far;
}

bool GameCamera::HidesCar() const {
    return Mover && Mover->Camera->HiddenCar != nullptr;
}

std::string GameCamera::ViewName() const {
    int s = ViewSetting;
    if (s < 0 || s > 4) s = 3;
    return DriveCameraViews::Names[s];
}

} // namespace bb
