#pragma once

// Port of NFSU2Unity Core/Physics/VehicleSim.cs.

#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/PhysicsMover.h"
#include "drive/physics/input/InputTables.h"
#include "drive/physics/input/KeyboardAxis.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/timing/SimClock.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bb {

class CarTypeInfoU2;
class CollisionWorld;
class ITrackTopology;
class Speed2Image;

// Small facade over the ported vehicle simulation for a single player car: game clock (SimClock), player
// input chain (RealDriver joystick handlers + RealDriver::Drive), CarState::BeginTimestep and
// PhysicsMover::Move, on a ground provider (FlatGround by default). Game space: Z up,
// X forward of the car at heading 0, metres; rotation rows are (forward, side/left, up).
// When the ground also implements ITrackTopology and IWorldCollision (the city: TrackWorld) the car gets its
// TopologyCoordinate and collides with the track polygons as in World::DoTimestep 0x609CE0
// (CheckForLoadedTopology, BeginMovement, CarCollisionBody::Move, World::DoTimestepCollisions).
// Not modelled: other cars, AI, replays, Car::PutFlatOnTrack.
//
// The exe and the ground must outlive the VehicleSim. There is no in-place reset: build a new VehicleSim to
// respawn (construction reads a few kB of tables from the exe image; well under a millisecond).
class VehicleSim {
private:
    const Speed2Image& exe_;
    std::unique_ptr<CarPhysicsInfo> cpiOwned_;
    std::unique_ptr<FlatGround> flatGround_;

public:
    PhysicsEnvironment Env;
    SimClock Clock;
    bb::InputTables InputTables;
    InputContext Input;
    bb::Car Car;
    RealDriver Driver;
    std::unique_ptr<PhysicsMover> Mover;
    CarPhysicsInfo* const Cpi;
    // The track topology of the ground, when it has one (TrackWorld), else null.
    ITrackTopology* Topology = nullptr;
    // The car-vs-world collision world, when the ground provides track polygons, else null.
    std::unique_ptr<CollisionWorld> Collisions;

    // Builds a player car from a CarTypeInfo (stock CarPhysicsInfo = CarTypeInfo+0x110) placed at
    // position (CG, game space, 3 floats; null = (0, 0, 0.5)) with heading (radians about Z).
    // ground = null: a FlatGround at z = 0.
    VehicleSim(const Speed2Image& exe, const CarTypeInfoU2& carType, IWheelWorld* ground = nullptr,
               const float* position = nullptr, float heading = 0.0f);

    VehicleSim(const Speed2Image& exe, std::unique_ptr<CarPhysicsInfo> cpi, int8_t carTypeD8, IWheelWorld* ground = nullptr,
               const float* position = nullptr, float heading = 0.0f);

    ~VehicleSim();
    VehicleSim(const VehicleSim&) = delete;
    VehicleSim& operator=(const VehicleSim&) = delete;

    // Last frame's world step (s) and number of physics sub-steps.
    float LastDt() const { return lastDt_; }
    int LastSubsteps() const { return lastSubsteps_; }

    // Gives the car `seconds` of nitrous (CarState+0x414.. in 4000 Hz ticks).
    void SetNitrous(float seconds);

    // true: manual gearbox (RealDriver shift mode 1); false: automatic.
    bool ManualGearbox() const;
    void SetManualGearbox(bool value);

    // One game frame. realSeconds = measured duration of the previous frame (the clock
    // quantises it to 1/1800 s ticks and applies the 30 fps limiter). Inputs: throttle/brake/handbrake 0..1,
    // steer -1 (left) .. 1 (right), nitrous held, shift buttons (edges are detected here).
    void Frame(float realSeconds, float throttle, float brake, float steer, float handbrake,
               bool nitrous = false, bool shiftUp = false, bool shiftDown = false);

    // One game frame driven by keyboard keys, through the ported PC input layer (KeyboardAxis):
    // DirectInput key mask -> joystick state -> mapping functions of the 0x7FDD10 table -> JoyEvents sent only
    // when a value changes -> RealDriver joystick handlers (steering 0x8F = 0x00 left / 0x80 / 0xFF right,
    // throttle 0x91, brake 0x94, handbrake 0x99 = 0 / 0xFF, shifts 0x9B / 0x9C = 1 on press, 0 on release) and
    // Player::RacingJoyHandler nitrous (0xA3). As in MainLoop, GenerateJoyEvents runs after the timestep is
    // prepared and before World::DoTimestep, also on paused frames. Keys are the state held at this frame.
    void FrameKeyboard(float realSeconds, bool left, bool right, bool up, bool down, bool handbrake,
                       bool nitrous = false, bool shiftUp = false, bool shiftDown = false);

    // ------------------------------------------------------------------ getters (game space)

    // Centre of gravity (RigidBody+0x20).
    std::array<float, 3> Position() const;
    // Geometry origin of the car model (CarState+0x10 = pos - R*cg).
    std::array<float, 3> GeometryPosition() const;
    // 3x3 rotation, row-major: rows forward, left, up.
    std::array<float, 9> Rotation3x3() const;
    std::array<float, 3> Velocity() const;
    // Wheel spin angle (rad; Wheel+0x24), i = 0 FL, 1 FR, 2 RR, 3 RL.
    float WheelSpinAngle(int i) const;
    // Wheel angular velocity (rad/s).
    float WheelOmega(int i) const;
    // Steering angle of a front wheel (rad about Z, from the Ackermann matrices); 0 for the rear.
    float WheelSteerAngle(int i) const;
    // Suspension compression (m; Suspension+0x1C), positive = compressed.
    float SuspensionCompression(int i) const;
    // Wheel centre in car space (relative to the CG, includes compression).
    std::array<float, 3> WheelLocalPosition(int i) const;
    bool WheelOnGround(int i) const;
    // Smoothed engine rpm (CarState+0x400).
    float Rpm() const { return Car.State.Rpm; }
    // Gear (DriveTrain enum: 0 reverse, 1 neutral, 2 = 1st ...).
    int Gear() const;
    // |velocity| m/s (CarState+0x3F0).
    float Speed() const { return Car.State.Speed; }
    // Signed forward speed m/s (CarState+0x3EC).
    float ForwardSpeed() const { return Car.State.ForwardSpeed; }
    bool NitrousActive() const;

private:
    VehicleSim(const Speed2Image& exe, std::unique_ptr<CarPhysicsInfo> cpi, int8_t carTypeD8, IWheelWorld* ground,
               const float* position, float heading, const std::string* carName);

    // Car::CheckForLoadedTopology 0x5F6CD0: re-checks the section at the car; losing the topology puts all
    // wheels on the ground with terrain 1 (gaining it calls Car::PutFlatOnTrack, not ported).
    void CheckForLoadedTopology();

    // The car's part of World::DoTimestep 0x609CE0 after the drivers: CheckForLoadedTopology, Car::BeginMovement
    // 0x5FBBE0 (SetSectionPosition at the car, CarState::BeginTimestep), World::DoTimestepMove (CarCollisionBody::Move
    // = BeginMove + PhysicsMover::Move + UpdatePosition), CheckForLoadedTopology, World::DoTimestepCollisions,
    // CheckForLoadedTopology. Without a topology: BeginTimestep and Move, as before.
    void Step(float dt);

    // The handlers registered for the keyboard events (RealDriver::AddJoyHandlers 0x41E060 and
    // Player::RacingJoyHandler 0x602BD0 case 0xA3).
    void DispatchJoyEvent(int ev, uint8_t value);

    void UpdateView();

    CarInputView view_;
    bool lastShiftUp_ = false, lastShiftDown_ = false, lastNitrous_ = false;
    float lastDt_ = 0.0f;
    int lastSubsteps_ = 0;
    std::unique_ptr<KeyboardAxis> keyboard_;
    std::vector<JoyEventRecord> joyEvents_;
};

} // namespace bb
