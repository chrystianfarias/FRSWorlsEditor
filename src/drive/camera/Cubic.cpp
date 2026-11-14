// Port of NFSU2Unity Core/Camera/Cubic.cs.

#include "drive/camera/Cubic.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"

namespace bb {

void Cubic1D::Init(int16_t flag, float duration, float tmax) {
    Val = dVal = Des = dDes = 0.0f; State = 0; Flag = flag; Duration = duration; TMax = tmax;
}

void Cubic1D::Update(float dt) {
    if (State != 1 && State != 2) return;
    if (State == 2) {
        T = 0.0f;
        if (Flag == 0) State = 1;
        F24 d = F24(Des) - Val;
        F24 s = F24(dDes) + dVal;
        D = Val; C = dVal;
        A = (s - (d + d)).Store();
        B = (d * K_Three - dDes - (F24(dVal) + dVal)).Store();
    }
    if (!BWareMath2::LeOrNaN(Duration, K_Eps)) T = (F24(dt) / Duration + T).Store();
    else T = CameraMath::One;
    if (!BWareMath2::LtOrNaN(T, TMax)) {
        State = 0;
        if (BWareMath2::LtOrNaN(TMax, CameraMath::One)) return;
        Val = Des; dVal = dDes;
        return;
    }
    F24 t = T;
    F24 ta = t * A;
    Val = (((ta + B) * t + C) * t + D).Store();
    dVal = ((ta * K_Three + (F24(B) + B)) * t + C).Store();
}

void Cubic1D::SetValDesired(float v) {
    Des = v;
    if (BWareMath2::NeOrNaN(v, Val)) State = 2;
}

void Cubic1D::SetVal(float v) {
    Val = v;
    if (BWareMath2::NeOrNaN(v, Des)) State = 2;
}

void Cubic1D::SetdVal(float v) {
    dVal = v;
    if (BWareMath2::NeOrNaN(v, dDes)) State = 2;
}

} // namespace bb
