// Port of NFSU2Unity Core/Physics/Collision/CollisionBody.cs.
#include "drive/physics/collision/CollisionBody.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/rigidbody/RigidBody.h"

namespace bb {

// ---------------------------------------------------------------------------------------------- CollisionBody

float* CollisionBody::GetPosition() { return Zero3; }
float* CollisionBody::GetVelocity() { return Zero3; }
float* CollisionBody::GetRotation() { return Identity; }
float* CollisionBody::GetAngularVelocity() { return Zero3; }
float* CollisionBody::GetCenterOfGravity() { return Zero3; }

void CollisionBody::Interpolate(float* pos, float* rot, float /*t*/)
{
    BWareMath2::CopyMatrix(rot, GetRotation());
    const float* p = GetPosition();
    pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
}

void CollisionBody::GetCurrentVelocities(float* lin, float* ang)
{
    const float* v = GetVelocity();
    lin[0] = v[0]; lin[1] = v[1]; lin[2] = v[2];
    const float* w = GetAngularVelocity();
    ang[0] = w[0]; ang[1] = w[1]; ang[2] = w[2];
}

bool CollisionBody::Overlaps(const CollisionBody& o, float margin) const
{
    if ((BWareMath2::Fcom(F24(margin) + o.MaxX, F24(MinX)) & 0x01) != 0) return false;
    if (X87::ParityEven(BWareMath2::Fcom(F24(o.MinX) - margin, F24(MaxX)) & 0x41)) return false;
    if ((BWareMath2::Fcom(F24(margin) + o.MaxY, F24(MinY)) & 0x01) != 0) return false;
    if (X87::ParityEven(BWareMath2::Fcom(F24(o.MinY) - margin, F24(MaxY)) & 0x41)) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------- DynamicCollisionBody

void DynamicCollisionBody::SetPosition(const float* p)
{
    float* d = GetPosition();
    d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
}

void DynamicCollisionBody::BeginMove(float dt)
{
    BWareMath2::CopyMatrix(StartRot, GetRotation());
    const float* p = GetPosition();
    StartPos[0] = p[0]; StartPos[1] = p[1]; StartPos[2] = p[2];
    const float* v = GetVelocity();
    StartVel[0] = v[0]; StartVel[1] = v[1]; StartVel[2] = v[2];
    BackupDt = dt;
}

void DynamicCollisionBody::Interpolate(float* pos, float* rot, float t)
{
    const float* cp = GetPosition();
    const float* cr = GetRotation();
    if (BWareMath2::GeOrNaN(BackupDt, CollisionK::Eps1e6)) {
        float s = ((F24(BackupDt) - t) / BackupDt).Store();
        const float dp[3] = { (F24(cp[0]) - StartPos[0]).Store(), (F24(cp[1]) - StartPos[1]).Store(), (F24(cp[2]) - StartPos[2]).Store() };
        float dr[16];
        for (int k = 0; k < 16; k++) dr[k] = (F24(cr[k]) - StartRot[k]).Store();
        BWareMath2::ScaleAdd(pos, StartPos, dp, s);
        for (int k = 0; k < 16; k++) rot[k] = (F24(dr[k]) * s + StartRot[k]).Store();
        return;
    }
    pos[0] = cp[0]; pos[1] = cp[1]; pos[2] = cp[2];
    BWareMath2::CopyMatrix(rot, cr);
}

void DynamicCollisionBody::VelocityBack(float* o, float t)
{
    const float* v = GetVelocity();
    if (BWareMath2::GeOrNaN(BackupDt, CollisionK::Eps1e6)) {
        float s = ((F24(BackupDt) - t) / BackupDt).Store();
        const float d[3] = { (F24(v[0]) - StartVel[0]).Store(), (F24(v[1]) - StartVel[1]).Store(), (F24(v[2]) - StartVel[2]).Store() };
        BWareMath2::ScaleAdd(o, StartVel, d, s);
        return;
    }
    o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
}

void DynamicCollisionBody::Backup(float t)
{
    float pos[3];
    float rot[16];
    Interpolate(pos, rot, t);
    float vel[3];
    VelocityBack(vel, t);
    if (Rb == nullptr) return;                              // vtbl+0x78 (_return_0x10) for bodies without one
    F24 m = Rb->Mass;
    float px = (m * vel[0]).Store(), py = (F24(vel[1]) * m).Store(), pz = (F24(vel[2]) * m).Store();
    Rb->Pos[0] = pos[0]; Rb->Pos[1] = pos[1]; Rb->Pos[2] = pos[2];
    BWareMath2::CopyMatrix(Rb->R, rot);
    Rb->P[0] = px; Rb->P[1] = py; Rb->P[2] = pz;
}

void DynamicCollisionBody::GetCurrentVelocities(float* lin, float* ang)
{
    if (PhysicsEnabled) {
        lin[0] = Rb->Vel[0]; lin[1] = Rb->Vel[1]; lin[2] = Rb->Vel[2];
        ang[0] = Rb->AngVel[0]; ang[1] = Rb->AngVel[1]; ang[2] = Rb->AngVel[2];
        return;
    }
    CollisionBody::GetCurrentVelocities(lin, ang);
}

void DynamicCollisionBody::UpdatePosition()
{
    const float* p = GetPosition();
    float minX = (F24(p[0]) - Radius).Store(), minY = (F24(p[1]) - Radius).Store();
    float maxX = (F24(Radius) + p[0]).Store(), maxY = (F24(p[1]) + Radius).Store();
    if (World != nullptr) World->SetBox(this, minX, minY, maxX, maxY);
    else { MinX = minX; MinY = minY; MaxX = maxX; MaxY = maxY; }
    float c[3];
    BWareMath2::MulVector3X87(c, StartRot, LocalCenter);
    Center[0] = (F24(c[0]) + StartPos[0]).Store();
    Center[1] = (F24(StartPos[1]) + c[1]).Store();
    Center[2] = (F24(StartPos[2]) + c[2]).Store();
}

void DynamicCollisionBody::BuildBoundingVolumes()
{
    float r3 = 0, rxy = 0, zMax = 0, zMin = 0, xMax = 0, xMin = 0, yMax = 0, yMin = 0, z2Max = 0, z2Min = 0;
    GjkVertex v;
    for (int i = 0; i < Shape->Count; i++) {
        Shape->GetPoint(i, v);
        F24 sxy = F24(v.Y) * v.Y + F24(v.X) * v.X;
        float r = F24::Sqrt(sxy).Store();
        F24 rr = F24::Sqrt(sxy + F24(v.Z) * v.Z);
        if ((BWareMath2::Fcom(rr, F24(r3)) & 0x41) == 0) r3 = rr.Store();
        if ((BWareMath2::Fcom(r, rxy) & 0x41) == 0) rxy = r;
        if ((BWareMath2::Fcom(v.Z, zMax) & 0x41) == 0) zMax = v.Z;
        else if (!BWareMath2::GeOrNaN(v.Z, zMin)) zMin = v.Z;
        if ((BWareMath2::Fcom(v.X, xMax) & 0x41) == 0) xMax = v.X;
        else if (!BWareMath2::GeOrNaN(v.X, xMin)) xMin = v.X;
        if ((BWareMath2::Fcom(v.Y, yMax) & 0x41) == 0) yMax = v.Y;
        else if (!BWareMath2::GeOrNaN(v.Y, yMin)) yMin = v.Y;
        if ((BWareMath2::Fcom(v.Z, z2Max) & 0x41) == 0) z2Max = v.Z;
        else if (!BWareMath2::GeOrNaN(v.Z, z2Min)) z2Min = v.Z;
    }
    Radius = r3;
    ExtentRxy = (F24(CollisionK::RadiusMargin) + rxy).Store();
    ExtentZ = (F24(CollisionK::RadiusMargin) + zMax).Store();
    ExtentNegZ = (F24(CollisionK::RadiusMargin) - zMin).Store();
    VolMin[0] = xMin; VolMin[1] = yMin; VolMin[2] = z2Min;
    VolMax[0] = xMax; VolMax[1] = yMax; VolMax[2] = z2Max;
    float bmin[3] = {};
    float bmax[3] = {};
    if (auto* gp = dynamic_cast<GjkPolytope*>(Shape)) gp->GetBoundingBox(bmin, bmax);
    else if (auto* gb = dynamic_cast<GjkBox*>(Shape)) {
        bmin[0] = (F24(gb->Center[0]) - gb->Half[0]).Store(); bmin[1] = (F24(gb->Center[1]) - gb->Half[1]).Store(); bmin[2] = (F24(gb->Center[2]) - gb->Half[2]).Store();
        bmax[0] = (F24(gb->Half[0]) + gb->Center[0]).Store(); bmax[1] = (F24(gb->Half[1]) + gb->Center[1]).Store(); bmax[2] = (F24(gb->Half[2]) + gb->Center[2]).Store();
    }
    LocalCenter[0] = ((F24(bmax[0]) + bmin[0]) * CollisionK::Half).Store();
    LocalCenter[1] = ((F24(bmax[1]) + bmin[1]) * CollisionK::Half).Store();
    LocalCenter[2] = ((F24(bmax[2]) + bmin[2]) * CollisionK::Half).Store();
}

} // namespace bb
