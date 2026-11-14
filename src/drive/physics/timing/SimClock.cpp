#include "drive/physics/timing/SimClock.h"

namespace bb {

using namespace TimingFpu;

// ---------------------------------------------------------------------------------------- TimeGlobals

void TimeGlobals::SetFPSLimiter(float fps) { LimitMinimumVideoTimeElapsed = D(TimingConstants::K_One, fps); }

void TimeGlobals::ResetWorldTime() {
    WorldTime = 1; WorldTimeFrames = 1;
    WorldTimer = 0xFA0;                 // 4000 = one second
    WorldTimeSeconds = TimingConstants::K_One;
    WorldTimeFramesElapsed = 0; WorldTimeElapsed = 0.0f; WorldTimeElapsedFrame = 0.0f; WorldLoopCounter = 0;
}

float TimeGlobals::PrepareRealTimestep(float realSec) {
    float waited = 0.0f;
    float wait = S(LimitMinimumVideoTimeElapsed, realSec);           // 0x57EB0A fld Limit ; fsub arg ; fst var_8
    if (!Jnz41(wait, TimingConstants::K_Zero)                         // 0x57EB1F wait > 0
        && !Jp5(wait, LimitMinimumVideoTimeElapsed))                  // 0x57EB41 wait < Limit
    {
        waited = wait;                                                // busy wait wait*1000 ms (0x57EB64 loop)
        realSec = A(wait, realSec);                                   // 0x57EB7E fld var_8 ; fadd arg ; fstp arg
    }
    float acc = A(TimestepRemainder, realSec);                        // 0x57EB8F fld flt_865148 ; fadd arg
    float maxDt;
    if (CurrentVideoMode == 0) {
        maxDt = M(MaxTicksPerTimestep, TimingConstants::K_0_02);      // 0x57EBBD
        if (Jnz41(acc, TimingConstants::K_0_02)) acc = TimingConstants::K_0_02;   // acc <= 0.02 -> 0.02
    } else {
        maxDt = M(MaxTicksPerTimestep, TimingConstants::K_1_60);      // 0x57EBDE
        if (Jnz41(acc, LimitMinimumVideoTimeElapsed)) acc = LimitMinimumVideoTimeElapsed;
    }
    if (!Jp5(maxDt, acc)) acc = maxDt;                                // 0x57EBFD fld var_8 ; fcomp st1 : max >= acc keeps acc
    int ticks = Ftol(A(M(TimingConstants::K_1800, acc), TimingConstants::K_Half));        // 0x57EC10
    int ticks4000 = Ftol(A(FildMul(ticks, TimingConstants::K_4000_1800), TimingConstants::K_Half)); // 0x57EC29
    TimestepRemainder = S(acc, FildMul(ticks4000, TimingConstants::K_1_4000));          // 0x57EC42 fsubr st, st1
    TimestepTicks = ticks;                                            // 0x57EC8D
    RealTimeElapsed = FildMul(ticks, TimingConstants::K_1_1800);      // 0x57EC88
    return waited;
}

void TimeGlobals::PrepareRealTimestepReplay(int joylogTicks) {
    TimestepTicks = joylogTicks;
    RealTimeElapsed = FildMul(joylogTicks, TimingConstants::K_1_1800);
}

void TimeGlobals::AdvanceRealTime() {
    float s = A(RealTimeElapsed, RealTimeElapsedFrame);              // fld RTE ; fadd RTEF
    int frames = Ftol(M(TimingConstants::K_60, s));                   // fld 60 ; fmul st, st1
    RealTimeElapsedFrame = S(s, FildMul(frames, TimingConstants::K_1_60)); // fsubr st, st1
    if (frames == 0) frames = 1;
    RealTimeFrames += frames;
    RealTimeFramesElapsed = frames;
    gCardiogram = RealTimeFrames;
    RealTimer += Ftol(A(M(RealTimeElapsed, TimingConstants::K_4000), TimingConstants::K_Half));
    RealLoopCounter++;
}

void TimeGlobals::AdvanceWorldTime() {
    NeedToPrepareWorldTimestep = 1;
    if (Jnp44(WorldTimeElapsed, TimingConstants::K_Zero)) return;     // test ah,44h ; jnp : == 0
    float s = A(WorldTimeElapsedFrame, WorldTimeElapsed);
    int frames = Ftol(M(TimingConstants::K_60, s));
    WorldTimeFrames += frames;
    WorldTimeFramesElapsed = frames;
    WorldTime = WorldTimeFrames;
    WorldTimeElapsedFrame = S(s, FildMul(frames, TimingConstants::K_1_60));
    WorldTimer += Ftol(A(M(WorldTimeElapsed, TimingConstants::K_4000), TimingConstants::K_Half));
    WorldLoopCounter++;
    WorldTimeElapsed = 0.0f;
    WorldTimeSeconds = FildMul(WorldTimer, TimingConstants::K_1_4000);
}

// ---------------------------------------------------------------------------------------- Ticker

Ticker Ticker::Init(int64_t qpcFrequency, bool pc53) {
    Ticker t;
    // fild qword freq (exact) ; fdivr 1000.0 ; fst flt_828028 (value stays on the stack)
    double st = pc53 ? 1000.0 / static_cast<double>(qpcFrequency)     // rounded to 53 bits
                     : static_cast<double>(Div24(1000.0, qpcFrequency)); // rounded once to 24 bits
    t.MsPerTick = static_cast<float>(st);                             // fst dword
    // fmul 2^31 ; loop: while (x < 60000) { x += x; shift++ }  (doubling is exact)
    double x = st * 2147483648.0;
    int shift = 0;
    if (x < 60000.0) {
        do { x = x + x; shift++; } while (x < 60000.0);
    }
    t.Shift = shift;
    return t;
}

// a / b correctly rounded to a 24-bit significand (PC24 fdiv by an exact 64-bit integer).
float Ticker::Div24(double a, int64_t b) {
    double q = a / static_cast<double>(b);                            // rounded to 53 bits
    float r = static_cast<float>(q);                                  // may be a double rounding
    if (static_cast<double>(r) == q) return r;
    uint32_t rb = X87::Bits(r);
    // true quotient lies strictly between r and its neighbour on q's side
    float other = X87::Bits((static_cast<double>(r) < q) == (r > 0) ? rb + 1 : rb - 1);
    float lo = static_cast<double>(r) < q ? r : other, hi = static_cast<double>(r) < q ? other : r;
    double mid = (static_cast<double>(lo) + static_cast<double>(hi)) * 0.5;   // exact (25 significant bits)
    double prod = mid * static_cast<double>(b);
    double err = TwoProdErr(mid, static_cast<double>(b), prod);      // mid*b = prod + err exactly
    double diff = (prod - a) + err;                                   // sign of mid*b - a
    if (diff > 0) return lo;
    if (diff < 0) return hi;
    return (X87::Bits(lo) & 1u) == 0 ? lo : hi;                       // tie: even significand
}

double Ticker::TwoProdErr(double a, double b, double p) {
    const double split = 134217729.0;
    volatile double ca = split * a; double ah = ca - (ca - a), al = a - ah;
    volatile double cb = split * b; double bh = cb - (cb - b), bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

float Ticker::DifferenceMs(uint32_t a, uint32_t b) const {
    int d = static_cast<int>(b - a);
    if (d < 0) d = 0;
    float scaled = FildMul(d, static_cast<float>(1 << Shift));       // (1<<shift) is a power of two: exact float
    return M(scaled, MsPerTick);
}

// ---------------------------------------------------------------------------------------- WorldTimeState

void WorldTimeState::SetTimeScale(float scale, float duration, std::function<void(int)> cb, int arg) {
    TimeScale = scale; TimeScaleTimer = duration; Callback = std::move(cb); CallbackArg = arg;
}

void WorldTimeState::UpdateWorldPaused(float realTimeElapsed) {
    if (Jnz41(PauseTime, TimingConstants::K_Zero)) return;           // <= 0
    PauseTime = S(PauseTime, realTimeElapsed);
    if (Jp41(PauseTime, TimingConstants::K_Zero)) return;            // still > 0 (or NaN)
    PauseTime = 0.0f;
    FireCallback();
}

void WorldTimeState::FireCallback() {
    auto cb = Callback;
    if (!cb) return;
    cb(CallbackArg);
    Callback = nullptr; CallbackArg = 0;
}

bool WorldTimeState::IsWorldPaused(bool tweakerStepWorld, bool tweakerPauseWorld) const {
    if (tweakerStepWorld) return false;
    if (Jz41(PauseTime, TimingConstants::K_Zero)) return true;       // PauseTime > 0
    if (tweakerPauseWorld) return true;
    return PauseFlag != 0;
}

float WorldTimeState::GetTimestep(int ticks, float realTimeElapsed, bool replayPlaying, bool replayNoPace, float& replayPaceWait) {
    replayPaceWait = 0.0f;
    float dt = FildMul(ticks, TimingConstants::K_1_1800);            // sub_576CE0 ; fstp var_4
    if (replayPlaying && !replayNoPace) {
        float x = A(S(dt, realTimeElapsed), ReplayPace);              // fld dt ; fsub RTE ; fadd [+104]
        float st;
        if (Jnz41(x, TimingConstants::K_Zero)) st = TimingConstants::K_Zero;   // <= 0 -> 0
        else st = Jp5(TimingConstants::K_ReplayPaceMax, x) ? x : TimingConstants::K_ReplayPaceMax;
        ReplayPace = st;
        if (!Jnz41(st, TimingConstants::K_Zero)) replayPaceWait = st;   // spin while elapsed*0.001 < st
    }
    if (!Jnz41(TimeScaleTimer, TimingConstants::K_Zero)) {           // timer > 0
        TimeScaleTimer = S(TimeScaleTimer, dt);
        if (!Jp41(TimeScaleTimer, TimingConstants::K_Zero)) {        // <= 0: slow motion over
            TimeScaleTimer = 0.0f;
            TimeScale = TimingConstants::K_One;
            FireCallback();
        }
    }
    return M(dt, TimeScale);
}

// ---------------------------------------------------------------------------------------- PhysicsSubsteps

namespace PhysicsSubsteps {

int Split(float dt, float maximumTimeStep, float& nf, float& h) {
    int n = Ftol(D(dt, maximumTimeStep)) + 1;                         // fld dt ; fdiv MaximumTimeStep ; __ftol2 ; inc
    nf = static_cast<float>(n);                                       // fild ; fstp dword (round to nearest)
    h = D(dt, nf);                                                    // fld dt ; fdiv var_8
    return n;
}

void Accumulate(float* acc, const float* lastForce, const float* lastTorque) {
    acc[0] = A(acc[0], lastForce[0]);
    acc[1] = A(lastForce[1], acc[1]);
    acc[2] = A(lastForce[2], acc[2]);
    acc[3] = A(lastTorque[0], acc[3]);
    acc[4] = A(lastTorque[1], acc[4]);
    acc[5] = A(lastTorque[2], acc[5]);
}

void Average(float* acc, float nf) {
    float inv = D(TimingConstants::K_One, nf);
    for (int i = 0; i < 6; i++) acc[i] = M(inv, acc[i]);
}

} // namespace PhysicsSubsteps

// ---------------------------------------------------------------------------------------- SimClock

void SimClock::BeginWorld() {
    Globals.ResetWorldTime();
    Globals.SetFPSLimiter(TimingConstants::FpsLimiterInGame);
    World.PauseTime = 0.0f; World.PauseFlag = 0; World.ReplayPace = 0.0f; World.TimeScale = TimingConstants::K_One;
    World.TimeScaleTimer = 0.0f; World.Callback = nullptr; World.CallbackArg = 0;
}

SimFrame SimClock::Frame(float realSec) {
    SimFrame f;
    TimeGlobals& g = Globals;
    f.BusyWait = g.PrepareRealTimestep(realSec);
    f.Ticks = g.TimestepTicks;
    f.RealTimeElapsed = g.RealTimeElapsed;
    World.UpdateWorldPaused(g.RealTimeElapsed);
    f.Paused = World.IsWorldPaused(TweakerStepWorld, TweakerPauseWorld) || RaceCoordinatorState == 1;
    f.WorldTimerAtStep = g.WorldTimer;
    if (f.Paused || ReplayFrozen) {
        g.WorldTimeElapsed = TimeGlobals::ApplyGameSpeed(0.0f);
    } else {
        f.Timestep = World.GetTimestep(g.TimestepTicks, g.RealTimeElapsed, false, false, f.ReplayPaceWait);
        f.WorldTimeElapsed = TimeGlobals::ApplyGameSpeed(f.Timestep);
        g.WorldTimeElapsed = f.WorldTimeElapsed;
        g.NeedToPrepareWorldTimestep = 0;
        // World::DoTimestep(dt) -> ... -> PhysicsMover::Move(dt) for every physics car (same dt)
        f.Substeps = PhysicsSubsteps::Split(f.WorldTimeElapsed, MaximumTimeStep, f.SubstepCount, f.SubstepDt);
    }
    g.AdvanceRealTime();
    g.AdvanceWorldTime();
    return f;
}

} // namespace bb
