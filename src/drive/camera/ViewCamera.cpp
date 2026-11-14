// Port of NFSU2Unity Core/Camera/ViewCamera.cs.

#include "drive/camera/ViewCamera.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"

#include <cstring>

namespace bb {

namespace {

void Rate3(float* o, const float* cur, const float* prev, F24 inv) {
    o[0] = (F24(cur[0]) - prev[0]).Store();
    o[1] = (F24(cur[1]) - prev[1]).Store();
    o[2] = (F24(cur[2]) - prev[2]).Store();
    o[3] = 0.0f;
    o[0] = (inv * o[0]).Store();
    o[1] = (inv * o[1]).Store();
    o[2] = (inv * o[2]).Store();
}

} // namespace

ViewCamera ViewCamera::CreateDefault() {
    ViewCamera c;
    CameraMath::Identity(c.Current.Matrix);
    c.Current.TargetDistance = 10.0f;
    c.Current.Fov = 0x36FB;
    c.Current.NearZ = 0.5f;
    c.Current.FarZ = 10000.0f;
    c.Previous.CopyFrom(c.Current);
    return c;
}

void ViewCamera::SetCameraMatrix(const float* m, float elapsed) {
    if (StopUpdating) return;
    CameraKey& k = Current;
    Previous.CopyFrom(k);
    ElapsedTime = elapsed;
    std::memcpy(k.Matrix, m, 16 * sizeof(float));
    const float* M = k.Matrix;
    // T = transpose(M) (eTransposeMatrix -> D3DX transpose, pure copies)
    F24 x = F24(M[0]) * M[12] + F24(M[1]) * M[13] + F24(M[2]) * M[14] + M[3];
    F24 y = F24(M[4]) * M[12] + F24(M[5]) * M[13] + F24(M[6]) * M[14] + M[7];
    float z = (F24(M[8]) * M[12] + F24(M[9]) * M[13] + F24(M[10]) * M[14] + M[11]).Store();
    k.Position[0] = -x.Store(); k.Position[1] = -y.Store(); k.Position[2] = -z;
    float dir[3];
    const float col2[3] = { M[2], M[6], M[10] };
    BWareMath2::NormalizeX87(dir, col2);
    k.Direction[0] = dir[0]; k.Direction[1] = dir[1]; k.Direction[2] = dir[2];
    F24 dist = k.TargetDistance;
    float tx = (dist * dir[0]).Store(), ty = (F24(dir[1]) * dist).Store(), tz = (F24(dir[2]) * dist).Store();
    k.Target[0] = (F24(tx) + k.Position[0]).Store();
    k.Target[1] = (F24(k.Position[1]) + ty).Store();
    k.Target[2] = (F24(k.Position[2]) + tz).Store();
    if (ResetVelocities) {
        ResetVelocities = false;
        Previous.CopyFrom(k);
        ElapsedTime = CameraMath::One;
    }
    if (BWareMath2::LeOrNaN(ElapsedTime, 0.0f)) return;
    F24 inv = F24(CameraMath::One) / ElapsedTime;
    Rate3(PositionRate, k.Position, Previous.Position, inv);
    Rate3(DirectionRate, k.Direction, Previous.Direction, inv);
    Rate3(TargetRate, k.Target, Previous.Target, inv);
    int dfov = static_cast<uint16_t>(k.Fov - Previous.Fov);
    ScalarRate[0] = ((F24(k.TargetDistance) - Previous.TargetDistance) * inv).Store();
    ScalarRate[1] = ((F24(k.FocalDistance) - Previous.FocalDistance) * inv).Store();
    ScalarRate[2] = ((F24(k.DepthOfField) - Previous.DepthOfField) * inv).Store();
    ScalarRate[3] = ((F24(k.NearZ) - Previous.NearZ) * inv).Store();
    ScalarRate[4] = ((F24(k.FarZ) - Previous.FarZ) * inv).Store();
    FovRate = static_cast<uint16_t>(X87::FtoL2Lo((F24::MulDouble(1.0f, dfov) * inv).V));
    for (int b = 0; b < 16; b += 4)
        for (int i = 0; i < 4; i++)
            NoiseRate[b + i] = (inv * (F24(k.Noise[b + i]) - Previous.Noise[b + i]).Store()).Store();
}

} // namespace bb
