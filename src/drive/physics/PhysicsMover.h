#pragma once

// Port of NFSU2Unity Core/Physics/PhysicsMover.cs, PhysicsMoverForces.cs and PhysicsMoverTuning.cs
// (one C# partial class; the three parts are PhysicsMover.cpp, PhysicsMoverForces.cpp, PhysicsMoverTuning.cpp).

#include "drive/numerics/X87.h"
#include "drive/physics/rigidbody/RigidBody.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace bb {

class Aerodynamics;
class Car;
class CarDriver;
class CarPhysicsInfo;
class CarState;
class DriveTrain;
class Engine;
class MoverData;
class PhysicsEnvironment;
class Suspension;
class TwoWheelAckermanSteering;
class Wheel;
class World;

// PhysicsMover (0x130 bytes, vtbl 0x7A25E0) of SPEED2.EXE v1.2: the car mover of MovementMode 1. Owns the
// physics components and runs PhysicsMover::Move 0x5B72D0 (explicit Euler over the 50-float state vector
// RB(32), W0,S0,W1,S1,W2,S2,W3,S3 (2 each), Engine(1), DriveTrain(1), with sub-steps of at most
// MaximumTimeStep). All arithmetic follows the x87 instruction order (F24 on the stack, Store() per fstp).
// Spec: docs/re/physics-overview.md 1.6, 3.3, appendix C 4. Port status: docs/re/port-status-integration.md.
// The component pointers below are owned by the mover (built by the full constructor).
class PhysicsMover {
public:
    static constexpr int StateFloats = 50;

    PhysicsEnvironment* const Env;
    CarDriver* Driver = nullptr;                         // +0x004
    // +0x008 PhysicsSystem (vtbl PhysicsVehicle 0x7A25D4), +0x00C "PhysMov", +0x014 component list
    int StateSize = 0;                                   // +0x01C sum of GetSize() (50)
    RigidBody* Rb = nullptr;                             // +0x020
    TwoWheelAckermanSteering* Steering = nullptr;        // +0x024
    Suspension* Susp[4] = {};                            // +0x028
    Wheel* Wheels[4] = {};                               // +0x038
    bb::Engine* Engine = nullptr;                        // +0x048
    bb::DriveTrain* DriveTrain = nullptr;                // +0x04C
    Aerodynamics* Aero = nullptr;                        // +0x050
    CarPhysicsInfo* Cpi = nullptr;                       // +0x054
    CarState* State = nullptr;                           // +0x058 (= Car+0x40)
    bb::Car* Car = nullptr;                              // +0x05C
    int DampState = 0;                                   // +0x060 DampingSuitability 0/1/2
    bool CgShifted = false;                              // +0x064 (byte)
    int RoofResetTimer = 0;                              // +0x068 WorldTimer stamp (0 = off)
    float GripScale = 0;                                 // +0x06C (1.0 in the ctor; Car+0x4D8 after SetMovementMode)
    float EBrakeRearGrip = 0;                            // +0x070
    int HydraulicState[4] = {};                          // +0x074 0 idle, 1 held, 2 rising, 3 falling
    bool IsOnDyno = false;                               // +0x084 (byte)
    float DynoAnchor[3] = {};                            // +0x090
    float DynoPos[3] = {};                               // +0x0E0
    float DynoVel[3] = {};                               // +0x0F0
    float AccForce[3] = {};                              // +0x100 mean of RB.LastForce over the last Move
    float AccTorque[3] = {};                             // +0x110 mean of RB.LastTorque
    int Field124 = 0;                                    // +0x124 (0 in the ctor)

    // Empty mover (fields assigned by the caller); used by tests that wire partial object graphs.
    explicit PhysicsMover(PhysicsEnvironment* env);

    // PhysicsMover::PhysicsMover(CarPhysicsInfo*, CarTypeInfo*, CarDriver*, Car*) 0x5B5CC0.
    // carTypeD8 = (s8)CarTypeInfo+0xD8 (RigidBody+0x15C top-speed flag, Engine+0x74 torque flag).
    // Fields the original leaves to the slot pools start at 0. Car::SetMovementMode then sets
    // GripScale = Car+0x4D8 (not done here, as in the original ctor).
    PhysicsMover(PhysicsEnvironment* env, CarPhysicsInfo* cpi, int8_t carTypeD8, CarDriver* driver, bb::Car* car);

    ~PhysicsMover();
    PhysicsMover(const PhysicsMover&) = delete;
    PhysicsMover& operator=(const PhysicsMover&) = delete;

    // PhysicsMover::InitializeStateFromCar 0x5A48A0: RigidBody::SetState from CarState (+0x20 pos, +0x50 R,
    // +0x30 vel, +0xD0 angVel), suspension state 0, wheel angle/omega from +0x300/+0x2F0, engine omega from the
    // smoothed rpm (or TestRPM), drivetrain gear = CarState+0x3F4 with the clutch engaged.
    void InitializeStateFromCar();

    // ------------------------------------------------------------------ Move (PhysicsMover.cpp)

    // PhysicsMover::Move(float dt) 0x5B72D0. Returns dt. Not modelled: bGetTicker (profiling), Wheel::MaybeMakeSkids
    // (visual) and the online clock sync at the end.
    float Move(float dt);

    // PhysicsSystem::IntegrateEuler(float h) 0x5AA480 on this mover: GetDerivative of every component in list
    // order, s = h*d + s, then PhysicsMover::ComputeAuxiliaries (auxiliaries, BottomOut, ResolveForces).
    void IntegrateEuler(float h);

    // PhysicsSystem::ComputeAuxiliaries 0x5A0D80: ComputeAuxiliaries of every component, in list order.
    void SystemComputeAuxiliaries();

    // PhysicsMover::ComputeAuxiliaries 0x5B04D0: component auxiliaries, BottomOut, ResolveForces.
    void ComputeAuxiliaries();

    // PhysicsMover::BottomOut 0x5ABB00: rest/roof-reset damping; bump-stop impulses when not damping; if anything
    // changed the state, UglyTimestepHack = 0 and the auxiliaries are recomputed (without ResolveForces).
    void BottomOut();

    // GetDampState 0x5B0490.
    int GetDampState() const { return DampState; }

    // ------------------------------------------------------------------ forces (PhysicsMoverForces.cpp)

    // PhysicsMover::ResolveForces 0x5B0520 (PhysicsVehicle vtbl+8, this = mover+8).
    void ResolveForces();

    // PhysicsMover::IterateForces 0x5ABDF0: first clutch and tyre guesses, then 5 passes that correct each
    // non-sliding wheel's longitudinal force for the body acceleration implied by the current forces, with the
    // clutch re-solved after each pass; finally the friction circle and the clutch lock.
    void IterateForces();

    // PhysicsMover::JumpStabilizer 0x5A5B60: spin damping while (almost) airborne and spinning slowly.
    void JumpStabilizer();

    // Car::MaybeForceYaw 0x5ECF70: yaw assist requested by the driver (vtbl+0x84; 0 for the RealDriver, which
    // makes the function a no-op for the player after its early checks).
    void MaybeForceYaw();

    // PhysicsMover::DriftStabilizer 0x5AC260 (drift modes, not in drag): yaw assist and rear grip.
    void DriftStabilizer();

    // PhysicsMover::ResolveHydraulicForces 0x5A51B0 (lowrider heights and hops).
    void ResolveHydraulicForces();

    // PhysicsMover::ResolveDynoForces 0x5A5C20: spring/damper to the dyno anchor; non-driven wheels roll with it.
    void ResolveDynoForces();

    // ------------------------------------------------------------------ tuning (PhysicsMoverTuning.cpp)

    // PhysicsMover::Calibrate 0x5AB990: hydraulic CG, inertia tensor, static wheel load (weight + aero downforce
    // at the current speed, split front/rear by the CG position) as the suspension preload, wheel inertia.
    void Calibrate();

    // FixCarCenterOfGravity(Car*, RigidBody*, bool*) 0x5A60F0: in Car mode 1 the CG height follows the hydraulic
    // heights of the collision body; otherwise a shifted CG is restored to CarPhysicsInfo.Cg.
    bool FixCarCenterOfGravity();

    // PhysicsMover::TuneBoostAndDampers 0x5A49A0: terrain drag, damping/gravity per Car mode, yaw-scaled inertia,
    // race-start heat and rest damping, catch-up scales, yaw torque clamp, top speed, rear torque split.
    void TuneBoostAndDampers();

    // sub_599170 (ApplyCatchupScale, inferred): catch-up / slipstream scales.
    void ApplyCatchupScale(float s);

    // PhysicsMover::DelegateDriverInput 0x5ABBF0: pedal dominance, throttle dead zone and anti-stall, steering,
    // gear request, nitrous level, automatic gearbox (sub_5AA960 unless selectGear is given),
    // aero throttle and ground fraction.
    void DelegateDriverInput(const std::function<int(int, float)>& selectGear = nullptr);

    // PhysicsMover::TuneWheelParams(float dt) 0x5A4D80: brake distribution (launch brake), handbrake and its
    // rear grip, rain, rear grip boost, flat tyres, nitrous grip, dyno.
    void TuneWheelParams(float dt);

    // sub_5990C0 (RearGripBoost, inferred, cdecl): rear grip factor >= 1 in corners.
    F24 RearGripBoost(float slip, float ebrake, float speed, float baseGrip, float corner, int pilotFlag, float& pitch, float hitGrip) const;

    // sub_5EC790 (Car::RecentHitGrip, inferred): Graph unk_89CCE4 of the seconds since the last hit (< 4 s).
    F24 RecentHitGrip() const;

    // sub_5992B0 (EvaluateDampingSuitability, inferred): 0 no damping, 1 damping, 2 at rest.
    int EvaluateDampingSuitability(int prev) const;

    // MovementDousingAndRoofReset(Car*, RigidBody*, Timer&, DampingSuitability&, PhysicsMover*) 0x5A5E20.
    // Returns 1 when the body was frozen/damped (the caller recomputes the auxiliaries).
    int MovementDousingAndRoofReset();

    // sub_5A5540 (PhysicsMover::UpdateCarState, inferred): copies the body and wheel state to CarState, the
    // rate-limited visual steering, contact counters, smoothed rpm and wheel-spin indicators. The render wheel
    // matrices (CarState::CalculateWheelMatrices 0x5921B0, through the CPU-dispatched D3DX matrix multiply) are
    // not computed.
    void UpdateCarState(float dt);

private:
    bb::World& World() const;
    const MoverData& K() const;
    // "fld TestRPM ; fcomp 100.0 ; test ah,44h ; jnp": true when TestRPM == 100 (use the car rpm).
    bool TestRpmIsOff() const;
    // 0 FWD (split < 0.2), 1 AWD, 2 RWD (split > 0.8): "test ah,5; jp" then "test ah,41h; jnz".
    static int DriveType(float split);
    // Non-driven wheel test of TuneWheelParams / ResolveDynoForces (split via [Wheel+0x20]+0 -> cpi+0x1C0).
    static bool NotDriven(const Wheel* w);
    static bool Gt(F24 a, F24 b);
    static bool Lt(F24 a, F24 b);

    // Owned components (the public raw pointers above point into these).
    std::unique_ptr<RigidBody> rbOwned_;
    std::unique_ptr<TwoWheelAckermanSteering> steeringOwned_;
    std::unique_ptr<Wheel> wheelsOwned_[4];
    std::unique_ptr<Suspension> suspOwned_[4];
    std::unique_ptr<bb::Engine> engineOwned_;
    std::unique_ptr<bb::DriveTrain> driveTrainOwned_;
    std::unique_ptr<Aerodynamics> aeroOwned_;
    float misc_[81] = {};                                // the MiscCarTunings row the drivetrain points to

    // Derivative buffer of PhysicsSystem::IntegrateEuler (a stack buffer in the game). RigidBody::GetDerivative
    // leaves entries 3, 23, 27, 29..31 unwritten: they stay 0 here (stack garbage in the game; see the doc).
    float dRb_[RigidBody::StateSize] = {};
    float dWheel_[4][2] = {};
    float dSusp_[4][2] = {};
};

} // namespace bb
