// Golden suite for the integrated PhysicsMover (golden/mover_*.json, gen_golden_mover.py); port of
// NFSU2Unity tools/harness/csharp/SuiteMover.cs.
// A case carries the memory of the whole object graph before the PhysicsMover ctor ("init"), optionally a snapshot
// to continue from ("pre"), and a list of operations (ctor / move / substep), each with the memory of every object
// recorded after it. The suite builds the C++ graph with the ported ctor, (loads the snapshot), runs the operations
// and, after each one, writes every modelled field of every object (plus the structural fields: vtables, names,
// component list links, object pointers) onto its own previous image of the memory, then compares all bytes.

#include "golden.h"

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/PhysicsMover.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/engine/Nitrous.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/Wheel.h"

#include <map>
#include <memory>
#include <sstream>

using namespace golden;
using namespace bb;

namespace {

constexpr uint32_t G_WorldTimer = 0x86518C, G_RealTimer = 0x8651AC, G_WorldTimeElapsed = 0x865154, G_Ugly = 0x7FC800;
constexpr uint32_t VT_Mover = 0x7A25E0, VT_Vehicle = 0x7A25D4, VT_Rb = 0x7A20C4, VT_Steer = 0x7A2084, VT_Wheel = 0x7A20E8;
constexpr uint32_t VT_Susp = 0x7A210C, VT_Engine = 0x7A235C, VT_DriveTrain = 0x7A2508, NosCurveVA = 0x7FBF38, MiscVA = 0x7FC2A8;

using Mem = std::map<std::string, Bytes>;

// ------------------------------------------------------------------ symmetric memory mapper
struct Img {
    uint8_t* B; bool Read;
    Img(Bytes& b, bool read) : B(b.data()), Read(read) {}
    void F(int o, float& v) { if (Read) std::memcpy(&v, B + o, 4); else std::memcpy(B + o, &v, 4); }
    void I(int o, int& v) { if (Read) std::memcpy(&v, B + o, 4); else std::memcpy(B + o, &v, 4); }
    void U8(int o, uint8_t& v) { if (Read) v = B[o]; else B[o] = v; }
    void S8(int o, int8_t& v) { if (Read) v = static_cast<int8_t>(B[o]); else B[o] = static_cast<uint8_t>(v); }
    void Bool(int o, bool& v) { if (Read) v = B[o] != 0; else B[o] = static_cast<uint8_t>(v ? 1 : 0); }
    void U16(int o, uint16_t& v) { if (Read) std::memcpy(&v, B + o, 2); else std::memcpy(B + o, &v, 2); }
    template <size_t N> void Fa(int o, float (&a)[N]) { for (size_t i = 0; i < N; i++) F(o + 4 * static_cast<int>(i), a[i]); }
    template <size_t N> void Ia(int o, int (&a)[N]) { for (size_t i = 0; i < N; i++) I(o + 4 * static_cast<int>(i), a[i]); }
    template <size_t N> void Ba(int o, uint8_t (&a)[N]) { for (size_t i = 0; i < N; i++) U8(o + static_cast<int>(i), a[i]); }
    void P(int o, uint32_t va) { if (!Read) std::memcpy(B + o, &va, 4); }
    void Str(int o, const char* s) { if (!Read) { size_t n = std::strlen(s); std::memcpy(B + o, s, n); B[o + n] = 0; } }
};

// A CarDriver whose vtbl+0x84 returns a fixed yaw (the golden patches a vtable copy).
class YawDriver : public CarDriver {
public:
    uint16_t Yaw = 0;
    uint16_t GetForcedYaw() override { return Yaw; }
};

struct Ctx {
    std::map<std::string, uint32_t> Va;
    Mem M;
    World* W = nullptr;
    std::unique_ptr<CarPhysicsInfo> Cpi;
    std::unique_ptr<Car> C;
    std::unique_ptr<RealDriver> Real;
    std::unique_ptr<YawDriver> Yaw;
    CarDriver* Driver = nullptr;
    std::unique_ptr<PhysicsMover> Mover;
    int MiscType = 0;
    uint32_t CpiVa() const { return Va.at("cpi"); }
    uint32_t Cs() const { return Va.at("car") + 0x40; }
};

void MapCar(Img m, Ctx& x) {
    Car& c = *x.C;
    m.I(0x24, c.DriverType); m.I(0x28, c.MovementMode); m.I(0x38, c.Mode);
    m.F(0x468, c.CatchupScale); m.F(0x4D8, c.GripScale); m.F(0x4DC, c.Slipstream); m.F(0x4E4, c.CatchupOverride);
    m.U8(0x95C, c.FlatTyreMask); m.I(0x96C, c.ResetRequest); m.I(0x974, c.BlinkStart); m.F(0x978, c.BlinkDuration);
    CarState& s = c.State;
    const int b = 0x40;
    m.Fa(b + 0x10, s.GeomPos); m.Fa(b + 0x20, s.Pos); m.Fa(b + 0x30, s.Vel); m.Fa(b + 0x50, s.R); m.Fa(b + 0x90, s.R2);
    m.Fa(b + 0xD0, s.AngVel); m.Fa(b + 0x2F0, s.WheelOmega); m.Fa(b + 0x300, s.WheelAngle); m.Fa(b + 0x310, s.WheelLoad);
    m.Fa(b + 0x320, s.WheelRoadNoise); m.Fa(b + 0x330, s.BottomOut); m.Fa(b + 0x340, s.SuspDisplay); m.Ba(b + 0x350, s.WheelOnGround);
    m.Ia(b + 0x354, s.WheelTerrain); m.Ia(b + 0x364, s.MaxWheelTerrain); m.Fa(b + 0x374, s.WheelSlip); m.Fa(b + 0x394, s.WheelGripUsage);
    m.Ba(b + 0x3B4, s.WheelAttached); m.Fa(b + 0x3B8, s.HydraulicHeight); m.Ba(b + 0x3C8, s.Hopped);
    m.U16(b + 0x3CC, s.Heading); m.U16(b + 0x3CE, s.SteerVisual); m.F(b + 0x3D8, s.AvgGripUsage);
    m.F(b + 0x3DC, s.Spin03); m.F(b + 0x3E0, s.Spin12); m.I(b + 0x3E4, s.WheelsOnGround); m.I(b + 0x3E8, s.WheelsAttached);
    m.F(b + 0x3EC, s.ForwardSpeed); m.F(b + 0x3F0, s.Speed); m.I(b + 0x3F4, s.Gear); m.F(b + 0x3F8, s.Throttle);
    m.F(b + 0x400, s.Rpm); m.I(b + 0x408, s.ClutchEngaged); m.F(b + 0x40C, s.ClutchTorque);
    m.I(b + 0x414, s.NosCapacity); m.I(b + 0x418, s.NosShot); m.I(b + 0x41C, s.NosRemaining); m.I(b + 0x420, s.NosStartTime);
    m.I(b + 0x424, s.PilotFlag); m.I(b + 0x42C, s.BurnoutCount); m.F(b + 0x430, s.BurnoutTimer);
    m.F(b + 0x434, s.BurnoutIntensity); m.F(b + 0x438, s.BurnoutStill); m.U8(b + 0x480, s.Field480);
}

void MapCcb(Img m, Ctx& x) {
    CarCollisionBody& c = *x.C->Collision;
    m.S8(0x18, c.Contacts); m.U8(0x19, c.Damping); m.F(0xD0, c.ExtentNegZ); m.F(0xD4, c.ExtentZ);
    m.I(0x120, c.WallContacts); m.I(0x140, c.LastHitTime);
}

void MapDriver(Img m, Ctx& x) {
    CarDriver& d = *x.Driver;
    m.U16(0x1E0, d.SteerAngle); m.I(0x1E4, d.GearCommand); m.I(0x1E8, d.CurrentGear);
    m.Fa(0x1EC, d.Hop); m.Fa(0x1FC, d.HydraulicHeight); m.F(0x20C, d.Throttle); m.F(0x210, d.EBrake); m.F(0x214, d.Brake);
}

uint32_t NodeVa(const Ctx& x, int k) {
    switch (k) {
    case 0: return x.Va.at("rb") + 4;
    case 9: return x.Va.at("engine") + 4;
    case 10: return x.Va.at("dt") + 4;
    default: return x.Va.at(std::string((k - 1) % 2 == 0 ? "w" : "s") + std::to_string((k - 1) / 2)) + 4;
    }
}

void Links(Img& m, const Ctx& x, int k) {
    uint32_t head = x.Va.at("mover") + 0x14;
    m.P(4, k == 10 ? head : NodeVa(x, k + 1));
    m.P(8, k == 0 ? head : NodeVa(x, k - 1));
}

std::string N(const char* p, int i) { return std::string(p) + std::to_string(i); }

void MapMover(Img m, Ctx& x) {
    PhysicsMover& p = *x.Mover;
    m.P(0, VT_Mover); m.P(4, x.Va.at("driver")); m.P(8, VT_Vehicle); m.Str(0xC, "PhysMov");
    m.P(0x14, NodeVa(x, 0)); m.P(0x18, NodeVa(x, 10));
    m.I(0x1C, p.StateSize);
    m.P(0x20, x.Va.at("rb")); m.P(0x24, x.Va.at("steer"));
    for (int i = 0; i < 4; i++) { m.P(0x28 + 4 * i, x.Va.at(N("s", i))); m.P(0x38 + 4 * i, x.Va.at(N("w", i))); }
    m.P(0x48, x.Va.at("engine")); m.P(0x4C, x.Va.at("dt")); m.P(0x50, x.Va.at("aero")); m.P(0x54, x.CpiVa()); m.P(0x58, x.Cs()); m.P(0x5C, x.Va.at("car"));
    m.I(0x60, p.DampState); m.Bool(0x64, p.CgShifted); m.I(0x68, p.RoofResetTimer); m.F(0x6C, p.GripScale);
    m.F(0x70, p.EBrakeRearGrip); m.Ia(0x74, p.HydraulicState); m.Bool(0x84, p.IsOnDyno); m.Fa(0x90, p.DynoAnchor);
    m.Fa(0xE0, p.DynoPos); m.Fa(0xF0, p.DynoVel); m.Fa(0x100, p.AccForce); m.Fa(0x110, p.AccTorque); m.I(0x124, p.Field124);
}

void MapRb(Img m, Ctx& x) {
    RigidBody& rb = *x.Mover->Rb;
    m.P(0, VT_Rb); Links(m, x, 0); m.Str(0xC, "Rigid"); m.P(0x14, x.CpiVa() + 0x110);
    if (m.Read) rb.ReadFrom(m.B); else rb.WriteTo(m.B);
}

void MapSteer(Img m, Ctx& x) {
    TwoWheelAckermanSteering& st = *x.Mover->Steering;
    m.P(0, VT_Steer); m.P(4, x.CpiVa()); m.P(8, x.Va.at("steer") + 0x10);
    m.Fa(0x10, st.Mat[0]); m.Fa(0x50, st.Mat[1]); m.U16(0x90, st.Angle);
}

void MapWheel(Img m, Ctx& x, int i) {
    Wheel& w = *x.Mover->Wheels[i];
    uint32_t ui = static_cast<uint32_t>(i);
    m.P(0, VT_Wheel); Links(m, x, 1 + 2 * i); m.Str(0xC, "Wheel");
    m.P(0x14, x.CpiVa() + 0x10 + 0x30 * ui); m.P(0x18, x.CpiVa() + 0xD0 + 0x20 * (ui / 2)); m.P(0x1C, x.CpiVa() + 0x270); m.P(0x20, x.Cs());
    m.F(0x24, w.Angle); m.F(0x28, w.Omega); m.F(0x2C, w.Inertia);
    m.Fa(0x30, w.Spin); m.Fa(0x70, w.World); m.Fa(0xB0, w.Local); m.Fa(0xF0, w.ContactVel);
    m.F(0x100, w.LastSlip); m.F(0x104, w.LastLatVel); m.F(0x108, w.Torque);
    m.I(0x10C, w.BrakeLocked); m.I(0x110, w.RollState); m.I(0x114, w.Terrain); m.F(0x118, w.SlipVel);
    m.U8(0x11C, w.OnGround); m.F(0x120, w.NetTorqueFilt); m.F(0x124, w.Fx); m.F(0x128, w.Fy);
    m.Fa(0x130, w.ForceWorld); m.Fa(0x140, w.ContactPt); m.Fa(0x150, w.GroundNormal);
    m.U8(0x160, w.Sliding); m.F(0x164, w.Load); m.F(0x168, w.RoadNoise); m.F(0x16C, w.SlipCopy);
    m.F(0x170, w.GripUsage); m.F(0x174, w.FlatDrag); m.F(0x178, w.Brake); m.F(0x17C, w.EBrake);
    m.F(0x180, w.GripScale); m.F(0x184, w.GripScale2); m.F(0x188, w.LongGripScale); m.F(0x18C, w.RideOffset);
    m.F(0x190, w.LongForceScale); m.F(0x194, w.LatForceScale);
}

void MapSusp(Img m, Ctx& x, int i) {
    Suspension& s = *x.Mover->Susp[i];
    m.P(0, VT_Susp); Links(m, x, 2 + 2 * i); m.Str(0xC, "Suspnsn");
    m.P(0x14, x.CpiVa() + 0x170 + 0x20 * (static_cast<uint32_t>(i) / 2)); m.P(0x18, x.Va.at(N("s", i ^ 1)));
    m.F(0x1C, s.Compression); m.F(0x20, s.CompressionRate); m.F(0x24, s.SpringForce);
    // WheelPosLocal.w (+0x3C) is never written by the original: only xyz are modelled
    for (int k = 0; k < 3; k++) m.F(0x30 + 4 * k, s.WheelPosLocal[k]);
    m.Fa(0x40, s.LocalNoSteer); m.I(0x80, s.Touching); m.F(0x88, s.Preload); m.Fa(0x90, s.Attach);
    m.P(0xA0, x.Va.at(N("w", i))); m.P(0xA4, x.Va.at("rb")); m.P(0xA8, i < 2 ? x.Va.at("steer") : 0u); m.I(0xAC, s.Index); m.F(0xB0, s.ExchangeRet);
}

void MapTable(Img& m, Ctx& x, int o, Engine::Table& t) {
    m.I(o, t.Count); m.F(o + 4, t.Min); m.F(o + 8, t.Max); m.F(o + 12, t.InvStep);
    const EngineInfo& ei = x.Cpi->Engine;
    uint32_t y = t.Y == ei.Torque ? x.CpiVa() + 0x200 : t.Y == ei.EngineBrake ? x.CpiVa() + 0x248
               : t.Y == ei.Boost ? x.CpiVa() + 0x224 : NosCurveVA;
    m.P(o + 16, y);
}

void MapEngine(Img m, Ctx& x) {
    Engine& e = *x.Mover->Engine;
    m.P(0, VT_Engine); Links(m, x, 9); m.Str(0xC, "Engine");
    m.F(0x14, e.Omega); m.P(0x18, x.CpiVa() + 0x1F0); m.P(0x1C, x.Cs()); m.P(0x20, x.Va.at("dt"));
    MapTable(m, x, 0x24, e.TorqueTable); MapTable(m, x, 0x38, e.EngineBrakeTable); MapTable(m, x, 0x4C, e.BoostTable); MapTable(m, x, 0x60, e.NosTable);
    m.I(0x74, e.HpMultFlag); m.F(0x78, e.Throttle); m.F(0x7C, e.LoadTorque); m.F(0x80, e.ThrottleGate);
    m.I(0x84, e.NosLevel); m.Bool(0x88, e.RevLimiter); m.F(0x8C, e.Heat); m.F(0x90, e.TorqueScale);
    m.F(0x94, e.Unk94); m.F(0x98, e.MaxBoost); m.F(0x9C, e.Boost); m.I(0xA0, e.TurboState);
    m.F(0xA4, e.BoostAtRelease); m.F(0xA8, e.TurboTimer); m.F(0xAC, e.BoostPrev);
}

void MapDt(Img m, Ctx& x) {
    DriveTrain& d = *x.Mover->DriveTrain;
    m.P(0, VT_DriveTrain); Links(m, x, 10); m.Str(0xC, "Drvtrn");
    m.F(0x14, d.Clutch); m.P(0x18, x.CpiVa() + 0x1B0); m.P(0x1C, MiscVA + 0x144 * static_cast<uint32_t>(x.MiscType)); m.P(0x20, x.Va.at("engine"));
    for (int i = 0; i < 4; i++) m.P(0x24 + 4 * i, x.Va.at(N("w", i)));
    m.P(0x34, x.Cs());
    m.I(0x38, d.TargetGear); m.I(0x3C, d.Gear); m.I(0x40, d.GearAtRequest); m.I(0x44, d.ClutchState);
    m.I(0x48, d.ShiftPending); m.F(0x4C, d.DriveOmega); m.F(0x50, d.AutoTimer); m.I(0x54, d.LastRequest);
    m.F(0x58, d.LastRpmDiff); m.F(0x5C, d.ShiftLockTime); m.I(0x60, d.ClutchMode); m.F(0x64, d.BonusTime);
    m.F(0x68, d.ShiftBonus); m.F(0x6C, d.RpmAtShift); m.F(0x70, d.ClutchTorque); m.Bool(0x74, d.ClutchPedal);
    m.Fa(0x78, d.ShiftModifier); m.Fa(0x84, d.ShiftUp); m.Fa(0xA8, d.ShiftDown); m.Fa(0xCC, d.ShiftModRange);
    m.F(0xE4, d.PerfectRangeMul); m.F(0xE8, d.ClutchRateMul); m.F(0xEC, d.RearSplit); m.I(0xF0, d.PerfType);
}

void MapAero(Img m, Ctx& x) {
    Aerodynamics& a = *x.Mover->Aero;
    m.P(0, x.CpiVa() + 0x260); m.P(4, x.Va.at("rb"));
    m.F(0x8, a.SpeedScale); m.F(0xC, a.DragScale); m.F(0x10, a.Throttle); m.F(0x14, a.GroundFrac);
    m.F(0x18, a.Unk18); m.F(0x1C, a.Unk1C); m.Fa(0x20, a.DragForce); m.F(0x30, a.Enable); m.F(0x34, a.TerrainDrag);
}

// World.Seed is a uint; map it through an int view.
int seedTmp = 0;

void MapAll(Ctx& x, bool read, Mem& mem) {
    auto I = [&](const char* r) { return Img(mem.at(r), read); };
    MapCar(I("car"), x); MapCcb(I("ccb"), x); MapDriver(I("driver"), x);
    seedTmp = static_cast<int>(x.W->Seed);
    I("world").I(0xF8, seedTmp);
    if (!x.Mover) return;
    MapMover(I("mover"), x); MapRb(I("rb"), x); MapSteer(I("steer"), x);
    for (int i = 0; i < 4; i++) { MapWheel(Img(mem.at(N("w", i)), read), x, i); MapSusp(Img(mem.at(N("s", i)), read), x, i); }
    MapEngine(I("engine"), x); MapDt(I("dt"), x); MapAero(I("aero"), x);
}

float BF(const Bytes& v) { return RdF(v, 0); }
int BI(const Bytes& v) { return RdI(v, 0); }

void SetGlobals(World& w, const JVal& g) {
    for (size_t i = 0; i < g.k.size(); i++) {
        Bytes v = FromHex(g.a[i].s);
        RaceModeFlags& f = w.Flags;
        switch (Hex(g.k[i])) {
        case 0x7FBED8: w.TestRPM = BF(v); break;
        case 0x89E7F4: w.Difficulty = BI(v); break;
        case 0x86B36C: w.NoWheels = BI(v); break;
        case 0x86B31C: w.TriggerBounce = BI(v); break;
        case RaceModeFlags::VA_DragByte: f.DragByte = v[0]; break;
        case RaceModeFlags::VA_DriftByte: f.DriftByte = v[0]; break;
        case RaceModeFlags::VA_BurnoutByte: f.BurnoutByte = v[0]; break;
        case RaceModeFlags::VA_TweakIsBurnout: f.TweakIsBurnout = BI(v); break;
        case RaceModeFlags::VA_TweakIsDriftRace: f.TweakIsDriftRace = BI(v); break;
        case RaceModeFlags::VA_TweakIsDragRace: f.TweakIsDragRace = BI(v); break;
        case RaceModeFlags::VA_TweakBurnoutPhysics: f.TweakBurnoutPhysics = BI(v); break;
        case RaceModeFlags::VA_TweakDriftPhysics: f.TweakDriftPhysics = BI(v); break;
        case G_WorldTimer: w.WorldTimer = BI(v); break;
        case G_RealTimer: w.RealTimer = BI(v); break;
        case G_WorldTimeElapsed: w.WorldTimeElapsed = BF(v); break;
        case G_Ugly: w.UglyTimestepHack = BF(v); break;
        default: break;
        }
    }
}

std::string GlobHex(const World& w, uint32_t va) {
    auto h = [](const void* p) { return HexBytes(static_cast<const uint8_t*>(p), 4); };
    switch (va) {
    case G_WorldTimer: return h(&w.WorldTimer);
    case G_RealTimer: return h(&w.RealTimer);
    case G_WorldTimeElapsed: return h(&w.WorldTimeElapsed);
    case G_Ugly: return h(&w.UglyTimestepHack);
    default: return "?";
    }
}

Mem Regions(const JVal& e) {
    Mem d;
    for (size_t i = 0; i < e.k.size(); i++) d[std::string(e.k[i])] = FromHex(e.a[i].s);
    return d;
}

std::unique_ptr<PhysicsEnvironment> env;
FlatGround flat;

// Car fields that live in other objects (participant at Car+0x1C).
void CarConstants(Ctx& x, Mem& mem) {
    uint32_t part = RdU(mem.at("car"), 0x1C);
    x.C->HasParticipant = part != 0;
    if (part != 0) {
        if (part != x.Va.at("part")) throw std::runtime_error("participant outside the layout");
        const Bytes& pb = mem.count("part") ? mem.at("part") : x.M.at("part");
        x.C->ParticipantField10 = RdU16(pb.data(), 0x10);
        uint32_t p1e0 = RdU(pb, 0x1E0);
        x.C->ParticipantMode1E0 = p1e0 == part + 0x100 ? RdI(pb, 0x104) : 0;
    }
}

// Builds the C++ graph from the pre-ctor memory and runs the ported ctor.
void Build(Ctx& x, const JVal& root, const JVal& layout) {
    if (!env) env = std::make_unique<PhysicsEnvironment>(Speed2Image(ExePath()));
    for (size_t i = 0; i < layout.k.size(); i++) x.Va[std::string(layout.k[i])] = Hex(layout.a[i].s);
    x.M = Regions(root["init"]);
    env->World = World();
    x.W = &env->World;
    flat = FlatGround();
    env->Terrain = &flat;
    x.Cpi = CarPhysicsInfo::FromBytes(x.M.at("cpi").data());
    x.C = std::make_unique<Car>();
    x.C->Cpi = x.Cpi.get();
    x.C->State.Cpi = x.Cpi.get();
    x.C->State.Topology = reinterpret_cast<void*>(static_cast<intptr_t>(RdI(x.M.at("car"), 0x44)));
    x.C->RideLevel = static_cast<int8_t>(x.M.at("ride")[0x14]);
    x.C->RideRpmDecelIndex = RdF(x.M.at("ride"), 0x2F4);
    x.C->SlotType = RdI(x.M.at("slot"), 4);
    x.C->SlotFlag900 = x.M.at("slot")[0x900];
    CarConstants(x, x.M);
    x.Real = std::make_unique<RealDriver>();
    x.Driver = x.Real.get();
    x.C->Driver = x.Driver;
    MapAll(x, true, x.M);
    x.W->Seed = static_cast<uint32_t>(seedTmp);
    x.W->Flags.DragByte = 0;
    SetGlobals(*x.W, root["glob0"]);
    float split = x.Cpi->RearTorqueSplit();
    x.MiscType = X87::ParityEven(X87::Fcom(split, 0.2f) & 0x05) ? 1 : 0;
    if ((X87::Fcom(split, 0.8f) & 0x41) == 0) x.MiscType = 2;
    x.Mover = std::make_unique<PhysicsMover>(env.get(), x.Cpi.get(), static_cast<int8_t>(x.M.at("cti")[0xD8]), x.Driver, x.C.get());
    x.C->Mover = x.Mover.get();
}

void LoadSnapshot(Ctx& x, Mem& pre) {
    CarConstants(x, pre);
    if (RdU(pre.at("driver"), 0) == x.Va.at("vtbl")) {
        x.Yaw = std::make_unique<YawDriver>();
        x.Yaw->Yaw = 0x0C00;
        x.Driver = x.Yaw.get(); x.C->Driver = x.Driver; x.Mover->Driver = x.Driver;
    }
    MapAll(x, true, pre);
    x.W->Seed = static_cast<uint32_t>(seedTmp);
    for (auto& kv : pre) x.M[kv.first] = kv.second;
}

std::string Compare(Ctx& x, const JVal& op, const std::string& label) {
    MapAll(x, false, x.M);
    WrU(x.M.at("world").data(), 0xF8, x.W->Seed);
    std::string sb;
    const JVal& out = op["out"];
    for (size_t r = 0; r < out.k.size(); r++) {
        Bytes rec = FromHex(out.a[r].s);
        const Bytes& mine = x.M.at(std::string(out.k[r]));
        for (size_t i = 0; i < rec.size(); i += 4) {
            size_t n = std::min<size_t>(4, rec.size() - i);
            if (std::memcmp(rec.data() + i, mine.data() + i, n) != 0) {
                sb += " " + std::string(out.k[r]) + "+" + HexX(static_cast<uint32_t>(i)) + ":" + HexUpper(rec.data() + i, n) + "!=" + HexUpper(mine.data() + i, n);
                if (sb.size() > 400) break;
            }
        }
        if (sb.size() > 400) break;
    }
    const JVal& glob = op["glob"];
    for (size_t i = 0; i < glob.k.size(); i++) {
        std::string got = GlobHex(*x.W, Hex(glob.k[i]));
        if (got != glob.a[i].s) sb += " glob" + std::string(glob.k[i]) + ":" + std::string(glob.a[i].s) + "!=" + got;
    }
    return sb.empty() ? std::string() : label + sb;
}

class SuiteMover : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("mover_", 0) == 0; }

    void BeginFile(const std::string&, const JVal& root) override { layout_ = &root["layout"]; }

    void Evaluate(BWareMath&, const std::string&, GoldenCase& c, std::string& want, std::string& got) override {
        const JVal& root = *c.Root;
        std::string fail;
        try {
            Ctx x;
            Build(x, root, *layout_);
            if (const JVal* pre = root.Get("pre")) {
                Mem p = Regions(*pre);
                LoadSnapshot(x, p);
                SetGlobals(*x.W, root["preglob"]);
                if (const JVal* xg = root.Get("xglob")) SetGlobals(*x.W, *xg);
            }
            int k = 0;
            for (const JVal& op : root["ops"].a) {
                std::string kind = op["op"].Str();
                if (kind == "move") {
                    float dt = X87::Bits(Hex(op["dt"].s));
                    if (const JVal* inp = op.Get("inp")) {
                        x.W->WorldTimer = op["wt"].I32();
                        x.W->RealTimer = x.W->WorldTimer;
                        x.W->WorldTimeElapsed = dt;
                        x.C->State.BeginTimestep();
                        CarDriver& d = *x.Driver;
                        d.Throttle = X87::Bits(Hex((*inp)["thr"].s));
                        d.Brake = X87::Bits(Hex((*inp)["brk"].s));
                        d.EBrake = X87::Bits(Hex((*inp)["eb"].s));
                        d.SteerAngle = static_cast<uint16_t>((*inp)["steer"].I32());
                        if ((*inp)["gc"].IsNum()) d.GearCommand = (*inp)["gc"].I32();
                        if ((*inp)["nos"].I32() != 0) Nitrous::StartNitrous(x.C->State, x.W->NosNow(), env->Engine);
                    } else {
                        x.W->WorldTimeElapsed = dt;
                    }
                    x.Mover->Move(dt);
                } else if (kind == "substep") {
                    float h = X87::Bits(Hex(op["h"].s));
                    x.W->UglyTimestepHack = h;
                    x.Mover->IntegrateEuler(h);
                }
                fail = Compare(x, op, "op" + std::to_string(k) + "(" + kind + ")");
                if (!fail.empty()) break;
                k++;
            }
            // The mover must go before the car (components point into it).
            x.Mover.reset();
        } catch (const std::exception& e) {
            fail = std::string("exception: ") + e.what();
        }
        want = "ok";
        got = fail.empty() ? "ok" : fail;
    }

private:
    const JVal* layout_ = nullptr;
};

Register reg(new SuiteMover);

} // namespace
