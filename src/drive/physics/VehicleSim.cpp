#include "drive/physics/VehicleSim.h"

#include "drive/cars/CarTypeInfoU2.h"
#include "drive/io/BinHash.h"
#include "drive/physics/TrackTopology.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/collision/GjkShapes.h"
#include "drive/physics/collision/WorldCollision.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/engine/Nitrous.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/Wheel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bb {

namespace {
int8_t CarTypeD8(const CarTypeInfoU2& carType) {
    if (carType.Raw.size() <= 0xD8) throw std::runtime_error("CarTypeInfo record too short");
    return static_cast<int8_t>(carType.Raw[0xD8]);
}

// MathF.Round: to nearest, ties to even (the default FPU rounding mode).
int RoundEven(float v) { return static_cast<int>(std::nearbyint(v)); }
} // namespace

VehicleSim::VehicleSim(const Speed2Image& exe, const CarTypeInfoU2& carType, IWheelWorld* ground,
                       const float* position, float heading)
    : VehicleSim(exe, CarPhysicsInfo::FromCarType(carType), CarTypeD8(carType), ground, position, heading, &carType.CollectionName) {}

VehicleSim::VehicleSim(const Speed2Image& exe, std::unique_ptr<CarPhysicsInfo> cpi, int8_t carTypeD8, IWheelWorld* ground,
                       const float* position, float heading)
    : VehicleSim(exe, std::move(cpi), carTypeD8, ground, position, heading, nullptr) {}

VehicleSim::~VehicleSim() = default;

VehicleSim::VehicleSim(const Speed2Image& exe, std::unique_ptr<CarPhysicsInfo> cpi, int8_t carTypeD8, IWheelWorld* ground,
                       const float* position, float heading, const std::string* carName)
    : exe_(exe), cpiOwned_(std::move(cpi)), Env(exe), InputTables(exe), Cpi(cpiOwned_.get()) {
    if (ground == nullptr) {
        flatGround_ = std::make_unique<FlatGround>();
        ground = flatGround_.get();
    }
    Env.Terrain = ground;
    Input.Flags = Env.World.Flags;
    Car.Cpi = Cpi; Car.SlotType = 1; Car.DriverType = 1; Car.HasParticipant = false;
    CarState& s = Car.State;
    s.Cpi = Cpi;
    const float defaultPos[3] = {0.0f, 0.0f, 0.5f};
    const float* p = position != nullptr ? position : defaultPos;
    s.Pos[0] = p[0]; s.Pos[1] = p[1]; s.Pos[2] = p[2];
    float c = std::cos(heading), sn = std::sin(heading);
    const float r[16] = {c, sn, 0, 0, -sn, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(s.R, r, sizeof r);
    for (int i = 0; i < 4; i++) s.WheelAttached[i] = 1;
    s.WheelsAttached = 4;
    s.Gear = 1;
    s.Rpm = Cpi->Engine.IdleRpm;
    s.PilotFlag = 1;
    Driver.Port = 0; Driver.ForceFullBrake = false; Driver.GearCommand = -1; Driver.CurrentGear = 1;
    Car.Driver = &Driver;
    Clock.BeginWorld();
    Env.World.WorldTimer = Clock.Globals.WorldTimer;
    Mover = std::make_unique<PhysicsMover>(&Env, Cpi, carTypeD8, &Driver, &Car);
    Car.Mover = Mover.get();
    Mover->GripScale = Car.GripScale;                          // Car::SetMovementMode: mover+0x6C = Car+0x4D8

    // --- world (only with a ground that has a topology; FlatGround / TestGround keep the old path) ---
    Topology = dynamic_cast<ITrackTopology*>(Env.Terrain);
    if (Topology != nullptr) {
        Car.State.Topology = Topology->CreateTopologyCoordinate();   // CarState+0x4 -> Car+0x5C4
        Topology->SetSectionPosition(Car.State.Topology, s.Pos[0], s.Pos[1], 0.0f);
        Car.TopologyLoaded = Topology->IsConnected(Car.State.Topology);
        if (auto* wc = dynamic_cast<IWorldCollision*>(Env.Terrain)) {
            CollisionK::Load(exe);
            Collisions = std::make_unique<CollisionWorld>(&Env.World, wc, Topology);
            // RideInfo::GetCollisionVolumeNameHash 0x61C320 (stock car: "<CAR>_CV"), FindGJKPolytope 0x5A18C0
            GjkPolytope* hull = carName != nullptr ? wc->FindCollisionVolume(*carName, BinHash::Hash(*carName + "_CV")) : nullptr;
            Car.Collision->Attach(Collisions.get(), Car.State.Topology, hull, Env.World.WorldTimer);
        }
    }
}

void VehicleSim::CheckForLoadedTopology() {
    CarState& s = Car.State;
    Topology->SetSectionPosition(s.Topology, s.Pos[0], s.Pos[1], 0.0f);
    bool loaded = Topology->IsConnected(s.Topology);
    if (loaded == Car.TopologyLoaded) return;
    Car.TopologyLoaded = loaded;
    if (!loaded)
        for (int i = 0; i < 4; i++) {
            s.SetWheelTouchingGround(i, 1);
            s.SetWheelTerrain(i, 1);
        }
}

void VehicleSim::Step(float dt) {
    if (Topology == nullptr) {
        Car.State.BeginTimestep();
        Mover->Move(dt);
        return;
    }
    CheckForLoadedTopology();
    Topology->SetSectionPosition(Car.State.Topology, Car.State.Pos[0], Car.State.Pos[1], 0.0f);
    Car.State.BeginTimestep();
    if (Collisions != nullptr) Car.Collision->Move(dt);
    else Mover->Move(dt);
    CheckForLoadedTopology();
    if (Collisions != nullptr) Collisions->DoTimestepCollisions(dt);
    CheckForLoadedTopology();
}

void VehicleSim::SetNitrous(float seconds) {
    int t = static_cast<int>(seconds * 4000.0f);
    Car.State.NosCapacity = t; Car.State.NosRemaining = t; Car.State.NosShot = 0; Car.State.NosStartTime = 0;
}

bool VehicleSim::ManualGearbox() const { return Driver.ShiftMode == 1; }

void VehicleSim::SetManualGearbox(bool value) {
    Driver.ShiftMode = value ? 1 : 0;
    Driver.GearCommand = value ? std::max(2, Mover->DriveTrain->Gear) : -1;
}

void VehicleSim::Frame(float realSeconds, float throttle, float brake, float steer, float handbrake,
                       bool nitrous, bool shiftUp, bool shiftDown) {
    World& w = Env.World;
    SimFrame f = Clock.Frame(realSeconds);
    lastDt_ = f.WorldTimeElapsed;
    lastSubsteps_ = f.Substeps;
    if (f.Paused || !(f.WorldTimeElapsed > 0.0f)) return;
    w.WorldTimer = f.WorldTimerAtStep;
    w.RealTimer = Clock.Globals.RealTimer;
    w.WorldTimeElapsed = f.WorldTimeElapsed;
    Input.WorldTimer = w.WorldTimer;
    Input.WorldLoopCounter = Clock.Globals.WorldLoopCounter;
    Input.TestRPM = w.TestRPM;
    Input.Flags = w.Flags;
    UpdateView();

    // GenerateJoyEvents -> RealDriver joystick handlers (raw bytes as DirectInput would give them)
    auto Pedal = [](float v) { return static_cast<uint8_t>(std::max(0, std::min(255, RoundEven(v * 255.0f)))); };
    float st = std::max(-1.0f, std::min(1.0f, steer));
    Driver.JoyHandlerSteer(0x8F, static_cast<uint8_t>(std::max(0, std::min(255, RoundEven(128.0f + 127.0f * st)))));
    Driver.JoyHandlerThrottle(0x92, Pedal(throttle), view_, InputTables);
    Driver.JoyHandlerBrake(0x95, Pedal(brake));
    Driver.JoyHandlerEBrake(0x99, Pedal(handbrake));
    if (shiftUp && !lastShiftUp_) Driver.JoyHandlerShiftUp(1, view_);
    if (shiftDown && !lastShiftDown_) Driver.JoyHandlerShiftDown(1, view_);
    lastShiftUp_ = shiftUp; lastShiftDown_ = shiftDown;
    if (nitrous && !lastNitrous_) Nitrous::StartNitrous(Car.State, w.NosNow(), Env.Engine);
    else if (!nitrous && lastNitrous_) Nitrous::StopNitrous(Car.State, w.NosNow(), Env.Engine.NitrousContinuous);
    lastNitrous_ = nitrous;

    // World::DoTimestep: RealDriver::Drive, Car::BeginMovement (CarState::BeginTimestep), PhysicsMover::Move
    w.WorldLoopCounter = Clock.Globals.WorldLoopCounter;
    if (Topology != nullptr) CheckForLoadedTopology();
    Driver.Drive(w.WorldTimeElapsed, view_, Input, InputTables);
    Step(w.WorldTimeElapsed);
}

void VehicleSim::FrameKeyboard(float realSeconds, bool left, bool right, bool up, bool down, bool handbrake,
                               bool nitrous, bool shiftUp, bool shiftDown) {
    World& w = Env.World;
    SimFrame f = Clock.Frame(realSeconds);
    lastDt_ = f.WorldTimeElapsed;
    lastSubsteps_ = f.Substeps;
    bool step = !f.Paused && f.WorldTimeElapsed > 0.0f;
    if (step) {
        w.WorldTimer = f.WorldTimerAtStep;
        w.RealTimer = Clock.Globals.RealTimer;
        w.WorldTimeElapsed = f.WorldTimeElapsed;
        Input.WorldTimer = w.WorldTimer;
        Input.WorldLoopCounter = Clock.Globals.WorldLoopCounter;
        Input.TestRPM = w.TestRPM;
        Input.Flags = w.Flags;
    }
    UpdateView();

    // GenerateJoyEvents 0x5809C0
    if (!keyboard_) keyboard_ = std::make_unique<KeyboardAxis>(exe_);
    keyboard_->SetKeys(left, right, up, down, handbrake, nitrous, shiftUp, shiftDown);
    joyEvents_.clear();
    keyboard_->Poll(joyEvents_);
    for (const JoyEventRecord& e : joyEvents_)
        if (keyboard_->Port.Dispatch(e)) DispatchJoyEvent(e.Event(), e.Value());

    if (!step) return;
    w.WorldLoopCounter = Clock.Globals.WorldLoopCounter;
    if (Topology != nullptr) CheckForLoadedTopology();
    Driver.Drive(w.WorldTimeElapsed, view_, Input, InputTables);
    Step(w.WorldTimeElapsed);
}

void VehicleSim::DispatchJoyEvent(int ev, uint8_t value) {
    switch (ev) {
        case 0x8F: case 0x90: Driver.JoyHandlerSteer(ev, value); break;
        case 0x91: case 0x92: case 0x93: Driver.JoyHandlerThrottle(ev, value, view_, InputTables); break;
        case 0x94: case 0x95: case 0x96: Driver.JoyHandlerBrake(ev, value); break;
        case 0x97: case 0x98: Driver.JoyHandlerReverse(ev, value, view_); break;
        case 0x99: case 0x9A: Driver.JoyHandlerEBrake(ev, value); break;
        case 0x9B: case 0x9D: Driver.JoyHandlerShiftUp(value, view_); break;
        case 0x9C: case 0x9E: Driver.JoyHandlerShiftDown(value, view_); break;
        case 0x9F: case 0xA0: Driver.JoyHandlerDisengageClutch(ev, value); break;
        case 0xA3:
            // value != 0: StartNitrous (Player::IsNitrousAvailable not modelled);
            // 0: StopNitrous only if it is on and NitrousContinuous != 0 (0x603562)
            if (value != 0) Nitrous::StartNitrous(Car.State, Env.World.NosNow(), Env.Engine);
            else if (Nitrous::IsNitrousOn(Car.State, Env.World.NosNow(), Env.Engine) != 0 && Env.Engine.NitrousContinuous != 0)
                Nitrous::StopNitrous(Car.State, Env.World.NosNow(), Env.Engine.NitrousContinuous);
            break;
        default:
            break;
    }
}

void VehicleSim::UpdateView() {
    const CarState& s = Car.State;
    view_.SpeedLong = s.ForwardSpeed;
    view_.Car118 = s.AngVel[2];                                 // Car+0x118 = CarState+0xD8 (yaw rate)
    view_.Car128 = 0.0f;                                        // Car+0x128 (CarState+0xE8): writer not identified
    view_.Car430 = s.Speed;
    view_.Gear = s.Gear;
    view_.Rpm = s.Rpm;
    view_.VelX = s.Vel[0]; view_.VelY = s.Vel[1]; view_.VelZ = s.Vel[2];
    view_.MovementMode = Car.MovementMode;
    view_.LastCollisionTime = Car.Collision->LastHitTime;
    view_.Yaw = s.AngVel[2];                                    // CarState::GetYaw 0x59FA10 (inferred: yaw rate)
    view_.TopSpeed = Cpi->TopSpeed;                             // sub_5B5930 (inferred)
    view_.SteerMaxDeg = Cpi->Control.SteerMaxDeg;
    view_.IdleRpm = Cpi->Engine.IdleRpm;
    view_.NumForwardGears = Cpi->DriveTrain.NumGears;
}

// ------------------------------------------------------------------ getters (game space)

std::array<float, 3> VehicleSim::Position() const { return {Mover->Rb->Pos[0], Mover->Rb->Pos[1], Mover->Rb->Pos[2]}; }

std::array<float, 3> VehicleSim::GeometryPosition() const { return {Car.State.GeomPos[0], Car.State.GeomPos[1], Car.State.GeomPos[2]}; }

std::array<float, 9> VehicleSim::Rotation3x3() const {
    const float* r = Mover->Rb->R;
    return {r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10]};
}

std::array<float, 3> VehicleSim::Velocity() const { return {Mover->Rb->Vel[0], Mover->Rb->Vel[1], Mover->Rb->Vel[2]}; }

float VehicleSim::WheelSpinAngle(int i) const { return Mover->Wheels[i]->Angle; }

float VehicleSim::WheelOmega(int i) const { return Mover->Wheels[i]->Omega; }

float VehicleSim::WheelSteerAngle(int i) const {
    return i < 2 ? std::atan2(Mover->Steering->Mat[i][1], Mover->Steering->Mat[i][0]) : 0.0f;
}

float VehicleSim::SuspensionCompression(int i) const { return Mover->Susp[i]->Compression; }

std::array<float, 3> VehicleSim::WheelLocalPosition(int i) const {
    const float* p = Mover->Susp[i]->WheelPosLocal;
    return {p[0], p[1], p[2]};
}

bool VehicleSim::WheelOnGround(int i) const { return Mover->Wheels[i]->OnGround != 0; }

int VehicleSim::Gear() const { return Mover->DriveTrain->Gear; }

bool VehicleSim::NitrousActive() const { return Mover->Engine->NosLevel > 0; }

} // namespace bb
