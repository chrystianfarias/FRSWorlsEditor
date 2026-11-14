#include "drive/physics/CarState.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/PhysicsMover.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/engine/EngineLinks.h"
#include "drive/physics/engine/EngineTables.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cmath>

namespace bb {

namespace {
inline float A(float x, float y) { return BWareMath::A(x, y); }
inline float S(float x, float y) { return BWareMath::S(x, y); }
inline float M(float x, float y) { return BWareMath::M(x, y); }
} // namespace

// ------------------------------------------------------------------ CarState

void CarState::BeginTimestep() {
    for (int i = 0; i < 4; i++) MaxWheelTerrain[i] = 1;
}

void CarState::SetWheelTouchingGround(int i, uint8_t v) {
    uint8_t old = WheelOnGround[i];
    WheelOnGround[i] = v;
    if (old == 0) { if (v != 0) WheelsOnGround++; }
    else if (v == 0) WheelsOnGround--;
}

void CarState::SetWheelTerrain(int i, int t) {
    WheelTerrain[i] = t;
    if (t > MaxWheelTerrain[i]) MaxWheelTerrain[i] = t;
}

bool CarState::IsDriveWheel(int i) const {
    float split = Cpi->RearTorqueSplit();
    // fld [cpi+1C0h]; fcomp 0.2; test ah,5; jp -> second test when split >= 0.2 (or NaN)
    if (split < WheelK::FwdSplit) return i == 1 || i == 0;
    // fcomp 0.8; test ah,41h; jnz -> drive wheel when split <= 0.8 (or NaN)
    if (!(split > WheelK::RwdSplit)) return true;
    return i == 2 || i == 3;
}

float CarState::GetSlipAngle(const BWareMath& math) const {
    uint16_t a = math.FastATan(Vel[0], Vel[1]);
    uint16_t d = static_cast<uint16_t>(Heading - a);
    float r = M(static_cast<float>(d), EngineK::AngToRad);              // fild (16-bit value, exact) ; fmul
    if ((X87::Fcom(r, EngineK::Pi) & 0x41) == 0) r = S(r, A(EngineK::Pi, EngineK::Pi));
    return r;
}

void CarState::CalculateGeometryPosition(const float* cg) {
    float t[3];
    BWareMath2::MulVector3X87(t, R, cg != nullptr ? cg : Cpi->Cg);
    F24 x = F24(Pos[0]) - t[0];
    F24 y = F24(Pos[1]) - t[1];
    float z = (F24(Pos[2]) - t[2]).Store();
    GeomPos[2] = z;
    GeomPos[0] = x.Store();
    GeomPos[1] = y.Store();
}

// ------------------------------------------------------------------ Car

Car::Car() : collisionOwned_(std::make_unique<CarCollisionBody>()) {
    Collision = collisionOwned_.get();
    Collision->Car = this;
}

Car::~Car() = default;

bool Car::IsBlinking(int worldTimer) const {
    if (BlinkStart == 0 || BlinkStart == 0x7FFFFFFF) return false;
    float e = EngineMath::FildMul(static_cast<int>(static_cast<uint32_t>(worldTimer) - static_cast<uint32_t>(BlinkStart)), EngineK::TickToSec);
    return !X87::ParityEven(X87::Fcom(e, BlinkDuration) & 0x05);     // elapsed < duration
}

bool Car::AreHydraulicsBouncing() const {
    const float* h = Driver->Hop;
    if (!(h[0] <= 0.0f || std::isnan(h[0]))) return true;              // fcomp 0; test ah,41h; jz
    if (!(h[1] <= 0.0f || std::isnan(h[1]))) return true;
    if (!(h[2] <= 0.0f || std::isnan(h[2]))) return true;
    return !(h[3] <= 0.0f || std::isnan(h[3]));
}

int Car::GetHydraulicState(int i) const { return MovementMode == 1 ? Mover->HydraulicState[i] : 0; }

bool Car::NoDriveWheelOnGround() const {
    for (int i = 0; i < 4; i++)
        if (State.WheelOnGround[i] != 0 && State.IsDriveWheel(i)) return false;
    return true;
}

bool Car::IsSupported() const {
    if (State.WheelsOnGround > 0) return true;
    int n = Collision->Contacts;
    for (int i = 0; i < n; i++) {
        auto* c = Collision->World != nullptr ? Collision->World->NthCollision(Collision, i) : nullptr;
        if (c != nullptr && c->IsFlatGroundInvolved()) return true;
    }
    return false;
}

} // namespace bb
