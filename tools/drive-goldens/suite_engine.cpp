// Golden suite for the engine / drivetrain / nitrous area (golden/engine_*.json, gen_golden_engine.py).
// Port of NFSU2Unity tools/harness/csharp/SuiteEngine.cs. Each case carries the object graph as memory
// regions at fixed addresses (mem: {name: {va, hex}}) plus game globals (glob: {va: hex}). The suite builds
// the C++ objects from the bytes, runs the port, writes the modelled fields back over a copy of the input
// bytes and compares every region with the bytes recorded after the original call.

#include "golden.h"

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/Burnout.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/engine/Nitrous.h"
#include "drive/physics/wheel/Wheel.h"

#include <memory>
#include <stdexcept>

using namespace golden;
using namespace bb;

namespace {

constexpr uint32_t G_WorldTimer = 0x86518C, G_RealTimer = 0x8651AC, G_UglyTimestepHack = 0x7FC800, G_WorldTimeElapsed = 0x865154;
constexpr uint32_t G_DragByte = 0x89E7D8, G_TweakIsDragRace = 0x890108, G_ShiftPointSearch = 0x86B33C;
constexpr uint32_t G_OnlineByte = 0x89CF48 + 0x21, G_Online1 = 0x8669F4, G_Online2 = 0x866A14;
constexpr uint32_t G_NoBurnout = 0x89E7E2, G_TweakIsBurnout = 0x890100, G_TweakBurnoutPhysics = 0x890110;
constexpr uint32_t VT_Engine = 0x7A235C, VT_DriveTrain = 0x7A2508, NosCurveVA = 0x7FBF38;

std::unique_ptr<Speed2Image> exeImg;
std::unique_ptr<PhysicsEnvironment> envPtr;
PhysicsEnvironment& Env() {
    if (!envPtr) {
        exeImg = std::make_unique<Speed2Image>(ExePath());
        envPtr = std::make_unique<PhysicsEnvironment>(*exeImg);
    }
    return *envPtr;
}
const EngineTables& K() { return Env().Engine; }

struct Region { uint32_t Va = 0; Bytes In, Got; };

struct Graph {
    std::vector<std::pair<std::string, Region>> Mem;   // file order (for the report)
    std::map<uint32_t, Bytes> Glob;
    EngineInfo Ei; DriveTrainInfo Dti;
    std::unique_ptr<CarPhysicsInfo> Cpi;
    std::unique_ptr<bb::Car> Car;
    CarState* Cs = nullptr;
    WheelInfo Wi[4];
    std::unique_ptr<Wheel> W[4];
    std::unique_ptr<Engine> Eng;
    std::unique_ptr<DriveTrain> Dt;
    float Misc[0x144 / 4] = {};
    uint32_t Seed = 0;
    bool ReplayLocked = false;

    Region& M(const std::string& n) {
        for (auto& p : Mem) if (p.first == n) return p.second;
        throw std::runtime_error("missing region " + n);
    }
    int G(uint32_t va) const { return RdI(Glob.at(va), 0); }
    float Gf(uint32_t va) const { return RdF(Glob.at(va), 0); }
    uint8_t Gb(uint32_t va) const { return Glob.at(va)[0]; }
    bool IsDrag() const { return Gb(G_DragByte) != 0 || G(G_TweakIsDragRace) != 0; }
    int NosNow() const { return Gb(G_OnlineByte) != 0 || G(G_Online1) > 0 || G(G_Online2) > 0 ? G(G_RealTimer) : G(G_WorldTimer); }
};

void Fa(const Bytes& b, int o, float* dst, int n) { for (int i = 0; i < n; i++) dst[i] = RdF(b, o + 4 * i); }
void WFa(Bytes& b, int o, const float* src, int n) { for (int i = 0; i < n; i++) WrF(b, o + 4 * i, src[i]); }
void Wu(Bytes& b, int o, uint32_t v) { WrU(b.data(), o, v); }

void ReadTable(Graph& g, const Bytes& e, int o, Engine::Table& t) {
    t.Count = RdI(e, o); t.Min = RdF(e, o + 4); t.Max = RdF(e, o + 8); t.InvStep = RdF(e, o + 12);
    uint32_t y = RdU(e, o + 16), ei = g.M("einfo").Va;
    t.Y = y == ei + 0x10 ? g.Ei.Torque : y == ei + 0x58 ? g.Ei.EngineBrake : y == ei + 0x34 ? g.Ei.Boost
        : y == NosCurveVA ? K().NosCurve.data() : nullptr;   // null: constructor input (garbage), rewritten by the ctor
}

uint32_t TableY(Graph& g, const float* y) {
    uint32_t ei = g.M("einfo").Va;
    if (y == g.Ei.Torque) return ei + 0x10;
    if (y == g.Ei.EngineBrake) return ei + 0x58;
    if (y == g.Ei.Boost) return ei + 0x34;
    if (y == K().NosCurve.data()) return NosCurveVA;
    throw std::runtime_error("unknown table data");
}

std::unique_ptr<Graph> Load(GoldenCase& c) {
    auto gp = std::make_unique<Graph>();
    Graph& g = *gp;
    const JVal& mem = (*c.Root)["mem"];
    for (size_t i = 0; i < mem.k.size(); i++) {
        Region r;
        r.Va = Hex(mem.a[i]["va"].s);
        r.In = FromHex(mem.a[i]["hex"].s);
        r.Got = r.In;
        g.Mem.emplace_back(std::string(mem.k[i]), std::move(r));
    }
    const JVal& glob = (*c.Root)["glob"];
    for (size_t i = 0; i < glob.k.size(); i++) g.Glob[Hex(glob.k[i])] = FromHex(glob.a[i].s);

    const Bytes& ei = g.M("einfo").In;
    g.Ei.IdleRpm = RdF(ei, 0); g.Ei.RedlineRpm = RdF(ei, 4); g.Ei.MaxRpm = RdF(ei, 8); g.Ei.Inertia = RdF(ei, 0xC);
    Fa(ei, 0x10, g.Ei.Torque, 9); Fa(ei, 0x34, g.Ei.Boost, 9); Fa(ei, 0x58, g.Ei.EngineBrake, 3);
    const Bytes& di = g.M("dtinfo").In;
    g.Dti.NeutralShiftFrac = RdF(di, 0); g.Dti.Unk04 = RdF(di, 4); g.Dti.FinalDriveFront = RdF(di, 8); g.Dti.FinalDriveRear = RdF(di, 0xC);
    g.Dti.RearTorqueSplit = RdF(di, 0x10); g.Dti.ClutchStiffness = RdF(di, 0x14); g.Dti.NumGears = RdI(di, 0x18); g.Dti.ClutchRate = RdF(di, 0x1C);
    Fa(di, 0x20, g.Dti.GearRatio, 8);
    const Bytes& cs = g.M("cs").In;
    g.Car = std::make_unique<bb::Car>();
    CarState* c0 = g.Cs = &g.Car->State;
    g.Cpi = CarPhysicsInfo::FromBytes(g.M("cpi").In.data());
    c0->Cpi = g.Cpi.get();
    c0->WheelsOnGround = RdI(cs, 0x3E4); c0->Speed = RdF(cs, 0x3F0);
    c0->NosCapacity = RdI(cs, 0x414); c0->NosShot = RdI(cs, 0x418); c0->NosRemaining = RdI(cs, 0x41C); c0->NosStartTime = RdI(cs, 0x420);
    c0->BurnoutCount = RdI(cs, 0x42C); c0->Vel[0] = RdF(cs, 0x30); c0->Vel[1] = RdF(cs, 0x34);
    c0->Heading = RdU16(cs.data(), 0x3CC); c0->ForwardSpeed = RdF(cs, 0x3EC); c0->BurnoutTimer = RdF(cs, 0x430);
    c0->BurnoutIntensity = RdF(cs, 0x434); c0->BurnoutStill = RdF(cs, 0x438);
    for (int i = 0; i < 4; i++) {
        const Bytes& w = g.M("w" + std::to_string(i)).In;
        g.Wi[i] = WheelInfo{};
        g.Wi[i].Radius = RdF(g.M("wi" + std::to_string(i)).In, 0x10);
        g.W[i] = std::make_unique<Wheel>(&Env(), &g.Wi[i], nullptr, nullptr, g.Cs);
        Wheel& wh = *g.W[i];
        wh.Omega = RdF(w, 0x28); wh.Inertia = RdF(w, 0x2C); wh.Torque = RdF(w, 0x108);
        wh.BrakeLocked = RdI(w, 0x10C); wh.SlipVel = RdF(w, 0x118); wh.OnGround = w[0x11C]; wh.Fx = RdF(w, 0x124);
        wh.SlipCopy = RdF(w, 0x16C); wh.Load = RdF(w, 0x164); wh.GripScale = RdF(w, 0x180);
    }
    Fa(g.M("misc").In, 0, g.Misc, 0x144 / 4);
    g.Seed = RdU(g.M("world").In, 0xF8);
    g.ReplayLocked = RdU(g.M("rm").In, 0xA0) != 0 && g.M("rmdata").In[0x41] != 0;

    g.Eng = std::make_unique<Engine>(K());
    g.Dt = std::make_unique<DriveTrain>(K());
    const Bytes& e = g.M("engine").In;
    Engine& eng = *g.Eng;
    eng.Omega = RdF(e, 0x14); eng.Info = &g.Ei; eng.CarState = g.Cs; eng.DriveTrain = g.Dt.get();
    ReadTable(g, e, 0x24, eng.TorqueTable); ReadTable(g, e, 0x38, eng.EngineBrakeTable);
    ReadTable(g, e, 0x4C, eng.BoostTable); ReadTable(g, e, 0x60, eng.NosTable);
    eng.HpMultFlag = RdI(e, 0x74); eng.Throttle = RdF(e, 0x78); eng.LoadTorque = RdF(e, 0x7C); eng.ThrottleGate = RdF(e, 0x80);
    eng.NosLevel = RdI(e, 0x84); eng.RevLimiter = e[0x88] != 0; eng.Heat = RdF(e, 0x8C); eng.TorqueScale = RdF(e, 0x90);
    eng.Unk94 = RdF(e, 0x94); eng.MaxBoost = RdF(e, 0x98); eng.Boost = RdF(e, 0x9C); eng.TurboState = RdI(e, 0xA0);
    eng.BoostAtRelease = RdF(e, 0xA4); eng.TurboTimer = RdF(e, 0xA8); eng.BoostPrev = RdF(e, 0xAC);

    const Bytes& d = g.M("dt").In;
    DriveTrain& dt = *g.Dt;
    dt.Clutch = RdF(d, 0x14); dt.Info = &g.Dti; dt.MiscTunings = g.Misc; dt.Engine = &eng;
    for (int i = 0; i < 4; i++) dt.Wheels[i] = g.W[i].get();
    dt.CarState = g.Cs;
    dt.TargetGear = RdI(d, 0x38); dt.Gear = RdI(d, 0x3C); dt.GearAtRequest = RdI(d, 0x40); dt.ClutchState = RdI(d, 0x44);
    dt.ShiftPending = RdI(d, 0x48); dt.DriveOmega = RdF(d, 0x4C); dt.AutoTimer = RdF(d, 0x50); dt.LastRequest = RdI(d, 0x54);
    dt.LastRpmDiff = RdF(d, 0x58); dt.ShiftLockTime = RdF(d, 0x5C); dt.ClutchMode = RdI(d, 0x60); dt.BonusTime = RdF(d, 0x64);
    dt.ShiftBonus = RdF(d, 0x68); dt.RpmAtShift = RdF(d, 0x6C); dt.ClutchTorque = RdF(d, 0x70); dt.ClutchPedal = d[0x74] != 0;
    Fa(d, 0x78, dt.ShiftModifier, 3); Fa(d, 0x84, dt.ShiftUp, 9); Fa(d, 0xA8, dt.ShiftDown, 9); Fa(d, 0xCC, dt.ShiftModRange, 6);
    dt.PerfectRangeMul = RdF(d, 0xE4); dt.ClutchRateMul = RdF(d, 0xE8); dt.RearSplit = RdF(d, 0xEC); dt.PerfType = RdI(d, 0xF0);
    return gp;
}

void WriteTable(Graph& g, Bytes& e, int o, const Engine::Table& t) {
    WrI(e, o, t.Count); WrF(e, o + 4, t.Min); WrF(e, o + 8, t.Max); WrF(e, o + 12, t.InvStep); Wu(e, o + 16, TableY(g, t.Y));
}

void Store(Graph& g, bool engineCtor, bool dtCtor) {
    Engine& eng = *g.Eng;
    Bytes& e = g.M("engine").Got;
    WrF(e, 0x14, eng.Omega);
    if (engineCtor) {
        Wu(e, 0, VT_Engine);
        std::memcpy(e.data() + 0xC, "Engine", 7);
        Wu(e, 0x18, g.M("einfo").Va); Wu(e, 0x1C, g.M("cs").Va);
        Wu(e, 0x20, eng.DriveTrain == nullptr ? 0u : g.M("dt").Va);
        WriteTable(g, e, 0x24, eng.TorqueTable); WriteTable(g, e, 0x38, eng.EngineBrakeTable);
        WriteTable(g, e, 0x4C, eng.BoostTable); WriteTable(g, e, 0x60, eng.NosTable);
    }
    WrI(e, 0x74, eng.HpMultFlag); WrF(e, 0x78, eng.Throttle); WrF(e, 0x7C, eng.LoadTorque); WrF(e, 0x80, eng.ThrottleGate);
    WrI(e, 0x84, eng.NosLevel); e[0x88] = static_cast<uint8_t>(eng.RevLimiter ? 1 : 0); WrF(e, 0x8C, eng.Heat); WrF(e, 0x90, eng.TorqueScale);
    WrF(e, 0x94, eng.Unk94); WrF(e, 0x98, eng.MaxBoost); WrF(e, 0x9C, eng.Boost); WrI(e, 0xA0, eng.TurboState);
    WrF(e, 0xA4, eng.BoostAtRelease); WrF(e, 0xA8, eng.TurboTimer); WrF(e, 0xAC, eng.BoostPrev);

    DriveTrain& dt = *g.Dt;
    Bytes& d = g.M("dt").Got;
    if (dtCtor) {
        Wu(d, 0, VT_DriveTrain);
        std::memcpy(d.data() + 0xC, "Drvtrn", 7);
        Wu(d, 0x18, g.M("dtinfo").Va); Wu(d, 0x1C, g.M("misc").Va); Wu(d, 0x20, g.M("engine").Va);
        for (int i = 0; i < 4; i++) {
            int k = -1;
            for (int j = 0; j < 4; j++) if (g.W[j].get() == dt.Wheels[i]) k = j;
            Wu(d, 0x24 + 4 * i, g.M("w" + std::to_string(k)).Va);
        }
        Wu(d, 0x34, g.M("cs").Va);
    }
    WrF(d, 0x14, dt.Clutch);
    WrI(d, 0x38, dt.TargetGear); WrI(d, 0x3C, dt.Gear); WrI(d, 0x40, dt.GearAtRequest); WrI(d, 0x44, dt.ClutchState);
    WrI(d, 0x48, dt.ShiftPending); WrF(d, 0x4C, dt.DriveOmega); WrF(d, 0x50, dt.AutoTimer); WrI(d, 0x54, dt.LastRequest);
    WrF(d, 0x58, dt.LastRpmDiff); WrF(d, 0x5C, dt.ShiftLockTime); WrI(d, 0x60, dt.ClutchMode); WrF(d, 0x64, dt.BonusTime);
    WrF(d, 0x68, dt.ShiftBonus); WrF(d, 0x6C, dt.RpmAtShift); WrF(d, 0x70, dt.ClutchTorque); d[0x74] = static_cast<uint8_t>(dt.ClutchPedal ? 1 : 0);
    WFa(d, 0x78, dt.ShiftModifier, 3); WFa(d, 0x84, dt.ShiftUp, 9); WFa(d, 0xA8, dt.ShiftDown, 9); WFa(d, 0xCC, dt.ShiftModRange, 6);
    WrF(d, 0xE4, dt.PerfectRangeMul); WrF(d, 0xE8, dt.ClutchRateMul); WrF(d, 0xEC, dt.RearSplit); WrI(d, 0xF0, dt.PerfType);

    for (int i = 0; i < 4; i++) {
        Bytes& w = g.M("w" + std::to_string(i)).Got;
        Wheel& x = *g.W[i];
        WrF(w, 0x28, x.Omega); WrF(w, 0x2C, x.Inertia); WrF(w, 0x108, x.Torque); WrI(w, 0x10C, x.BrakeLocked);
        WrF(w, 0x118, x.SlipVel); w[0x11C] = x.OnGround; WrF(w, 0x124, x.Fx); WrF(w, 0x16C, x.SlipCopy);
        WrF(w, 0x164, x.Load); WrF(w, 0x180, x.GripScale);
    }
    Bytes& cs = g.M("cs").Got;
    CarState& s = *g.Cs;
    WrI(cs, 0x3E4, s.WheelsOnGround); WrF(cs, 0x3F0, s.Speed); WrI(cs, 0x414, s.NosCapacity); WrI(cs, 0x418, s.NosShot);
    WrI(cs, 0x41C, s.NosRemaining); WrI(cs, 0x420, s.NosStartTime); WrI(cs, 0x42C, s.BurnoutCount);
    WrF(cs, 0x30, s.Vel[0]); WrF(cs, 0x34, s.Vel[1]); WrU16(cs.data(), 0x3CC, s.Heading);
    WrF(cs, 0x3EC, s.ForwardSpeed); WrF(cs, 0x430, s.BurnoutTimer); WrF(cs, 0x434, s.BurnoutIntensity);
    WrF(cs, 0x438, s.BurnoutStill);
    Wu(g.M("world").Got, 0xF8, g.Seed);
}

std::string Hex32(int v) { return Hex8(static_cast<uint32_t>(v)); }

class SuiteEngine : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("engine_", 0) == 0; }

    void Evaluate(BWareMath& math, const std::string& name, GoldenCase& c, std::string& want, std::string& got) override {
        auto gp = Load(c);
        Graph& g = *gp;
        const JVal& o = c.Out();
        std::string wRet, gRet;
        auto Eax = [&] { return o["eax"].Str(); };
        auto St0 = [&] { return o["f32"].Str(); };
        bool engineCtor = false, dtCtor = false;
        DriveTrain& dt = *g.Dt;
        Engine& eng = *g.Eng;
        const std::string& n = name;
        if (n == "engine_GetDriveWheelSlippage") { wRet = St0(); gRet = F32(dt.GetDriveWheelSlippage()); }
        else if (n == "engine_GetDiffAngVel") { wRet = St0(); gRet = F32(dt.GetDifferentialAngularVelocity()); }
        else if (n == "engine_GetDriveWheelAngAccel") { wRet = St0(); gRet = F32(dt.GetDriveWheelAngularAccel()); }
        else if (n == "engine_SetManualClutch") dt.SetManualClutchEngaged(c.F(0));
        else if (n == "engine_DTGetDerivative") WrF(g.M("up").Got, 0, dt.GetDerivative());
        else if (n == "engine_RateShift") { wRet = St0(); gRet = F32(dt.RateShift(c.F(0), static_cast<int>(c.U(1)), (c.U(2) & 0xFF) != 0)); }
        else if (n == "engine_GetShiftUpRPM") { wRet = St0(); gRet = F32(dt.GetShiftUpRpm(static_cast<int>(c.U(0)), (c.U(1) & 0xFF) != 0, g.IsDrag())); }
        else if (n == "engine_GetShiftDownRPM") { wRet = St0(); gRet = F32(dt.GetShiftDownRpm(static_cast<int>(c.U(0)), (c.U(1) & 0xFF) != 0)); }
        else if (n == "engine_ShouldAutoDisengage") { wRet = Eax(); gRet = Hex32(dt.ShouldAutoDisengage() ? 1 : 0); }
        else if (n == "engine_IsClutchSynced") { wRet = Eax(); gRet = Hex32(dt.IsClutchSynced() ? 1 : 0); }
        else if (n == "engine_EffThrottle") { wRet = St0(); gRet = F32(eng.GetEffectiveThrottle()); }
        else if (n == "engine_IsBlown") { wRet = Eax(); gRet = Hex32(eng.IsBlown() ? 1 : 0); }
        else if (n == "engine_UpdateHeat") eng.UpdateHeat(c.F(0));
        else if (n == "engine_TotalTorque") { wRet = St0(); gRet = F32(eng.GetTotalEngineTorque()); }
        else if (n == "engine_EngGetDerivative") WrF(g.M("up").Got, 0, eng.GetDerivative());
        else if (n == "engine_UpdateTurbo") eng.UpdateTurbo(c.F(0));
        else if (n == "engine_EngComputeAux") eng.ComputeAuxiliaries(g.Gf(G_UglyTimestepHack));
        else if (n == "engine_GetMaxBoost") { wRet = St0(); gRet = F32(Engine::GetMaxBoost(&g.Ei)); }
        else if (n == "engine_EngineCtor") { eng.Construct(&g.Ei, g.Cs); engineCtor = true; }
        else if (n == "engine_ApplyClutchTorque") dt.ApplyClutchTorque(c.F(0));
        else if (n == "engine_GuessClutchTorque") dt.GuessClutchTorque();
        else if (n == "engine_ModifyClutchTorque") dt.ModifyClutchTorque();
        else if (n == "engine_FinalizeClutchTorque") dt.FinalizeClutchTorque();
        else if (n == "engine_SetDiffAngVel") dt.SetDifferentialAngularVelocity(c.F(0));
        else if (n == "engine_DTComputeAux") dt.ComputeAuxiliaries(g.G(G_WorldTimer), g.Seed, g.ReplayLocked);
        else if (n == "engine_ChooseShiftMods") dt.ChooseNewShiftModifiers(g.Seed, g.ReplayLocked);
        else if (n == "engine_WorldRandom") { wRet = St0(); gRet = F32(World::BRandom(c.F(0), g.Seed)); }
        else if (n == "engine_CalcShiftPoints") {
            float up[9], down[9];
            Fa(g.M("up").In, 0, up, 9); Fa(g.M("down").In, 0, down, 9);
            DriveTrain::CalculateShiftPoints(&g.Dti, &g.Ei, up, down, g.G(G_ShiftPointSearch) != 0);
            WFa(g.M("up").Got, 0, up, 9); WFa(g.M("down").Got, 0, down, 9);
        } else if (n == "engine_DriveTrainCtor") {
            const Bytes& wp = g.M("wptrs").In;
            Wheel* wheels[4] = {};
            for (int i = 0; i < 4; i++) {
                uint32_t va = RdU(wp, 4 * i);
                for (int k = 0; k < 4; k++)
                    if (g.M("w" + std::to_string(k)).Va == va) { wheels[i] = g.W[k].get(); break; }
                if (!wheels[i]) throw std::runtime_error("wheel pointer outside the layout");
            }
            dt.Construct(&g.Dti, g.Misc, &eng, wheels, g.Cs, g.G(G_ShiftPointSearch) != 0, g.Seed, g.ReplayLocked);
            dtCtor = true;
        } else if (n == "engine_RecordShiftQuality") dt.RecordShiftQuality(c.F(0), g.IsDrag());
        else if (n == "engine_GearSelection") {
            wRet = Eax();
            gRet = Hex32(dt.UpdateGearSelection(static_cast<int>(c.U(0)), c.F(1), g.G(G_WorldTimer), g.Gf(G_WorldTimeElapsed), g.IsDrag()));
        } else if (n == "engine_NosSecondsLeft") { wRet = St0(); gRet = F32(Nitrous::GetNosSecondsLeft(*g.Cs, g.NosNow())); }
        else if (n == "engine_NosPercentLeft") { wRet = St0(); gRet = F32(Nitrous::GetNosPercentageLeft(*g.Cs, g.NosNow())); }
        else if (n == "engine_StopNitrous") Nitrous::StopNitrous(*g.Cs, g.NosNow(), K().NitrousContinuous);
        else if (n == "engine_IsNitrousOn") { wRet = Eax(); gRet = Hex32(Nitrous::IsNitrousOn(*g.Cs, g.NosNow(), K())); }
        else if (n == "engine_CalcBurnout") {
            const Bytes& car = g.M("car").In;
            g.Car->DriverType = RdI(car, 0x24); g.Car->BlinkStart = RdI(car, 0x974); g.Car->BlinkDuration = RdF(car, 0x978);
            g.Car->RideLevel = static_cast<int8_t>(g.M("ride").In[0x14]);
            World world;
            world.RaceState = RdI(g.M("race").In, 0); world.WorldTimer = g.G(G_WorldTimer); world.WorldTimeElapsed = g.Gf(G_WorldTimeElapsed);
            world.Flags.BurnoutByte = g.Gb(G_NoBurnout); world.Flags.TweakIsBurnout = g.G(G_TweakIsBurnout);
            world.Flags.TweakBurnoutPhysics = g.G(G_TweakBurnoutPhysics);
            if (RdF(car, 0x42C) != g.Cs->ForwardSpeed) throw std::runtime_error("Car+0x42C must mirror CarState+0x3EC");
            Wheel* ws[4] = {g.W[0].get(), g.W[1].get(), g.W[2].get(), g.W[3].get()};
            Burnout::CalcBurnout(K(), math, world, *g.Car, *g.Cs->Cpi, ws, dt);
        } else if (n == "engine_StartNitrous") {
            wRet = "0x" + [&] { char t[8]; std::snprintf(t, sizeof t, "%02X", Hex(Eax()) & 0xFF); return std::string(t); }();
            gRet = Nitrous::StartNitrous(*g.Cs, g.NosNow(), K()) ? "0x01" : "0x00";
        } else throw std::runtime_error("no port registered for golden '" + name + "'");

        Store(g, engineCtor, dtCtor);
        std::string sw = "ret=" + wRet, sg = "ret=" + gRet;
        const JVal& outMem = o["mem"];
        for (auto& kv : g.Mem) {
            Bytes rec = FromHex(outMem[kv.first].s);
            const Bytes& mine = kv.second.Got;
            for (size_t i = 0; i < rec.size(); i += 4) {
                size_t len = std::min<size_t>(4, rec.size() - i);
                if (i + len > mine.size() || std::memcmp(rec.data() + i, mine.data() + i, len) != 0) {
                    sw += " " + kv.first + "+" + HexX(static_cast<uint32_t>(i)) + "=" + HexUpper(rec.data() + i, len);
                    sg += " " + kv.first + "+" + HexX(static_cast<uint32_t>(i)) + "=" +
                          (i + len <= mine.size() ? HexUpper(mine.data() + i, len) : std::string("?"));
                }
            }
        }
        const JVal& og = o["glob"];
        for (size_t i = 0; i < og.k.size(); i++) {
            Bytes rec = FromHex(og.a[i].s);
            if (rec != g.Glob.at(Hex(og.k[i]))) {
                sw += " glob" + std::string(og.k[i]) + "=" + og.a[i].Str();
                sg += " glob" + std::string(og.k[i]) + "=unchanged";
            }
        }
        want = sw;
        got = sg;
    }
};

golden::Register reg(new SuiteEngine);

} // namespace
