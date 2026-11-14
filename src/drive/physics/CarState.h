#pragma once

// Port of NFSU2Unity Core/Physics/CarState.cs (CarState and Car).

#include <cstdint>
#include <memory>

namespace bb {

class BWareMath;
class CarCollisionBody;
class CarDriver;
class CarPhysicsInfo;
class PhysicsMover;

// CarState (embedded at Car+0x40): mirror of the physics state for render/HUD/AI, refreshed once per
// PhysicsMover::Move by UpdateCarState 0x5A5540, plus a few fields owned by the physics (nitrous, burnout,
// hydraulic heights, wheel contact counter). Only the fields read or written by the vehicle dynamics are
// modelled; the original offsets are in the comments (docs/re/physics-overview.md section 3.7, appendix C section 5, section 8).
class CarState {
public:
    CarPhysicsInfo* Cpi = nullptr;                  // +0x000 CarPhysicsInfo*
    void* Topology = nullptr;                       // +0x004 TopologyCoordinate* (this of GetElevation; the C# object)
    float GeomPos[3] = {};                          // +0x010 pos - R*cg (CalculateGeometryPosition)
    float Pos[3] = {};                              // +0x020 = RigidBody.Pos (Car+0x60/+0x64 = x, y)
    float Vel[3] = {};                              // +0x030 = RigidBody.Vel
    float R[16] = {};                               // +0x050 = RigidBody.R (Car+0x90.., up row at Car+0xB0)
    float R2[16] = {};                              // +0x090 copy of +0x50
    float AngVel[3] = {};                           // +0x0D0 = RigidBody.AngVel
    // +0x0F0 + 0x40*i / +0x1F0 + 0x40*i: render wheel matrices (CalculateWheelMatrices, not modelled)
    float WheelOmega[4] = {};                       // +0x2F0 Wheel+0x28 (read back by InitializeStateFromCar)
    float WheelAngle[4] = {};                       // +0x300 Wheel+0x24
    float WheelLoad[4] = {};                        // +0x310 Wheel+0x164
    float WheelRoadNoise[4] = {};                   // +0x320 Wheel+0x168 when on the ground
    float BottomOut[4] = {};                        // +0x330 sum of ApplyGroundPenaltyImpulse (0 at Move start)
    float SuspDisplay[4] = {};                      // +0x340 render z offset of the wheel
    uint8_t WheelOnGround[4] = {};                  // +0x350 + i
    int WheelTerrain[4] = {};                       // +0x354 TerrainType
    int MaxWheelTerrain[4] = {};                    // +0x364 largest TerrainType since BeginTimestep
    float WheelSlip[8] = {};                        // +0x374 + 8i: Wheel+0x100, +0x104
    float WheelGripUsage[4] = {};                   // +0x394 Wheel+0x170
    uint8_t WheelAttached[4] = {};                  // +0x3B4 (CalculateWheelMatrices)
    float HydraulicHeight[4] = {};                  // +0x3B8 (Calibrate ride height; ResolveHydraulicForces)
    uint8_t Hopped[4] = {};                         // +0x3C8 hydraulic hop this Move
    uint16_t Heading = 0;                           // +0x3CC bFastATan(R[0][0], R[0][1])
    uint16_t SteerVisual = 0;                       // +0x3CE rate-limited steering for render
    float AvgGripUsage = 0;                         // +0x3D8
    float Spin03 = 0;                               // +0x3DC
    float Spin12 = 0;                               // +0x3E0
    int WheelsOnGround = 0;                         // +0x3E4 (Car+0x424)
    int WheelsAttached = 0;                         // +0x3E8 (Car+0x428) CarState::SetWheelAttached counter (4)
    float ForwardSpeed = 0;                         // +0x3EC signed longitudinal speed m/s (Car+0x42C)
    float Speed = 0;                                // +0x3F0 |v| (Car+0x430)
    int Gear = 0;                                   // +0x3F4 DriveTrain+0x3C (Car+0x434)
    float Throttle = 0;                             // +0x3F8 effective throttle
    float Rpm = 0;                                  // +0x400 smoothed rpm (Car+0x440)
    int ClutchEngaged = 0;                          // +0x408 DriveTrain+0x44 == 3
    float ClutchTorque = 0;                         // +0x40C DriveTrain+0x70
    int NosCapacity = 0;                            // +0x414 4000 Hz ticks
    int NosShot = 0;                                // +0x418
    int NosRemaining = 0;                           // +0x41C
    int NosStartTime = 0;                           // +0x420 (0 / 0x7FFFFFFF = off)
    int PilotFlag = 0;                              // +0x424 == 1 enables Wheel::GetPilotFactor and assists
    int BurnoutCount = 0;                           // +0x42C (also the differential mode of ApplyClutchTorque)
    float BurnoutTimer = 0;                         // +0x430
    float BurnoutIntensity = 0;                     // +0x434
    float BurnoutStill = 0;                         // +0x438
    uint8_t Field480 = 0;                           // +0x480 (0 after UpdateCarState)

    // CarState::BeginTimestep 0x591D70 (Car::BeginMovement, once per frame).
    void BeginTimestep();

    // CarState::SetWheelTouchingGround(int i, char v) 0x592040: keeps the on-ground counter.
    void SetWheelTouchingGround(int i, uint8_t v);

    // CarState::SetWheelTerrain(int i, TerrainType t) 0x592080 (signed compare).
    void SetWheelTerrain(int i, int t);

    // CarState::IsDriveWheel(int) 0x592870: split < 0.2 -> FWD (wheels 0,1); split <= 0.8 -> AWD (all);
    // otherwise RWD (wheels 2,3). split = [+0x0]+0x1C0.
    bool IsDriveWheel(int i) const;

    // CarState::GetSlipAngle (sub_591D90): radians in (-pi, pi] between the heading and the velocity.
    float GetSlipAngle(const BWareMath& math) const;

    // CarState::CalculateGeometryPosition(bVector3* cg) 0x592400: GeomPos = Pos - eMulVector(R, cg)
    // (cg = Cpi.Cg when null; eMulVector adds R's row 3).
    void CalculateGeometryPosition(const float* cg);
};

// The Car (0x9E0 bytes) fields the vehicle simulation reads or writes. CarState is embedded at +0x40, so the
// Car offsets +0x424.. +0x440 used by some functions are CarState fields (see State).
// Owns its CarCollisionBody; the mover, driver and CarPhysicsInfo are owned by the caller.
class Car {
public:
    CarState State;                                 // +0x040
    int8_t RideLevel = 0;                           // [+0x10] RideInfo+0x14 = (s8)RidePhysicsInfo+4 (-1: lazy rebuild)
    float RideRpmDecelIndex = 0;                    // [+0x10] RideInfo+0x2F4 (SmoothRPMDecel row)
    int SlotType = 0;                               // [+0x14]+0x4: 1 player, 2 racer, 3 traffic
    uint8_t SlotFlag900 = 0;                        // [+0x14]+0x900
    bool HasParticipant = false;                    // +0x01C != 0 (MaybeForceYaw / roof reset guard)
    uint16_t ParticipantField10 = 0;                // [+0x1C]+0x10 (sub_5EE250)
    int ParticipantMode1E0 = 0;                     // [[+0x1C]+0x1E0]+0x4 (== 2: dyno throttle gate)
    CarPhysicsInfo* Cpi = nullptr;                  // +0x020 (CG for FixCarCenterOfGravity)
    int DriverType = 0;                             // +0x024 1..3
    int MovementMode = 1;                           // +0x028 1 = PhysicsMover
    CarDriver* Driver = nullptr;                    // +0x030
    PhysicsMover* Mover = nullptr;                  // +0x034
    int Mode = 0;                                   // +0x038 1: gravity 11, no aero (inferred: test drive)
    CarCollisionBody* Collision = nullptr;          // +0x03C (owned, see below)
    float CatchupScale = 1.0f;                      // +0x468
    float GripScale = 1.0f;                         // +0x4D8 (copied to PhysicsMover+0x6C at creation)
    float Slipstream = 0;                           // +0x4DC
    float CatchupOverride = 0;                      // +0x4E4
    uint8_t FlatTyreMask = 0;                       // +0x95C bit i: tyre i flat
    int ResetRequest = 0;                           // +0x96C (sub_5EBF60 sets 1)
    int BlinkStart = 0;                             // +0x974
    float BlinkDuration = 0;                        // +0x978
    uint8_t Poofed = 0;                             // +0x998 Car::IsPoofed 0x5EC060
    bool TopologyLoaded = false;                    // +0x5C0 Car::CheckForLoadedTopology 0x5F6CD0

    Car();
    ~Car();
    Car(const Car&) = delete;
    Car& operator=(const Car&) = delete;

    // Car::IsBlinking 0x5EBFA0.
    bool IsBlinking(int worldTimer) const;

    // Car::AreHydraulicsBouncing 0x5ED290: any hop button of the driver > 0.
    bool AreHydraulicsBouncing() const;

    // Car::GetHydraulicState(int) 0x5ED470.
    int GetHydraulicState(int i) const;

    // sub_5ED1B0: 1 when no drive wheel touches the ground.
    bool NoDriveWheelOnGround() const;

    // sub_5ED220: wheels on the ground, or one of the car body's collisions in progress (sub_5A0820, the i-th
    // witness with a Collision involving it) is with a flat track polygon (sub_5A01B0: |nz| > 0.7).
    bool IsSupported() const;

private:
    std::unique_ptr<CarCollisionBody> collisionOwned_;
};

} // namespace bb
