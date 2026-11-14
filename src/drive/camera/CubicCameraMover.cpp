// Port of NFSU2Unity Core/Camera/CubicCameraMover.cs.

#include "drive/camera/CubicCameraMover.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"
#include "drive/physics/wheel/Graph.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bb {

// ------------------------------------------------------------------ VectorAverager

void VectorAverager::Reset() {
    std::fill(Buffer.begin(), Buffer.end(), 0.0f);
    Count = 0; Index = 0;
    Avg[0] = Buffer[0]; Avg[1] = Buffer[1]; Avg[2] = Buffer[2];
    Sum[0] = Buffer[0]; Sum[1] = Buffer[1]; Sum[2] = Buffer[2];
}

void VectorAverager::Add(const float* v) {
    if (Count < Size) Count++;
    int b = 4 * Index;
    Sum[0] = (F24(Sum[0]) - Buffer[b]).Store();
    Sum[1] = (F24(Sum[1]) - Buffer[b + 1]).Store();
    Sum[2] = (F24(Sum[2]) - Buffer[b + 2]).Store();
    Sum[0] = (F24(v[0]) + Sum[0]).Store();
    Sum[1] = (F24(v[1]) + Sum[1]).Store();
    Sum[2] = (F24(v[2]) + Sum[2]).Store();
    Buffer[b] = v[0]; Buffer[b + 1] = v[1]; Buffer[b + 2] = v[2];
    F24 inv = F24(CameraMath::One) / F24::MulDouble(CameraMath::One, Count);
    Avg[0] = (inv * Sum[0]).Store();
    Avg[1] = (F24(Sum[1]) * inv).Store();
    Avg[2] = (F24(Sum[2]) * inv).Store();
    Index++;
    if (Index >= Size) Index = 0;
}

// ------------------------------------------------------------------ CubicCameraMover

CubicCameraMover::CubicCameraMover(const CameraData& data, CameraContext& ctx, ViewCamera& camera, int viewId, CameraCar& car, int pov,
                                   bool disableLag, bool lookBack, bool noDof)
    : Data(&data), Ctx(&ctx), Camera(&camera), ViewId(viewId), Car(&car) {
    camera.Current.FarZ = 10000.0f;                            // CameraMover ctor
    camera.Current.NearZ = data.NearClip;                      // CameraMover::Enable
    LagEnabled = !disableLag; LookBack = lookBack; NoDof = noDof;
    Field30 = 0; PovType = pov; EffectivePov = pov; Snap = false;
    GroundTimer = ctx.WorldTimer;
    Field54 = 1;
    CubicPovData pd = data.PovTables[pov].Lookup(data.PovLookupX);
    Fov.Init(1, pd.F[2], data.CubicTMax);
    Fov.Duration = pd.F[2];
    Eye.Init(1, pd.F[0], data.CubicTMax);
    Look.Init(1, pd.F[1], data.CubicTMax);
    Forward.Init(1, pd.Vec(0xA0).data(), data.CubicTMax);
    Up.Init(1, pd.Vec(0xA0).data(), data.CubicTMax);
    Averager.Reset();
    Pov p;
    GetPov(PovType, p);
    float m[16] = {};
    SetDesired(m, p, pd, true);
    float inv[16];
    CameraMath::InvertTransformation(inv, m);
    if (LookBack) {
        inv[0] = -inv[0]; inv[1] = -inv[1]; inv[4] = -inv[4]; inv[5] = -inv[5];
        inv[8] = -inv[8]; inv[9] = -inv[9]; inv[12] = -inv[12]; inv[13] = -inv[13];
    }
    SeedFov(Fov);
    SeedVec(Eye, inv, Camera->Current.Position, Camera->PositionRate, Car->Vel);
    SeedVec(Look, inv, Camera->Current.Target, Camera->TargetRate, Car->Vel);
    F24 dx = F24(Eye.X.Val) - Eye.X.Des, dy = F24(Eye.Y.Val) - Eye.Y.Des, dz = F24(Eye.Z.Val) - Eye.Z.Des;
    F24 dist = F24::Sqrt(dz * dz + dy * dy + dx * dx);
    if (!BWareMath2::LeOrNaN(dist, data.SeedSnapDistance)) {
        Fov.Snap(); Eye.Snap(); Look.Snap();
    }
}

// sub_444910: FOV curve continues from the current camera FOV and its rate.
void CubicCameraMover::SeedFov(Cubic1D& c) {
    F24 rate = F24::MulDouble(CameraMath::One, Camera->FovRate);
    F24 fov = F24::MulDouble(CameraMath::One, Camera->Current.Fov);
    F24 v = fov + rate * Ctx->WorldTimeElapsed;
    float d = (rate * c.Duration).Store();
    c.Val = v.Store();
    if (BWareMath2::NeOrNaN(v, c.Des)) c.State = 2;
    c.dVal = d;
    if (BWareMath2::NeOrNaN(d, c.dDes)) c.State = 2;
}

// sub_444980 / sub_444A70: eye/look curves continue from the camera position/target and their rates.
void CubicCameraMover::SeedVec(Cubic3D& c, const float* inv, const float* pos, const float* rate, const float* carVel) {
    float p[3];
    BWareMath2::ScaleAdd(p, pos, rate, Ctx->WorldTimeElapsed);
    CameraMath::MulVec3(p, inv, p);
    c.SetVal(p);
    float rx = rate[0], ry = rate[1];
    float rz = rate[2];
    F24 x = F24(rx) - carVel[0], y = F24(ry) - carVel[1];
    float z = (F24(rz) - carVel[2]).Store();
    float dur = c.X.Duration;
    float v[4] = { (x * dur).Store(), (y * dur).Store(), (F24(z) * dur).Store(), 0.0f };
    CameraMath::MulVec4(v, inv, v);
    c.SetdVal(v);
}

// ------------------------------------------------------------------ small virtuals

bool CubicCameraMover::GetPov(int type, Pov& p) const {
    if (Car->Cameras == nullptr || !Car->Cameras->GetPov(type, Ctx->SplitScreen, p))
        throw std::runtime_error("car has no POV 0");
    return true;
}

bool CubicCameraMover::OutsidePOV() const { Pov p; GetPov(EffectivePov, p); return IsOutsideType(p.Type); }

bool CubicCameraMover::HighliteMode() const {
    if (!OutsidePOV()) return false;
    if (Car->HydraulicsBouncing || Car->HydraulicsExtended) return true;
    return Car->Mode != 0 && Car->WheelsOnGround <= 2;
}

void CubicCameraMover::ResetState(int realTimeFrames) {
    Up.Snap(); Fov.Snap(); Eye.Snap(); Look.Snap(); Forward.Snap();
    Camera->ResetVelocities = true;
    Camera->ResetFrame = realTimeFrames;
}

// ------------------------------------------------------------------ Update

void CubicCameraMover::Update() {
    Field54 = 0;
    int ebx = PovType;
    if (ebx == 4 || ebx == 2) {
        bool hl = OutsidePOV() && (Car->HydraulicsBouncing || Car->HydraulicsExtended || (Car->Mode != 0 && Car->WheelsOnGround <= 2));
        bool inCar = CameraWouldBeInCar();
        bool fallback = (Car->HasDriver && Car->DriverFlag2FB) || hl || inCar;
        if (!fallback && Ctx->ConstrainToTopology) {
            float p[3];
            float probe[3];
            // p = R * [ccb+0x110] + row3 + GeomPos (only used by the topology probe; inferido)
            std::memcpy(p, Car->GeomPos, sizeof(p));
            const float up3[3] = { Car->R[8], Car->R[9], Car->R[10] };
            BWareMath2::ScaleAdd(probe, p, up3, Data->LookUpOffset);
            fallback = Ctx->ConstrainToTopology(p, probe);
        }
        if (fallback) { Snap |= EffectivePov != 0; ebx = 0; }
        else Snap |= EffectivePov == 0;
    }
    EffectivePov = ebx;
    CubicPovData pd = Data->PovTables[ebx].Lookup(Data->PovLookupX);
    const RaceModeFlags& f = *Ctx->Flags;
    if (f.DriftByte != 0 || f.TweakIsDriftRace != 0 || f.BurnoutByte != 0 || f.TweakIsBurnout != 0) {
        pd.F[16] = 0.0f; pd.F[17] = 0.0f; pd.F[18] = 0.0f;
    }
    Averager.Add(Car->Accel);
    Up.X.Duration = pd.F[3]; Up.Y.Duration = pd.F[3]; Up.Z.Duration = pd.F[3];
    Fov.Duration = pd.F[2];
    Look.X.Duration = pd.F[1]; Look.Y.Duration = pd.F[1]; Look.Z.Duration = pd.F[1];
    Forward.SetDuration(pd.Vec(0xA0).data());
    float eyeDur = HighliteMode() ? (F24(pd.F[0]) / Data->HighliteScale).Store() : pd.F[0];
    Eye.X.Duration = eyeDur; Eye.Y.Duration = eyeDur; Eye.Z.Duration = eyeDur;
    float dt = Ctx->WorldTimeElapsed;
    Up.Update(dt); Fov.Update(dt); Eye.Update(dt); Look.Update(dt); Forward.Update(dt);

    Pov pov;
    GetPov(EffectivePov, pov);
    bool outside;
    const void* hidden;
    if (IsOutsideType(pov.Type)) { outside = true; hidden = nullptr; }
    else {
        outside = false;
        hidden = (!LookBack && EffectivePov == 4) ? nullptr : static_cast<const void*>(Car);
    }
    if (!ViewCamera::StopUpdating) Camera->HiddenCar = hidden;
    Camera->Current.NearZ = pov.Type == 3 ? Data->NearClipInCar : Data->NearClip;
    F24 dist = CameraMath::DistBetween(Camera->Current.Position, Car->Pos);
    if (!ViewCamera::StopUpdating) Camera->Current.TargetDistance = dist.Store();
    if (!NoDof) {
        if (!ViewCamera::StopUpdating) { Camera->Current.FocalDistance = Data->FocalDistance; Camera->Current.DepthOfField = Data->DepthOfField; }
    }
    else if (!ViewCamera::StopUpdating) { Camera->Current.FocalDistance = 0.0f; Camera->Current.DepthOfField = 0.0f; }

    float m[16] = {};
    SetDesired(m, pov, pd, Snap);
    Snap = false;
    float sign = (Field30 == 0 && LookBack && !(Ctx->HasPlayer && Ctx->UseBirdsEye)) ? -1.0f : 1.0f;
    float up[3] = { Up.X.Val, Up.Y.Val, CameraMath::One };
    if (!outside) std::memcpy(up, Car->UpR2, sizeof(up));
    float eye[3] = { (F24(sign) * Eye.X.Val).Store(), (F24(sign) * Eye.Y.Val).Store(), Eye.Z.Val };
    float look[3] = { (F24(sign) * Look.X.Val).Store(), (F24(sign) * Look.Y.Val).Store(), Look.Z.Val };
    CameraMath::MulVec3(eye, m, eye);
    CameraMath::MulVec3(look, m, look);
    float dz = (F24(eye[2]) - look[2]).Store();
    F24 k = Data->EyeLookScale;
    float dxs = ((F24(eye[0]) - look[0]) * k).Store();
    float dys = ((F24(eye[1]) - look[1]) * k).Store();
    F24 dzs = F24(dz) * k;
    eye[0] = (F24(dxs) + look[0]).Store();
    eye[1] = (F24(dys) + look[1]).Store();
    eye[2] = (dzs + look[2]).Store();
    // FOV: bATan(1.0, tan(fov/2)) * 2 (identity up to rounding; flt_78721C = 1.0)
    uint16_t half = static_cast<uint16_t>(static_cast<uint16_t>(X87::FtoL2Lo(Fov.Val)) >> 1);
    float s = BWareMath::Sin(half);
    float t = (F24(s) / BWareMath::Cos(half)).Store();
    uint16_t fov = static_cast<uint16_t>(Data->Math.ATan(Data->AspectFovScale, t) << 1);
    if (!ViewCamera::StopUpdating) Camera->Current.Fov = fov;
    float mat[16];
    CameraMath::LookAt(mat, eye, look, up);
    ApplyCameraShake(mat);
    float amp = Data->NoiseAmpByPov[EffectivePov], freq = Data->NoiseFreqByPov[EffectivePov];
    if (f.DragByte == 0 && f.TweakIsDragRace == 0) amp = (F24(amp) * Data->NoiseScaleNotDrag).Store();
    CameraNoise::TerrainVelocityNoise(*Data, *Camera, mat, *Car, amp, freq, Ctx->WorldTimer);
    if (outside) {
        if (Ctx->MinGapTopology) Ctx->MinGapTopology(mat);
        if (Ctx->MinGapCars) Ctx->MinGapCars(mat);
    }
    Camera->SetCameraMatrix(mat, dt);
}

// ApplyCameraShake(viewId, m) 0x572A50: views 1 and 3 use shaker 0, view 2 shaker 1.
void CubicCameraMover::ApplyCameraShake(float* m) {
    int idx = ViewId == 1 || ViewId == 3 ? 0 : ViewId == 2 ? 1 : -1;
    if (idx < 0) return;
    if (!BWareMath2::GeOrNaN(Ctx->WorldTimeScale, CameraMath::One)) return;
    Ctx->Shakers[idx].Apply(*Data, m, Ctx->WorldTimeScale);
}

// CameraWouldBeInCar 0x445FC0: eye point of POV +0x48 inside another car (World::IsInCar).
bool CubicCameraMover::CameraWouldBeInCar() const {
    if (!Ctx->IsInOtherCar) return false;
    Pov p;
    GetPov(PovType, p);
    float off[3];
    uint16_t angle;
    GetEyeOffsetAndAngle(p, off, angle);
    std::array<float, 16> m = CarMatrix();
    CameraMath::MulVec3(off, m.data(), off);
    return Ctx->IsInOtherCar(off, X87::Bits(0x3F000000u));
}

// Car::GetCarSpaceBoundingBox 0x401340 (misnamed): rows 0..2 of R, row 3 = (GeomPos, 1).
std::array<float, 16> CubicCameraMover::CarMatrix() const {
    std::array<float, 16> m = {};
    std::memcpy(m.data(), Car->R, 12 * sizeof(float));
    m[12] = Car->GeomPos[0]; m[13] = Car->GeomPos[1]; m[14] = Car->GeomPos[2]; m[15] = CameraMath::One;
    return m;
}

// GetEyeOffsetAndAngle 0x445EB0: (x, y, z) eye offset and look-down angle of a POV.
void CubicCameraMover::GetEyeOffsetAndAngle(const Pov& pov, float* off, uint16_t& angle) const {
    const RaceModeFlags& f = *Ctx->Flags;
    if (f.BurnoutByte != 0 || f.TweakIsBurnout != 0) {
        off[0] = Data->BurnoutEye[0]; off[1] = Data->BurnoutEye[1]; off[2] = Data->BurnoutEye[2];
        angle = Data->BurnoutAngle;
        return;
    }
    if (LookBack && Ctx->HasPlayer && Ctx->UseBirdsEye) {
        int i = Ctx->SplitScreen ? 1 : 0;
        off[0] = Data->BirdsEye[4 * i]; off[1] = Data->BirdsEye[4 * i + 1]; off[2] = Data->BirdsEye[4 * i + 2];
        angle = Data->BirdsEyeAngle[i];
        return;
    }
    if (Car->HasDriver && Car->DriverFlag2FB) {
        off[0] = Data->DriverFlagEye[0]; off[1] = Data->DriverFlagEye[1]; off[2] = Data->DriverFlagEye[2];
        angle = Data->DriverFlagAngle;
        return;
    }
    off[0] = pov.X; off[1] = pov.Y; off[2] = pov.Z;
    angle = pov.Angle;
}

// SetForward 0x445C20: desired heading = velocity direction blended with the car's forward by speed.
void CubicCameraMover::SetForward(const Pov& pov, bool snap) {
    const float fwd[3] = { Car->R[0], Car->R[1], Car->R[2] };
    if (IsOutsideType(pov.Type)) {
        if (!snap && HighliteMode()) return;
        float v[3] = { Car->Vel[0], Car->Vel[1], Car->Vel[2] };
        F24 dot = F24(v[2]) * fwd[2] + F24(v[1]) * fwd[1] + F24(v[0]) * fwd[0];
        if (!BWareMath2::GeOrNaN(dot, 0.0f))
            BWareMath2::ScaleAdd(v, v, fwd, (dot * Data->ReflectScale).Store());
        Graph g(std::vector<float>(Data->DriftSpeedGraph, Data->DriftSpeedGraph + 4), 2);
        float t = g.GetValue(Car->Speed).Store();
        BWareMath2::NormalizeX87(v, v);
        v[0] = (F24(v[0]) * t).Store(); v[1] = (F24(v[1]) * t).Store(); v[2] = (F24(v[2]) * t).Store();
        BWareMath2::ScaleAdd(v, v, fwd, (F24(CameraMath::One) - t).Store());
        int elapsed = static_cast<int>(static_cast<uint32_t>(Ctx->WorldTimer) - static_cast<uint32_t>(GroundTimer));
        F24 k = F24(CameraMath::One) - F24::MulDouble(Data->TickToSec, elapsed) / Data->ForwardFadeTime;
        float ks = k.Store();
        F24 kk;
        if (BWareMath2::LeOrNaN(ks, 0.0f)) kk = 0.0f;
        else kk = BWareMath2::GeOrNaN(CameraMath::One, ks) ? F24(ks) : F24(CameraMath::One);
        v[2] = (kk * v[2]).Store();
        Forward.SetValDesired(v);
        if (snap) Forward.Snap();
        return;
    }
    Forward.SetValDesired(fwd);
    Forward.Snap();
}

// MakeSpace 0x445E00: camera reference frame (smoothed heading for outside POVs, the car matrix otherwise).
void CubicCameraMover::MakeSpace(float* m) const {
    if (OutsidePOV()) {
        CameraMath::Identity(m);
        float row0[3];
        const float fv[3] = { Forward.X.Val, Forward.Y.Val, Forward.Z.Val };
        BWareMath2::NormalizeX87(row0, fv);
        m[0] = row0[0]; m[1] = row0[1]; m[2] = row0[2];
        float row1[3];
        const float r2[3] = { m[8], m[9], m[10] };
        BWareMath::CrossX87(row1, r2, row0);
        m[4] = row1[0]; m[5] = row1[1]; m[6] = row1[2];
        float row2[3];
        BWareMath::CrossX87(row2, row0, row1);
        m[8] = row2[0]; m[9] = row2[1]; m[10] = row2[2];
        m[12] = Car->GeomPos[0]; m[13] = Car->GeomPos[1]; m[14] = Car->GeomPos[2]; m[15] = CameraMath::One;
        return;
    }
    std::array<float, 16> cm = CarMatrix();
    std::memcpy(m, cm.data(), 16 * sizeof(float));
}

// SetDesired(m, pov, data, snap) 0x44DE90: desired eye/look/up/FOV for this frame.
void CubicCameraMover::SetDesired(float* m, const Pov& pov, const CubicPovData& pd, bool snap) {
    SetForward(pov, snap);
    MakeSpace(m);
    float inv[16];
    CameraMath::InvertTransformation(inv, m);
    float lag[4] = {};
    if (LagEnabled) {
        lag[0] = Averager.Avg[0]; lag[1] = Averager.Avg[1]; lag[2] = Averager.Avg[2]; lag[3] = 0.0f;
        CameraMath::MulVec4(lag, inv, lag);
    }
    if (Car->OnTopology && Car->WheelsOnGround > 1) GroundTimer = Ctx->WorldTimer;
    float off[3];
    uint16_t angle;
    GetEyeOffsetAndAngle(pov, off, angle);
    float sa = BWareMath::Sin(angle);
    F24 tn = F24(sa) / BWareMath::Cos(angle);
    float lookZ, lookX;
    if (IsOutsideType(pov.Type)) { lookZ = (F24(off[0]) * tn + off[2]).Store(); lookX = 0.0f; }
    else { lookZ = (F24(off[2]) - tn).Store(); lookX = (F24(off[0]) + CameraMath::One).Store(); }
    const float* F = pd.F;
    float eye[3] = { (F24(lag[0]) * F[16]).Store(), (F24(lag[1]) * F[17]).Store(), (F24(lag[2]) * F[18]).Store() };
    CameraMath::ClampVec(eye, pd.Vec(0x50).data(), pd.Vec(0x60).data());
    F24 oy, oz;
    bool hl = OutsidePOV() && (Car->HydraulicsBouncing || Car->HydraulicsExtended || (Car->Mode != 0 && Car->WheelsOnGround <= 2));
    if (hl) {
        off[0] = (F24(Data->HighliteScale) * off[0]).Store();
        oy = F24(off[1]) * Data->HighliteScale;
        oz = F24(Data->HighliteScale) * off[2];
    }
    else { oy = off[1]; oz = off[2]; }
    eye[0] = (F24(eye[0]) + off[0]).Store();
    eye[1] = (oy + eye[1]).Store();
    eye[2] = (F24(eye[2]) + oz).Store();
    Eye.SetValDesired(eye);
    float look[3] = { (F24(lag[0]) * F[28]).Store(), (F24(lag[1]) * F[29]).Store(), (F24(lag[2]) * F[30]).Store() };
    CameraMath::ClampVec(look, pd.Vec(0x80).data(), pd.Vec(0x90).data());
    look[0] = (F24(look[0]) + lookX).Store();
    look[2] = (F24(look[2]) + lookZ).Store();
    Look.SetValDesired(look);
    const float* avg = Averager.Avg;
    F24 uy = F24(F[5]) * avg[1];
    F24 uz = F24(F[6]) * avg[2] + CameraMath::One;
    float upv[3] = { (F24(avg[0]) * F[4]).Store(), uy.Store(), uz.Store() };
    CameraMath::ClampVec(upv, pd.Vec(0x20).data(), pd.Vec(0x30).data());
    Up.SetValDesired(upv);
    Fov.SetValDesired(static_cast<float>(Data->PovFov[pov.Type]));
    // nWhichChaseCamera (0x7EBEC0) == 2: MaybePullBackToSeeChaseCar is never called
    bool bumperOrInCar = pov.Type == 2 || pov.Type == 3;
    // the snap below tests the mover flag +0x52, not the argument (the argument only reaches SetForward)
    if (Snap || bumperOrInCar) {
        Up.Snap(); Fov.Snap(); Eye.Snap(); Look.Snap();
    }
    if (Snap) Ctx->FlushAccumulationCount++;
}

} // namespace bb
