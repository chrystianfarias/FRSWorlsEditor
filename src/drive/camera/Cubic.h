#pragma once

// Port of NFSU2Unity Core/Camera/Cubic.cs.

#include "drive/numerics/X87.h"

#include <array>
#include <cstdint>

namespace bb {

// tCubic1D (0x30 bytes): Hermite-style cubic from (Val, dVal) to (Des, dDes) over Duration, dVal in units per
// normalised time. The camera ctor builds every curve with Flag = 1 and TMax = 10: the curve is re-fitted from
// the current value and slope on every Update (State stays 2), which is the camera "lag spring".
class Cubic1D {
public:
    float Val = 0;        // +0x00
    float dVal = 0;       // +0x04
    float Des = 0;        // +0x08
    float dDes = 0;       // +0x0C
    float A = 0, B = 0, C = 0, D = 0; // +0x10..+0x1C polynomial coefficients
    float T = 0;          // +0x20 normalised time
    float Duration = 0;   // +0x24
    float TMax = 0;       // +0x28
    int16_t State = 0;    // +0x2C 0 idle, 1 running, 2 restart
    int16_t Flag = 0;     // +0x2E 1 = stay in state 2 (re-fit every update)

    static inline const float K_Eps = X87::Bits(0x3727C5ACu);   // flt_7845A0 1e-5
    static inline const float K_Three = X87::Bits(0x40400000u); // flt_787ADC 3.0

    // sub_4419A0 / sub_441A20 per component: zeroed curve.
    void Init(int16_t flag, float duration, float tmax);

    // tCubic1D::Update(dt, 0, 0) 0x5749E0 (the limiters sub_5748C0/sub_574910 only run with arguments > 0).
    void Update(float dt);

    // sub_570BA0 per component: Des = v; restart when v != Val (or NaN).
    void SetValDesired(float v);

    // sub_570AE0 per component: Val = v; restart when v != Des.
    void SetVal(float v);

    // sub_570B40 per component: dVal = v; restart when v != dDes.
    void SetdVal(float v);

    // Inline "snap" used by SetDesired/ResetState: Val = Des, dVal = dDes, State = 0.
    void Snap() { Val = Des; dVal = dDes; State = 0; }
};

// tCubic3D (0x90 bytes) = three tCubic1D (X +0x00, Y +0x30, Z +0x60).
class Cubic3D {
public:
    Cubic1D X, Y, Z;

    Cubic1D& operator[](int i) { return i == 0 ? X : i == 1 ? Y : Z; }

    // sub_4419A0(flag, duration, tmax).
    void Init(int16_t flag, float duration, float tmax) { X.Init(flag, duration, tmax); Y.Init(flag, duration, tmax); Z.Init(flag, duration, tmax); }

    // sub_441A20(flag, &durations, tmax).
    void Init(int16_t flag, const float* durations, float tmax) {
        X.Init(flag, durations[0], tmax); Y.Init(flag, durations[1], tmax); Z.Init(flag, durations[2], tmax);
    }

    // tCubic3D::Update(dt, 0, 0) 0x574DF0.
    void Update(float dt) { X.Update(dt); Y.Update(dt); Z.Update(dt); }

    // tCubic3D::SetValDesired 0x574CD0.
    void SetValDesired(const float* v) { X.SetValDesired(v[0]); Y.SetValDesired(v[1]); Z.SetValDesired(v[2]); }

    // sub_574C90.
    void SetVal(const float* v) { X.SetVal(v[0]); Y.SetVal(v[1]); Z.SetVal(v[2]); }

    // sub_574CB0.
    void SetdVal(const float* v) { X.SetdVal(v[0]); Y.SetdVal(v[1]); Z.SetdVal(v[2]); }

    // tCubic3D::SetDuration 0x574D10.
    void SetDuration(const float* d) { X.Duration = d[0]; Y.Duration = d[1]; Z.Duration = d[2]; }

    void Snap() { X.Snap(); Y.Snap(); Z.Snap(); }

    std::array<float, 3> Value() const { return { X.Val, Y.Val, Z.Val }; }
};

} // namespace bb
