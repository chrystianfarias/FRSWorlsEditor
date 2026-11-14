#include "drive/physics/PhysicsMover.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/Burnout.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/timing/SimClock.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/Wheel.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cstring>
#include <stdexcept>

namespace bb {

bb::World& PhysicsMover::World() const { return Env->World; }
const MoverData& PhysicsMover::K() const { return Env->Mover; }

PhysicsMover::PhysicsMover(PhysicsEnvironment* env) : Env(env) {}

PhysicsMover::~PhysicsMover() = default;

PhysicsMover::PhysicsMover(PhysicsEnvironment* env, CarPhysicsInfo* cpi, int8_t carTypeD8, CarDriver* driver, bb::Car* car)
    : Env(env) {
    RoofResetTimer = 0;
    Cpi = cpi;
    Car = car;
    Driver = driver;
    State = &car->State;
    // Car+0x34 is read during construction (Car::GetHydraulicState in the damping test); the game assigns it
    // after the ctor (SetMovementMode), so the ctor sees the previous mover. Use this one when there is none.
    if (car->Mover == nullptr) car->Mover = this;
    GripScale = 1.0f;
    IsOnDyno = false;
    rbOwned_ = std::make_unique<RigidBody>(env->Rb);
    Rb = rbOwned_.get();
    Rb->Construct(&cpi->RigidBody, K().Gravity);
    StateSize += RigidBody::StateSize;
    Rb->TopSpeed = cpi->TopSpeed;
    Rb->RaceEngineFlag = carTypeD8;
    Rb->CgOffset[0] = cpi->Cg[0]; Rb->CgOffset[1] = cpi->Cg[1]; Rb->CgOffset[2] = cpi->Cg[2];
    steeringOwned_ = std::make_unique<TwoWheelAckermanSteering>(cpi->Wheels);
    Steering = steeringOwned_.get();
    for (int i = 0; i < 4; i++) {
        wheelsOwned_[i] = std::make_unique<Wheel>(env, &cpi->Wheels[i], &cpi->Tires[i / 2], &cpi->Control, State);
        Wheels[i] = wheelsOwned_[i].get();
        suspOwned_[i] = std::make_unique<Suspension>(i, Rb, Wheels[i], i < 2 ? Steering : nullptr, &cpi->Suspensions[i / 2]);
        Susp[i] = suspOwned_[i].get();
        StateSize += 2 + 2;
        HydraulicState[i] = 0;
    }
    Susp[0]->Partner = Susp[1]; Susp[1]->Partner = Susp[0];
    Susp[2]->Partner = Susp[3]; Susp[3]->Partner = Susp[2];
    engineOwned_ = std::make_unique<bb::Engine>(env->Engine);
    Engine = engineOwned_.get();
    Engine->Construct(&cpi->Engine, State);
    StateSize += 1;
    Engine->HpMultFlag = carTypeD8;
    int type = DriveType(cpi->RearTorqueSplit());
    std::memcpy(misc_, env->Engine.MiscCarTunings.data() + 81 * type, sizeof misc_);
    driveTrainOwned_ = std::make_unique<bb::DriveTrain>(env->Engine);
    DriveTrain = driveTrainOwned_.get();
    DriveTrain->Construct(&cpi->DriveTrain, misc_, Engine, Wheels, State, World().ShiftPointSearch != 0, World().Seed, World().ReplayLocked);
    StateSize += 1;
    if (car->RideLevel == -1)
        throw std::runtime_error("RidePhysicsInfo::RebuildPhysicsInfo (lazy rebuild) is not ported");
    DriveTrain->PerfType = car->RideLevel;
    Engine->DriveTrain = DriveTrain;
    if (car->SlotType == 1 && World().Flags.Drag() && World().FEDatabase != 1)
        DriveTrain->PerfectRangeMul = K().PerfectShiftRangeMultipliers[World().Difficulty];
    if (car->SlotType == 2 && World().Flags.Drag() && (car->DriverType == 3 || car->DriverType == 2 || car->DriverType == 4))
        throw std::runtime_error("AI drag-race shift tuning (AICarController vtbl+0x78, pTheDifficultyInfo) is not ported");
    aeroOwned_ = std::make_unique<Aerodynamics>();
    Aero = aeroOwned_.get();
    Aero->Info = &cpi->Aero; Aero->Rb = Rb; Aero->SpeedScale = 1.0f; Aero->DragScale = 1.0f; Aero->Throttle = 0.0f;
    Aero->GroundFrac = 0.0f; Aero->Enable = 1.0f;
    World().UglyTimestepHack = 0.0f;
    EBrakeRearGrip = 1.0f;
    DampState = 0;
    CgShifted = false;
    Calibrate();
    InitializeStateFromCar();
    ComputeAuxiliaries();
    Field124 = 0;
}

int PhysicsMover::DriveType(float split) {
    int t = X87::ParityEven(X87::Fcom(split, WheelK::FwdSplit) & 0x05) ? 1 : 0;
    if ((X87::Fcom(split, WheelK::RwdSplit) & 0x41) == 0) t = 2;
    return t;
}

void PhysicsMover::InitializeStateFromCar() {
    Rb->SetState(State->Pos, State->R, State->Vel, State->AngVel);
    for (int i = 0; i < 4; i++) {
        Susp[i]->Compression = 0.0f;
        Susp[i]->CompressionRate = 0.0f;
        Wheels[i]->Angle = State->WheelAngle[i];
        Wheels[i]->Omega = State->WheelOmega[i];
    }
    float rpm = TestRpmIsOff() ? State->Rpm : World().TestRPM;
    Engine->Omega = (F24(rpm) * K().RpmToRad).Store();
    DriveTrain->LastRpmDiff = 0.0f;
    DriveTrain->Gear = State->Gear;
    DriveTrain->TargetGear = State->Gear;
    DriveTrain->ClutchState = 3;
    DriveTrain->Clutch = 1.0f;
}

bool PhysicsMover::TestRpmIsOff() const { return !X87::ParityEven(X87::Fcom(World().TestRPM, K().TestRpmOff) & 0x44); }

// ------------------------------------------------------------------ Move

float PhysicsMover::Move(float dt) {
    for (int i = 0; i < 4; i++) { State->Hopped[i] = 0; State->BottomOut[i] = 0.0f; }
    Calibrate();
    TuneBoostAndDampers();
    DelegateDriverInput();
    TuneWheelParams(dt);
    Burnout::CalcBurnout(Env->Engine, Env->Math, World(), *Car, *Cpi, Wheels, *DriveTrain);
    float nf, h;
    int n = PhysicsSubsteps::Split(dt, K().MaximumTimeStep, nf, h);
    float acc[6] = {};                                       // dword_86B390.. (global zero vector)
    for (int k = 0; k < n; k++) {
        World().UglyTimestepHack = h;
        IntegrateEuler(h);
        PhysicsSubsteps::Accumulate(acc, Rb->LastForce, Rb->LastTorque);
        BWareMath2::OrthonormalizeMatrix(Rb->R, Rb->R);
    }
    PhysicsSubsteps::Average(acc, nf);
    AccForce[0] = acc[0]; AccForce[1] = acc[1]; AccForce[2] = acc[2];
    AccTorque[0] = acc[3]; AccTorque[1] = acc[4]; AccTorque[2] = acc[5];
    UpdateCarState(dt);
    return dt;
}

void PhysicsMover::IntegrateEuler(float h) {
    Rb->GetDerivative(dRb_);
    for (int i = 0; i < 4; i++) {
        Wheels[i]->GetDerivative(dWheel_[i]);
        Susp[i]->GetDerivative(dSusp_[i]);
    }
    float dEngine = Engine->GetDerivative();
    float dClutch = DriveTrain->GetDerivative();
    Rb->IntegrateEuler(h, dRb_);
    for (int i = 0; i < 4; i++) {
        Wheel* w = Wheels[i];
        w->Angle = (F24(h) * dWheel_[i][0] + w->Angle).Store();
        w->Omega = (F24(h) * dWheel_[i][1] + w->Omega).Store();
        Suspension* s = Susp[i];
        s->Compression = (F24(h) * dSusp_[i][0] + s->Compression).Store();
        s->CompressionRate = (F24(h) * dSusp_[i][1] + s->CompressionRate).Store();
    }
    Engine->Omega = (F24(h) * dEngine + Engine->Omega).Store();
    DriveTrain->Clutch = (F24(h) * dClutch + DriveTrain->Clutch).Store();
    ComputeAuxiliaries();
}

void PhysicsMover::SystemComputeAuxiliaries() {
    Rb->ComputeAuxiliaries();
    for (int i = 0; i < 4; i++) {
        Wheels[i]->ComputeAuxiliaries();
        Susp[i]->ComputeAuxiliaries();
    }
    Engine->ComputeAuxiliaries(World().UglyTimestepHack);
    DriveTrain->ComputeAuxiliaries(World().WorldTimer, World().Seed, World().ReplayLocked);
}

void PhysicsMover::ComputeAuxiliaries() {
    SystemComputeAuxiliaries();
    BottomOut();
    ResolveForces();
}

void PhysicsMover::BottomOut() {
    int r = MovementDousingAndRoofReset();
    bool recompute = r != 0;
    if (r == 0) {
        for (int i = 0; i < 4; i++) {
            Suspension* s = Susp[i];
            if (X87::ParityEven(X87::Fcom(s->Compression, s->Info->MaxCompression) & 0x44)) continue;   // != (or NaN)
            F24 imp = s->ApplyGroundPenaltyImpulse();
            State->BottomOut[i] = (imp + State->BottomOut[i]).Store();
            recompute = true;
        }
    }
    if (World().TriggerBounce != 0 && Car->DriverType == 1) {
        World().TriggerBounce = 0;
        const float j[3] = {0.0f, 0.0f, K().TriggerBounceImpulse};
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        Rb->ApplyImpulse(j, zero);
        recompute = true;
    }
    if (recompute) {
        World().UglyTimestepHack = 0.0f;
        SystemComputeAuxiliaries();
    }
}

} // namespace bb
