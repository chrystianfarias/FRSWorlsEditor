// Golden suite for the simulation clock ("time_*") and the player-input chain ("input_*")
// (port of NFSU2Unity tools/harness/csharp/SuiteTiming.cs; goldens from gen_golden_timing.py).
// Every case has "in" and "out" dicts of named values (floats as hex bit patterns, ints as hex u32).
// want/got are "key=value" lists; for sequences only the first differing step is reported.

#include "golden.h"

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/PhysicsMover.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/input/InputTables.h"
#include "drive/physics/input/PlayerSteering.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/timing/SimClock.h"
#include "drive/physics/wheel/Graph.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/WheelData.h"

#include <memory>
#include <stdexcept>

using namespace golden;
using namespace bb;

namespace {

using KV = std::vector<std::pair<std::string, std::string>>;

uint32_t U(const JVal& o, const char* k) { return Hex(o[k].s); }
int I(const JVal& o, const char* k) { return static_cast<int>(U(o, k)); }
float F(const JVal& o, const char* k) { return X87::Bits(U(o, k)); }
std::string H(uint32_t v) { return Hex8(v); }
std::string Hi(int v) { return Hex8(static_cast<uint32_t>(v)); }
std::string Hf(float v) { return Hex8(X87::Bits(v)); }
std::string B(uint8_t v) { return Hex8(v); }

std::vector<float> Arr(const JVal& o, const char* k) {
    std::vector<float> r;
    for (const JVal& x : o[k].a) r.push_back(X87::Bits(Hex(x.s)));
    return r;
}

// Compares the expected keys of 'want' against 'got' (keys of got, in order).
void Cmp(const JVal& want, const KV& got, std::string& w, std::string& g, const std::string& prefix = "") {
    for (auto& kv : got) {
        const JVal* e = want.Get(kv.first);
        if (!e) throw std::runtime_error("golden lacks key " + kv.first);
        std::string ws;
        if (e->IsArr()) {
            for (size_t i = 0; i < e->a.size(); i++) { if (i) ws += ","; ws += e->a[i].Str(); }
        } else ws = e->Str();
        w += prefix + kv.first + "=" + ws + " ";
        g += prefix + kv.first + "=" + kv.second + " ";
    }
}

bool Step(const JVal& want, const KV& got, int k, std::string& w, std::string& g) {
    std::string ws, gs;
    Cmp(want, got, ws, gs, "s" + std::to_string(k) + ".");
    if (ws == gs) return true;
    w = ws; g = gs;
    return false;
}

TimeGlobals LoadGlobals(const JVal& e) {
    TimeGlobals t;
    for (size_t i = 0; i < e.k.size(); i++) {
        std::string n(e.k[i]);
        uint32_t v = Hex(e.a[i].s);
        int iv = static_cast<int>(v); float fv = X87::Bits(v);
        if (n == "LimitMinimumVideoTimeElapsed") t.LimitMinimumVideoTimeElapsed = fv;
        else if (n == "MaxTicksPerTimestep") t.MaxTicksPerTimestep = fv;
        else if (n == "CurrentVideoMode") t.CurrentVideoMode = iv;
        else if (n == "Rem") t.TimestepRemainder = fv;
        else if (n == "Ticks") t.TimestepTicks = iv;
        else if (n == "RealTimeElapsed") t.RealTimeElapsed = fv;
        else if (n == "RealTimeElapsedFrame") t.RealTimeElapsedFrame = fv;
        else if (n == "RealTimeFrames") t.RealTimeFrames = iv;
        else if (n == "RealTimeFramesElapsed") t.RealTimeFramesElapsed = iv;
        else if (n == "gCardiogram") t.gCardiogram = iv;
        else if (n == "RealTimer") t.RealTimer = iv;
        else if (n == "RealLoopCounter") t.RealLoopCounter = iv;
        else if (n == "WorldTimeElapsed") t.WorldTimeElapsed = fv;
        else if (n == "WorldTimeElapsedFrame") t.WorldTimeElapsedFrame = fv;
        else if (n == "WorldTimeFrames") t.WorldTimeFrames = iv;
        else if (n == "WorldTimeFramesElapsed") t.WorldTimeFramesElapsed = iv;
        else if (n == "WorldTime") t.WorldTime = iv;
        else if (n == "WorldTimer") t.WorldTimer = iv;
        else if (n == "WorldTimeSeconds") t.WorldTimeSeconds = fv;
        else if (n == "WorldLoopCounter") t.WorldLoopCounter = iv;
        else if (n == "NeedToPrepareWorldTimestep") t.NeedToPrepareWorldTimestep = iv;
        else throw std::runtime_error("unknown global " + n);
    }
    return t;
}

KV SaveGlobals(const TimeGlobals& t) {
    return {
        {"RealTimeElapsed", Hf(t.RealTimeElapsed)}, {"RealTimeElapsedFrame", Hf(t.RealTimeElapsedFrame)},
        {"RealTimeFrames", Hi(t.RealTimeFrames)}, {"RealTimeFramesElapsed", Hi(t.RealTimeFramesElapsed)},
        {"gCardiogram", Hi(t.gCardiogram)}, {"RealTimer", Hi(t.RealTimer)}, {"RealLoopCounter", Hi(t.RealLoopCounter)},
        {"WorldTimeElapsed", Hf(t.WorldTimeElapsed)}, {"WorldTimeElapsedFrame", Hf(t.WorldTimeElapsedFrame)},
        {"WorldTimeFrames", Hi(t.WorldTimeFrames)}, {"WorldTimeFramesElapsed", Hi(t.WorldTimeFramesElapsed)},
        {"WorldTime", Hi(t.WorldTime)}, {"WorldTimer", Hi(t.WorldTimer)}, {"WorldTimeSeconds", Hf(t.WorldTimeSeconds)},
        {"WorldLoopCounter", Hi(t.WorldLoopCounter)}, {"NeedToPrepareWorldTimestep", Hi(t.NeedToPrepareWorldTimestep)},
    };
}

void CopyFlagsFrom(const JVal& e, RaceModeFlags& f) {
    f.DragByte = static_cast<uint8_t>(U(e, "byte_89E7D8") != 0 ? 1 : 0);
    f.DriftByte = static_cast<uint8_t>(U(e, "byte_89E7D9") != 0 ? 1 : 0);
    f.BurnoutByte = static_cast<uint8_t>(U(e, "byte_89E7E2") != 0 ? 1 : 0);
    f.TweakIsBurnout = U(e, "g_tweakIsBurnout") != 0 ? 1 : 0;
    f.TweakIsDriftRace = U(e, "g_tweakIsDriftRace") != 0 ? 1 : 0;
    f.TweakIsDragRace = U(e, "g_tweakIsDragRace") != 0 ? 1 : 0;
    f.TweakBurnoutPhysics = U(e, "g_tweakBurnoutPhysics") != 0 ? 1 : 0;
    f.TweakDriftPhysics = U(e, "g_tweakDriftPhysics") != 0 ? 1 : 0;
}

const Speed2Image& Exe() { static Speed2Image e(ExePath()); return e; }
const InputTables& Tables() { static InputTables t(Exe()); return t; }
PhysicsEnvironment& PEnv() { static PhysicsEnvironment p(Exe()); return p; }

class SuiteTiming : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("time_", 0) == 0 || n.rfind("input_", 0) == 0; }

    void Evaluate(BWareMath&, const std::string& name, GoldenCase& c, std::string& want, std::string& got) override {
        const JVal& inp = (*c.Root)["in"];
        const JVal& o = (*c.Root)["out"];
        KV r;
        want.clear(); got.clear();
        if (name == "time_prepare") {
            TimeGlobals t;
            t.LimitMinimumVideoTimeElapsed = F(inp, "Limit"); t.MaxTicksPerTimestep = F(inp, "MaxTicks");
            t.CurrentVideoMode = I(inp, "VideoMode"); t.TimestepRemainder = F(inp, "Rem");
            float real = F(inp, "realSec");
            float waited = t.PrepareRealTimestep(real);
            r = {{"Rem", Hf(t.TimestepRemainder)}, {"Ticks", Hi(t.TimestepTicks)}, {"RealTimeElapsed", Hf(t.RealTimeElapsed)},
                 {"Adj", Hf(waited != 0.0f ? static_cast<float>(waited + real) : real)}};
        } else if (name == "time_tickerdiff") {
            Ticker tk; tk.Shift = I(inp, "shift"); tk.MsPerTick = F(inp, "ms");
            r = {{"f32", Hf(tk.DifferenceMs(U(inp, "a"), U(inp, "b")))}};
        } else if (name == "time_tickerinit" || name == "time_tickerinit53") {
            int64_t f = static_cast<int64_t>(static_cast<uint64_t>(U(inp, "freqHi")) << 32 | U(inp, "freqLo"));
            Ticker tk = Ticker::Init(f, name == "time_tickerinit53");
            r = {{"shift", Hi(tk.Shift)}, {"ms", Hf(tk.MsPerTick)}};
        } else if (name == "time_gettimestep") {
            WorldTimeState ws; ws.ReplayPace = F(inp, "pace"); ws.TimeScale = F(inp, "scale"); ws.TimeScaleTimer = F(inp, "timer");
            bool replay = U(inp, "replay") != 0;
            int ticks = replay ? I(inp, "ticks") : I(inp, "globalTicks");
            float wait = 0;
            float dt = ws.GetTimestep(ticks, F(inp, "rte"), replay, U(inp, "nopace") != 0, wait);
            r = {{"f32", Hf(dt)}, {"pace", Hf(ws.ReplayPace)}, {"scale", Hf(ws.TimeScale)}, {"timer", Hf(ws.TimeScaleTimer)}};
        } else if (name == "time_gamespeed") {
            r = {{"f32", Hf(TimeGlobals::ApplyGameSpeed(F(inp, "dt")))}};
        } else if (name == "time_paused") {
            WorldTimeState ws; ws.PauseTime = F(inp, "pause"); ws.PauseFlag = I(inp, "flag");
            ws.UpdateWorldPaused(F(inp, "rte"));
            r = {{"paused", Hi(ws.IsWorldPaused(U(inp, "step") != 0, U(inp, "tweakPause") != 0) ? 1 : 0)}, {"pause", Hf(ws.PauseTime)}};
        } else if (name == "time_advance") {
            TimeGlobals t = LoadGlobals(inp);
            t.AdvanceRealTime(); t.AdvanceWorldTime();
            r = SaveGlobals(t);
        } else if (name == "time_substeps") {
            float nf = 0, h = 0;
            int n = PhysicsSubsteps::Split(F(inp, "dt"), TimingConstants::DefaultMaximumTimeStep, nf, h);
            r = {{"n", Hi(n)}, {"nf", Hf(nf)}, {"h", Hf(h)}};
        } else if (name == "time_moveacc") {
            auto acc = Arr(inp, "acc"), lf = Arr(inp, "lf"), lt = Arr(inp, "lt");
            PhysicsSubsteps::Accumulate(acc.data(), lf.data(), lt.data());
            auto join = [&]() { std::string s; for (size_t i = 0; i < acc.size(); i++) { if (i) s += ","; s += Hf(acc[i]); } return s; };
            std::string mid = join();
            PhysicsSubsteps::Average(acc.data(), F(inp, "nf"));
            r = {{"acc", mid}, {"avg", join()}};
        } else if (name == "time_sequence") {
            EvalSequence(inp, want, got);
            return;
        } else if (EvaluateInputSeq(name, inp, want, got)) {
            return;
        } else if (!EvaluateInput(name, inp, r)) {
            throw std::runtime_error("no port registered for golden '" + name + "'");
        }
        Cmp(o, r, want, got);
    }

private:
    static void EvalSequence(const JVal& inp, std::string& want, std::string& got) {
        SimClock clock;
        clock.Globals = LoadGlobals(inp["init"]);
        want = got = "";
        int k = 0;
        for (const JVal& fr : inp["frames"].a) {
            if (const JVal* ev = fr.Get("ev")) {
                if (ev->Get("scale")) { clock.World.TimeScale = F(*ev, "scale"); clock.World.TimeScaleTimer = F(*ev, "timer"); }
                if (ev->Get("pause")) clock.World.PauseTime = F(*ev, "pause");
                if (ev->Get("pauseFlag")) clock.World.PauseFlag = I(*ev, "pauseFlag");
            }
            SimFrame f = clock.Frame(F(fr, "real"));
            const TimeGlobals& gt = clock.Globals;
            KV r = {
                {"Ticks", Hi(f.Ticks)}, {"Rem", Hf(gt.TimestepRemainder)}, {"RTE", Hf(f.RealTimeElapsed)},
                {"paused", Hi(f.Paused ? 1 : 0)}, {"WorldTimerAtStep", Hi(f.WorldTimerAtStep)},
                {"dt", Hf(f.Timestep)}, {"dtw", Hf(f.WorldTimeElapsed)}, {"n", Hi(f.Substeps)},
                {"nf", Hf(f.SubstepCount)}, {"h", Hf(f.SubstepDt)},
                {"RealTimer", Hi(gt.RealTimer)}, {"RealTimeFrames", Hi(gt.RealTimeFrames)},
                {"RealTimeFramesElapsed", Hi(gt.RealTimeFramesElapsed)}, {"RealTimeElapsedFrame", Hf(gt.RealTimeElapsedFrame)},
                {"WorldTimer", Hi(gt.WorldTimer)}, {"WorldTimeSeconds", Hf(gt.WorldTimeSeconds)},
                {"WorldTimeFrames", Hi(gt.WorldTimeFrames)}, {"WorldTimeElapsedFrame", Hf(gt.WorldTimeElapsedFrame)},
                {"WorldLoopCounter", Hi(gt.WorldLoopCounter)}, {"RealLoopCounter", Hi(gt.RealLoopCounter)},
                {"pause", Hf(clock.World.PauseTime)}, {"pace", Hf(clock.World.ReplayPace)},
                {"scale", Hf(clock.World.TimeScale)}, {"timer", Hf(clock.World.TimeScaleTimer)},
            };
            std::string w, g;
            Cmp(fr["out"], r, w, g, "f" + std::to_string(k) + ".");
            if (w != g) { want = w; got = g; return; }
            k++;
        }
    }

    static bool EvaluateInputSeq(const std::string& name, const JVal& inp, std::string& w, std::string& g) {
        w.clear(); g.clear();
        if (name == "input_joy") {
            const JVal& init = inp["init"];
            const JVal& carJ = init["car"];
            CarInputView car; car.Gear = I(carJ, "gear"); car.NumForwardGears = I(carJ, "ngears"); car.MovementMode = I(carJ, "mode");
            RealDriver d; d.ShiftMode = I(init, "shift"); d.GearCommand = I(init, "gc"); d.CurrentGear = I(init, "cg"); d.Port = 0;
            int k = 0;
            for (const JVal& st : inp["steps"].a) {
                const JVal& s = st["step"];
                if (const JVal* set = s.Get("set")) {
                    int v = I(s, "v");
                    if (set->s == "gear") car.Gear = v;
                    else if (set->s == "ignore") d.IgnorePedals = v != 0;
                    else if (set->s == "shift") d.ShiftMode = v;
                } else {
                    int ev = I(s, "ev"); uint8_t v = static_cast<uint8_t>(I(s, "v"));
                    switch (ev) {
                    case 0x8F: case 0x90: d.JoyHandlerSteer(ev, v); break;
                    case 0x91: case 0x92: case 0x93: d.JoyHandlerThrottle(ev, v, car, Tables()); break;
                    case 0x94: case 0x95: case 0x96: d.JoyHandlerBrake(ev, v); break;
                    case 0x97: case 0x98: d.JoyHandlerReverse(ev, v, car); break;
                    case 0x99: case 0x9A: d.JoyHandlerEBrake(ev, v); break;
                    case 0x9B: case 0x9D: d.JoyHandlerShiftUp(v, car); break;
                    case 0x9C: case 0x9E: d.JoyHandlerShiftDown(v, car); break;
                    case 0x9F: case 0xA0: d.JoyHandlerDisengageClutch(ev, v); break;
                    default: break;
                    }
                }
                KV got = {
                    {"L", B(d.SteerLeft)}, {"R", B(d.SteerRight)}, {"steer", Hf(d.SteerInput)},
                    {"td", B(d.ThrottleDigital)}, {"ta", B(d.ThrottleAnalog)}, {"ta2", B(d.ThrottleAnalog2)}, {"thr", Hf(d.ThrottleInput)},
                    {"bd", B(d.BrakeDigital)}, {"ba", B(d.BrakeAnalog)}, {"ba2", B(d.BrakeAnalog2)}, {"brk", Hf(d.BrakeInput)},
                    {"rd", B(d.ReverseDigital)}, {"ra", B(d.ReverseAnalog)}, {"rev", Hf(d.ReverseInput)},
                    {"ed", B(d.EBrakeDigital)}, {"ea", B(d.EBrakeAnalog)}, {"eb", Hf(d.EBrakeInput)},
                    {"clutch", Hf(d.ManualClutch)}, {"gc", Hi(d.GearCommand)}, {"cg", Hi(d.CurrentGear)},
                };
                if (!Step(st["out"], got, k, w, g)) break;
                k++;
            }
            return true;
        }
        if (name == "input_tbr") {
            const JVal& init = inp["init"];
            InputContext ctx; ctx.TestRPM = F(init, "testRpm"); ctx.Online = U(init, "online") != 0; ctx.RaceCoordinatorState = I(init, "rcs");
            CopyFlagsFrom(init["flags"], ctx.Flags);
            const JVal& carJ = init["car"];
            CarInputView car; car.Gear = I(carJ, "gear"); car.IdleRpm = F(carJ, "idle"); car.NumForwardGears = 6;
            RealDriver d; d.ShiftMode = I(init, "shift"); d.Port = 0;
            int k = 0;
            for (const JVal& fr : inp["frames"].a) {
                const JVal& fi = fr["inp"];
                d.ThrottleInput = F(fi, "thr"); d.BrakeInput = F(fi, "brk"); d.EBrakeInput = F(fi, "eb");
                car.SpeedLong = F(fi, "speed"); car.Car118 = F(fi, "c118"); car.Rpm = F(fi, "rpm"); car.Car430 = F(fi, "c430");
                ctx.WorldLoopCounter = I(fi, "wlc");
                if (fi.Get("ev_ebtimer")) d.EBrakeTimer = F(fi, "ev_ebtimer");
                if (fi.Get("ev_gear")) car.Gear = I(fi, "ev_gear");
                if (fi.Get("ev_launch")) d.DragLaunchTimer = I(fi, "ev_launch");
                d.DoThrottleBrakeReverse(F(fi, "dt"), car, ctx, Tables());
                KV got = {
                    {"thrOut", Hf(d.Throttle)}, {"ebOut", Hf(d.EBrake)}, {"brkOut", Hf(d.Brake)}, {"gc", Hi(d.GearCommand)},
                    {"ebTimer", Hf(d.EBrakeTimer)}, {"launch", Hi(d.DragLaunchTimer)}, {"latch", Hi(d.ReverseLatch ? 1 : 0)},
                    {"car438", Hf(car.ThrottleTelemetry)},
                };
                if (!Step(fr["out"], got, k, w, g)) break;
                k++;
            }
            return true;
        }
        if (name == "input_avg") {
            AverageWindow a(F(inp, "window"), F(inp, "rate"));
            uint8_t capFull = static_cast<uint8_t>(I(inp, "capFull"));
            if (capFull != a.Capacity) { w = "cap=" + B(capFull); g = "cap=" + B(a.Capacity); return true; }
            int k = 0;
            for (const JVal& st : inp["steps"].a) {
                a.Record(F(st, "v"), F(st, "t"));
                KV got = { {"cnt", B(a.Count)}, {"idx", B(a.Index)}, {"sum", Hf(a.Sum)}, {"avg", Hf(a.Average)}, {"old", Hi(a.Oldest)} };
                if (!Step(st["out"], got, k, w, g)) break;
                k++;
            }
            return true;
        }
        if (name == "input_steer") {
            const JVal& init = inp["init"];
            InputContext ctx;
            CopyFlagsFrom(init["flags"], ctx.Flags);
            CarInputView car; car.SteerMaxDeg = F(init, "maxdeg"); car.TopSpeed = F(init, "top"); car.Gear = 3;
            RealDriver d; d.Port = 0;
            PlayerSteering& ps = d.Steering;
            int k = 0;
            for (const JVal& fr : inp["frames"].a) {
                const JVal& fi = fr["inp"];
                ctx.WorldTimer = I(fi, "wt");
                car.SpeedLong = F(fi, "speed"); car.Yaw = F(fi, "yaw"); car.Car128 = F(fi, "c128");
                auto vel = Arr(fi, "vel"); car.VelX = vel[0]; car.VelY = vel[1]; car.VelZ = vel[2];
                car.LastCollisionTime = I(fi, "ts");
                d.Throttle = F(fi, "thr"); d.Brake = F(fi, "brk"); d.EBrake = F(fi, "eb");
                uint16_t ang = ps.DoSteering(F(fi, "inp"), F(fi, "dt"), car, d, ctx, Tables());
                KV got = {
                    {"ret", H(ang)}, {"angle", H(ps.Angle)}, {"limit", Hf(ps.Limit)}, {"last", Hf(ps.LastCurve)},
                    {"flag", Hi(ps.CounterSteerActive ? 1 : 0)},
                    {"a1cnt", B(ps.CurveAverage.Count)}, {"a1idx", B(ps.CurveAverage.Index)}, {"a1sum", Hf(ps.CurveAverage.Sum)},
                    {"a1avg", Hf(ps.CurveAverage.Average)}, {"a1old", Hi(ps.CurveAverage.Oldest)},
                    {"a2cnt", B(ps.RateAverage.Count)}, {"a2idx", B(ps.RateAverage.Index)}, {"a2sum", Hf(ps.RateAverage.Sum)},
                    {"a2avg", Hf(ps.RateAverage.Average)}, {"a2old", Hi(ps.RateAverage.Oldest)},
                };
                if (!Step(fr["out"], got, k, w, g)) break;
                k++;
            }
            return true;
        }
        return false;
    }

    static bool EvaluateInput(const std::string& name, const JVal& inp, KV& r) {
        if (name == "input_delegate") {
            CarDriver d;
            d.Throttle = F(inp, "thr"); d.Brake = F(inp, "brk"); d.EBrake = F(inp, "eb");
            d.SteerAngle = static_cast<uint16_t>(U(inp, "steer"));
            d.GearCommand = I(inp, "gc"); d.CurrentGear = I(inp, "cg");
            PhysicsEnvironment& env2 = PEnv();
            env2.World = World();
            env2.World.TestRPM = F(inp, "testRpm");
            CopyFlagsFrom(inp["flags"], env2.World.Flags);
            Car car;
            car.State.Gear = I(inp, "gear"); car.State.Rpm = F(inp, "rpm"); car.State.WheelsOnGround = I(inp, "won");
            car.State.NosStartTime = 0;
            Aerodynamics aero;
            PhysicsMover mv(&env2);
            mv.Car = &car; mv.State = &car.State; mv.Driver = &d; mv.Aero = &aero;
            WheelInfo wis[4];
            TwoWheelAckermanSteering steer(wis);
            mv.Steering = &steer;
            Engine eng(env2.Engine); eng.Omega = F(inp, "om");
            mv.Engine = &eng;
            DriveTrainInfo dti; dti.NumGears = I(inp, "ng");
            DriveTrain dt(env2.Engine);
            dt.Gear = I(inp, "dtg"); dt.LastRequest = I(inp, "st54"); dt.AutoTimer = X87::Bits(static_cast<uint32_t>(I(inp, "f50")));
            dt.Info = &dti;
            mv.DriveTrain = &dt;
            // sub_5AA960 is stubbed in the emulator to return its first argument (gear command)
            mv.DelegateDriverInput([](int gc, float) { return gc; });
            r = {
                {"thrOut", Hf(d.Throttle)}, {"brkOut", Hf(d.Brake)}, {"ebOut", Hf(d.EBrake)}, {"gcOut", Hi(d.GearCommand)},
                {"cgOut", Hi(d.CurrentGear)}, {"engThr", Hf(eng.Throttle)}, {"engNos", Hi(eng.NosLevel)},
                {"st54Out", Hi(dt.LastRequest)}, {"f50Out", H(X87::Bits(dt.AutoTimer))},
                {"aeroThr", Hf(aero.Throttle)}, {"aeroGnd", Hf(aero.GroundFrac)},
            };
            return true;
        }
        if (name == "input_graph") {
            auto pts = Arr(inp, "pts");
            int n = I(inp, "n");
            float arg = F(inp, "arg");
            float v1, v2 = X87::Bits(0xDEADBEEFu);
            if (n > 0) {
                Graph gr(pts, n);
                v1 = gr.GetValue(arg).Store();
                gr.GetValueTo(v2, arg);
            } else v1 = pts[1];   // n == 0: Graph::GetValue still returns pts[0].y; sub_40C840 writes nothing
            r = {{"graph", Hf(v1)}, {"graphTo", Hf(v2)}};
            return true;
        }
        return false;
    }
};

Register reg(new SuiteTiming);

} // namespace
