// Port of NFSU2Unity Core/Audio/Engine/EngineAudioController.cs.

#include "drive/audio/engine/EngineAudioController.h"

#include <algorithm>
#include <cmath>

namespace bb {

using namespace EngineAudioMath;

EngineAudioController::EngineAudioController(const EngineSelection& sel, float idleRpm, float maxRpm,
                                             const int* cosTable, const EngineAudioOptions& opt)
    : Sel(sel), Opt(opt), IdleRpm(idleRpm), MaxRpm(maxRpm), Tel(idleRpm, maxRpm), Shift(this), Accel(this),
      Eng(this, &rnd_), Hm(this, &rnd_), rnd_(opt.RandomSeed)
{
    if (cosTable != nullptr) CosTable.assign(cosTable, cosTable + 513);
    else CosTable = SyntheticCosTable();
    Frame.D[0] = ClampI(sel.Engine->AemsId, 0, 60);
    Frame.D[12] = 1;                       // (FEDatabase != 4) clamp 0..2, set at creation
}

const EngineAudioFrame& EngineAudioController::Tick(const EngineAudioInput& input, double dt)
{
    Time += dt;
    Ticks++;
    if (Opt.TelemetryFirst) {
        Tel.Update(input);
        if (Opt.Shifting) Shift.Update(dt);
        if (Opt.AccelTrans) Accel.Update(dt, input);
        Eng.Update(dt, input);
        Hm.Update(dt, input);
    } else {
        // CreateSFXCtrls order: Engine, HybridMotor, Tunnel, 3DCarPos, Shifting, AccelTrans, Physics
        Eng.Update(dt, input);
        Hm.Update(dt, input);
        if (Opt.Shifting) Shift.Update(dt);
        if (Opt.AccelTrans) Accel.Update(dt, input);
        Tel.Update(input);
    }
    SetEngineParams();
    return Frame;
}

void EngineAudioController::SetEngineParams()
{
    EngineAudioFrame& f = Frame;
    int vol = Opt.MixQ15;
    double ginRpmReq = Hm.GinRpm;
    double ratio = 1.0, pitchK = 1.0;
    double rpmG = ginRpmReq;
    if (Gin != nullptr) {
        if (rpmG <= Gin->MinRpm()) {
            ratio = rpmG / Gin->MinRpm();
            rpmG = Gin->MinRpm();
        }
        Gin->SetTargetRpm(rpmG);
        if (Dcl != nullptr) Dcl->SetTargetRpm(rpmG);
        if (Opt.HeardRpmFeedback) {
            // The game reads the player's last rendered grain; before the stream has rendered its first
            // block (or with audio off) that is 0, so keep the requested rpm until then (port choice).
            double heard = Gin->HeardRpm();
            if (heard > 0) ginRpmReq = heard * ratio;
        }
    }
    f.GinTargetRpm = rpmG;
    f.GinPitch = Ftol(ratio * pitchK * 4096);
    f.GinLpf = Ftol(Hm.CLpf);
    f.GinVol127 = ClampI(((Hm.VolGin * vol) >> 15) >> 8, 0, 127);
    f.DclVol127 = ClampI(((Hm.VolDcl * vol) >> 15) >> 8, 0, 127);

    int* d = f.D;
    d[1] = ClampI(Ftol(ginRpmReq), 0, 10000);
    d[3] = ClampI(Ftol(Eng.Torque * 10.24), 0, 0x400);
    d[4] = d[5] = ClampI(((Hm.VolAems * vol) >> 15) - 0x7FFF, -0x7FFF, 0);
    d[13] = 0;                             // clamp(dword_82B880 - 0x7FFF, -0x7FFF, 0): option volume, full
    d[14] = 0;                             // dword_82B070 * vol >> 15: send level (reverb), not ported
    d[15] = 0x4000;                        // azimuth: in front (the player car is not spatialised)
    d[16] = ClampI(-16383 - Ftol(pitchK * -16383), -0x3FFF, 0x3FFF);
    d[17] = 150;
    d[21] = ClampI(Eng.DistLpf, 0, 25000);
    d[22] = 0;
    d[25] = ClampI((Hm.VolRed * vol) >> 15, 0, 0x7FFF);

    f.ShiftClunk = Shift.ClunkEvent;
    f.ShiftClunkVol = f.ShiftClunk != 0
                          ? ShiftClunkVolume(f.ShiftClunk == 1 ? static_cast<int>(Sel.Shift->P[1])
                                                               : static_cast<int>(Sel.Shift->P[15]))
                          : 0;
    Shift.ClunkEvent = 0;
}

int EngineAudioController::ShiftClunkVolume(int baseVol) const
{
    double x = ClampD((static_cast<int>(Tel.RpmAudio) - 1500) / (8000 - 1500.0), 0, 1);
    double y = 0.1 + 0.9 * x;
    int idx = Ftol(y * -512 + 512);
    return ClampI(((CosTable[static_cast<size_t>(ClampI(idx, 0, 512))] * baseVol) >> 15) * Opt.MixQ15 >> 15, 0,
                  0x7FFF);
}

// ------------------------------------------------------------------------------------------- controllers

void EngineAudioController::Telemetry::Update(const EngineAudioInput& i)
{
    PrevGear = Gear;
    Gear = i.Gear;
    PrevThrRaw = ThrRaw;
    ThrRaw = static_cast<float>(i.Throttle * 100.0);
    ThrOn = ThrRaw > 30;
    Thr = static_cast<float>(ClampD(ThrRaw, Thr - 30.0, Thr + 30.0));
    RpmAudio = static_cast<float>((i.Rpm - static_cast<double>(Idle)) / (static_cast<double>(Max) - Idle) * 9000 + 1000);
    Mph = static_cast<float>(std::fabs(i.Speed) * 2.237);
}

void EngineAudioController::ShiftingCtl::Update(double dt)
{
    Telemetry& t = c_->Tel;
    EngineCtl& e = c_->Eng;
    StartEvent = EngageEvent = false;
    if (t.Gear > t.PrevGear && t.RpmAudio >= 3000) StartUp();
    else if (t.Gear < t.PrevGear && t.Gear >= 2) StartDown();

    if (eventTime_ >= 0 && c_->Time > eventTime_) {
        ClunkEvent = eventType_;
        eventTime_ = -1;
    }
    Rpm.Update(dt); Torque.Update(dt); Vol.Update(dt); LfoCAmp.Update(dt); LfoAAmp.Update(dt); Target.Update(dt);
    double phys = t.RpmAudio;
    switch (State) {
    case 1:
        if (Rpm.Done) {
            State = 2;
            Rpm.Set(ClampD(phys + P(5), 1000, 10000), phys, static_cast<int>(P(6)));
            Torque.Set(t.Thr, t.Thr, 0);
            Vol.Set(P(8), 0, static_cast<int>(P(7)));
            StartLfo(t.Gear);
            EngageEvent = true;
        }
        break;
    case 2:
        Rpm.B = static_cast<float>(phys);
        if (Rpm.Done) State = 3;
        break;
    case 3:
        Rpm.B = static_cast<float>(phys);
        if (Rpm.Done) Rpm.Cur = static_cast<float>(phys);
        if (LfoCAmp.Done && LfoAAmp.Done) Clear();
        break;
    case 4:
        if (Rpm.Done) {
            State = 5;
            double blip = P(19) * (t.Gear < 3 ? 0.7 : 1.0);
            Rpm.Set(e.Rpm, ClampD(phys + blip, 1000, 10000), static_cast<int>(P(20)));
            Torque.Set(Torque.Cur, 100, 215);
        }
        break;
    case 5:
        if (Rpm.Done) {
            State = 6;
            double drop = P(21) * (t.Gear < 3 && c_->Sel.DrivetrainLevel <= 1 ? 0.7 : 1.0);
            Rpm.Set(e.Rpm, e.Rpm - drop, static_cast<int>(P(22)));
            Torque.Set(100, 0, 215);
        }
        break;
    case 6:
        if (Rpm.Done) {
            State = 7;
            Rpm.Set(e.Rpm, phys, Ftol(ClampD(std::fabs(e.Rpm - phys) * P(23), 0, 800)));
            Torque.Set(0, t.Thr, 60);
        }
        break;
    case 7:
        Rpm.B = static_cast<float>(phys);
        if (Rpm.Done) Clear();
        break;
    default:
        break;
    }
}

void EngineAudioController::ShiftingCtl::StartUp()
{
    EngineCtl& e = c_->Eng;
    State = 1;
    e.Rpm = e.RpmPrev;                                     // undo the last engine frame
    double a8 = e.Rpm;
    RpmAtShift = static_cast<float>(a8);
    Torque.Set(c_->Tel.Thr, 0, 100);
    Rpm.Set(a8, a8 - P(3), static_cast<int>(P(4)));
    Vol.Reset(0);
    LfoAAmp.Reset(0);
    LfoCAmp.Reset(0);
    Target.Set(a8, a8 - 1200, static_cast<int>(P(4) + P(6)));
    eventTime_ = c_->Time + P(2);
    eventType_ = 1;
    StartEvent = true;
}

void EngineAudioController::ShiftingCtl::StartDown()
{
    EngineCtl& e = c_->Eng;
    State = 4;
    double a8 = e.Rpm;
    Torque.Set(c_->Tel.Thr, 0, 50);
    Rpm.Set(a8, ClampD(a8 - P(17), 1000, 10000), static_cast<int>(P(18)));
    Vol.Reset(0);
    LfoAAmp.Reset(0);
    LfoCAmp.Reset(0);
    eventTime_ = c_->Time + P(16);
    eventType_ = 2;
}

void EngineAudioController::ShiftingCtl::StartLfo(int gear)
{
    if (gear < 5) {
        double k = gear == 3 ? 0.85 : gear == 4 ? 0.7 : 1.0;
        LfoCAmp.Set(P(12) * k, 0, static_cast<int>(P(14)));
        LfoCPeriod = P(13);
        LfoAAmp.Set(P(9), 0, static_cast<int>(P(11)));
        LfoAPeriod = P(10);
    } else {
        LfoCAmp.Reset(0);
        LfoAAmp.Reset(0);
    }
}

void EngineAudioController::ShiftingCtl::Clear()
{
    State = 0;
    Vol.Reset(0);
    LfoAAmp.Reset(0);
    LfoCAmp.Reset(0);
}

void EngineAudioController::AccelTransCtl::Update(double dt, const EngineAudioInput& input)
{
    Telemetry& t = c_->Tel;
    EngineCtl& e = c_->Eng;
    const AccelFromIdleRow* row = c_->Sel.AccelFromIdle;
    bool rising = t.ThrOn && !prevOn_, falling = !t.ThrOn && prevOn_;
    prevOn_ = t.ThrOn;
    double dThr = t.ThrRaw - t.PrevThrRaw;
    bool shifting = c_->Shift.State != 0 || input.PhysicsShifting;
    LiftEvent = false;
    if (rising && State == 0 && !shifting && dThr > 30) {
        if (t.Mph <= 15 && t.Gear == 2 && t.RpmAudio <= 1500 && row != nullptr) {
            State = 2;                                     // FromIdle 0x45CD30
            Rpm.Set(e.Rpm, e.Rpm + row->RpmUp, row->MsUp);
            Ramp90.Set(0, row->K, row->MsUp);
            Torque.Reset(t.Thr);
            msRelease_ = row->MsRelease;
            msDecay_ = row->MsDecay;
        } else if (t.Gear >= 3 && e.Rpm >= 3000 && c_->Time - lastStab_ >= 2.0 &&
                   (input.OptimalShiftUpRpm <= 0 || e.Rpm <= input.OptimalShiftUpRpm - 250)) {
            State = 1;                                     // Stab 0x470030
            Rpm.Set(e.Rpm, e.Rpm + 1000, 500);
            Ramp90.Reset(0.8);
            Torque.Reset(100);
            lastStab_ = c_->Time;
            if (row != nullptr) {
                msRelease_ = row->MsRelease;
                msDecay_ = row->MsDecay;
            }
        }
    }
    if (falling && State == 0 && !shifting && e.Rpm >= 6000 && t.Gear >= 3) LiftEvent = true;

    Rpm.Update(dt); Ramp90.Update(dt); Torque.Update(dt);
    if (State != 0 && State != 4 && !t.ThrOn) {
        State = 4;
        Rpm.Set(e.Rpm, t.RpmAudio, msRelease_);
        Ramp90.Set(Ramp90.Cur, 0, msRelease_);
    } else if (State == 2 && Rpm.Done) {
        State = 3;
        Rpm.Set(Rpm.Cur, t.RpmAudio, msDecay_);
    } else if ((State == 1 || State == 3 || State == 4) && Rpm.Done) {
        State = 0;
    }
}

void EngineAudioController::EngineCtl::Update(double dt, const EngineAudioInput& input)
{
    Telemetry& t = c_->Tel;
    ShiftingCtl& s = c_->Shift;
    AccelTransCtl& a = c_->Accel;
    RedlineEnable = c_->Opt.Human && a.State == 0 && Rpm > 2500;      // 0x46F9D0
    UpdateLfo(dt);
    RandomBump(dt);

    // UpdateRPM 0x46F7D0
    double src = s.State != 0 ? s.Rpm.Cur : a.State != 0 ? a.Rpm.Cur : t.RpmAudio;
    if (RedlineEnable && s.State == 0) src = ClampD(src, Rpm - 60.0, Rpm + 999.0);
    RpmPrev = Rpm;
    Rpm = static_cast<float>(src + LfoC + Env + LfoC);              // the asm adds LFO C twice
    double tgt;
    if (s.State == 1 || s.State == 2) tgt = s.Target.Cur;
    else if (a.State == 1) tgt = t.RpmAudio;
    else if (!Redlining) tgt = src;
    else {
        if (bounceUp_) {
            bounce_ = static_cast<float>(Slew(bounce_, 200, 50));
            if (bounce_ == 200) bounceUp_ = false;
        } else {
            bounce_ = static_cast<float>(Slew(bounce_, 0, 50));
            if (bounce_ == 0) bounceUp_ = true;
        }
        tgt = src - bounce_;
    }
    Avg2.Record(tgt);
    RpmReal = static_cast<float>((Avg2.Mean - 1000.0) / 9000.0 *
                                     (static_cast<double>(c_->MaxRpm) - c_->IdleRpm) + c_->IdleRpm);

    // UpdateTorque 0x45C710
    if (s.State != 0) Avg3.Flush(s.Torque.Cur);
    else if (a.State != 0) Avg3.Flush(a.Torque.Cur);
    else Avg3.Record(t.Thr);
    Torque = static_cast<float>(Avg3.Mean + static_cast<double>(LfoB));

    // UpdateVolume 0x45C6C0
    Vol = 0x7FFF;
    if (s.State != 0) Vol = 0x7FFF - Ftol(s.Vol.Cur * -32767.0);
    Vol += Ftol(LfoA);

    DistanceLpf(input.ListenerDistance);

    // UpdateRedlining 0x46F610
    if (c_->Opt.Human) {
        if (!Redlining) {
            if (s.State == 0 && Rpm > 9800) {
                Redlining = true;
                int k = t.Gear == 0 ? 5 : 1;
                MainGain.Set(MainGain.Cur, 0.15, Ftol(450 * MainGain.Cur * k));
                RedGain.Set(RedGain.Cur, 0.85, Ftol((1 - RedGain.Cur) * 120 * k));
                bounceUp_ = true;
                bounce_ = 0;
            }
        } else if (Rpm < 9800 || s.State != 0) {
            Redlining = false;
            MainGain.Set(MainGain.Cur, 1.0, Ftol((1 - MainGain.Cur) * 50));
            RedGain.Set(RedGain.Cur, 0.0, Ftol(RedGain.Cur * 50));
        }
        MainGain.Update(dt);
        RedGain.Update(dt);
    }
}

void EngineAudioController::EngineCtl::DistanceLpf(double distM)
{
    double k = c_->Opt.Human && c_->Opt.RaceType67 ? 90.0 : 22.0;
    double x = ClampD((Ftol(distM) - 6.5) / k, 0.0, 1.0);
    int y = MixerCurve(c_->CosTable.data(), 4, Ftol(x * 32766));
    int lin = y * y >> 15;
    if (lin < 50) lin = 0;
    DistLpf = ClampI(Ftol(lin / 32767.0 * (24000 - 725) + 725), 0, 0x7FFF);
}

void EngineAudioController::EngineCtl::UpdateLfo(double dt)
{
    ShiftingCtl& s = c_->Shift;
    if (s.State == 0) {
        LfoA = LfoB = LfoC = 0;
        phaseA_ = phaseB_ = phaseC_ = 0x4097;
        return;
    }
    LfoA = Lfo(phaseA_, s.LfoAAmp.Cur, s.LfoAPeriod, dt);
    LfoB = 0;
    LfoC = Lfo(phaseC_, s.LfoCAmp.Cur, s.LfoCPeriod, dt);
}

float EngineAudioController::EngineCtl::Lfo(int& phase, double amp, double periodMs, double dt)
{
    double per = ClampD(periodMs, 1, 10000);
    phase -= Ftol(dt / (per * 0.001) * -65535.0);
    phase %= 0xFFFF;
    if (phase < 0) phase += 0xFFFF;
    return static_cast<float>(BSin(phase) * amp);
}

void EngineAudioController::EngineCtl::RandomBump(double dt)
{
    if (bumpPhase_ == 1) {
        Env = bumpUp_.Update(dt);
        if (bumpUp_.Done) {
            bumpPhase_ = 2;
            bumpDown_.Set(Env, 0, bumpMs_);
        }
    } else if (bumpPhase_ == 2) {
        Env = bumpDown_.Update(dt);
        if (bumpDown_.Done) {
            bumpPhase_ = 0;
            Env = 0;
        }
    }
    if (BumpRequest && c_->Opt.RandomBump) {
        bumpMs_ = rnd_->Next(100) + 25;
        double amp = rnd_->Next(100) + 25.0;
        bumpUp_.Set(0, amp, bumpMs_);
        bumpPhase_ = 1;
    }
    BumpRequest = false;
}

EngineAudioController::HybridMotorCtl::HybridMotorCtl(EngineAudioController* c, MsvcRand* rnd) : c_(c), rnd_(rnd)
{
    GinRpm = c->Sel.Engine->Idle;
    const DualGinRow* dg = c->Sel.Dual;
    if (dg != nullptr) {
        double lo = dg->Lo, hi = dg->Hi, r = hi - lo;
        dclWindow_ = AudioGraph{{0, 0}, {lo, 0}, {lo + r * dg->F6, 1}, {hi - r * dg->F5, 1}, {hi, 0}};
    }
}

void EngineAudioController::HybridMotorCtl::Update(double dt, const EngineAudioInput& input)
{
    (void)dt;
    Telemetry& t = c_->Tel;
    EngineCtl& e = c_->Eng;
    ShiftingCtl& s = c_->Shift;
    AccelTransCtl& a = c_->Accel;
    const EngineSoundRow& ed = *c_->Sel.Engine;

    // UpdateDeltaRPM 0x45D0A0
    double dPhys = static_cast<double>(t.RpmAudio) - prevAudio_, dEng = static_cast<double>(e.Rpm) - e.RpmPrev;
    prevAudio_ = t.RpmAudio;
    if (a.State == 1) Avg4.Flush(dPhys);
    else if (a.State != 0) Avg4.Record(dEng);
    else if (s.State == 5) Avg4.Flush(dEng);
    else Avg4.Record(dPhys);
    D = Avg4.Mean;

    double r = ClampD(e.Rpm, 1000, 10000);
    GinRpm = static_cast<float>((r - 1000) / 9000 * (static_cast<double>(ed.GinMax) - ed.Idle) + ed.Idle);
    bool shifting = input.PhysicsShifting || s.State != 0;
    if (c_->Sel.Dual == nullptr) SingleMix(shifting);
    else DualMix(shifting);

    // UpdateVolumeRedlining 0x45D190
    VolRed = Ftol(e.RedGain.Cur * static_cast<double>(VolRed));
    VolAems = Ftol(VolAems * static_cast<double>(e.MainGain.Cur));
    VolGin = Ftol(VolGin * static_cast<double>(e.MainGain.Cur));
    VolDcl = Ftol(VolDcl * static_cast<double>(e.MainGain.Cur));

    // UpdateMixerOutputs 0x470940: cruise detector -> random rpm bumps
    if (t.Mph > 30 && std::fabs(D) < 30) {
        if (c_->Time >= cruiseStart_ + 3.0 && --bumpCountdown_ <= 0) {
            e.BumpRequest = true;
            bumpCountdown_ = rnd_->Next(150) + 60;
        }
    } else {
        cruiseStart_ = c_->Time;
        bumpCountdown_ = 0;
    }
}

void EngineAudioController::HybridMotorCtl::SingleMix(bool shifting)
{
    Telemetry& t = c_->Tel;
    EngineCtl& e = c_->Eng;
    ShiftingCtl& s = c_->Shift;
    AccelTransCtl& ac = c_->Accel;
    const EngineSoundRow& ed = *c_->Sel.Engine;
    double a = ClampD(D / ed.DNorm, 0, 1);
    bool smooth = true;
    double wA, wG;
    auto LerpA = [&](double& x, double& y) {
        x = Lerp(ed.F14, ed.F12, a);
        y = Lerp(ed.F15, ed.F13, a);
    };
    if (shifting) {
        switch (s.State) {
        case 2: smooth = false; LerpA(wA, wG); break;
        case 3: case 5: LerpA(wA, wG); break;
        case 7:
            if (t.ThrOn) {
                LerpA(wA, wG);
                smooth = false;
            } else {
                wA = ed.F16;
                wG = ed.F17;
            }
            break;
        default: wA = ed.F16; wG = ed.F17; break;
        }
    } else if (ac.State != 0) {
        if (ac.State == 1) smooth = false;
        LerpA(wA, wG);
    } else if (t.ThrRaw > 5.0f) {
        LerpA(wA, wG);
    } else {
        wA = ed.F16;
        wG = ed.F17;
    }
    double lpf = ClampD(e.Torque * (24000.0 - ed.LpfMin) * 0.01 + ed.LpfMin, ed.LpfMin, 24000);
    lpf = std::min(lpf, static_cast<double>(e.DistLpf));
    if (smooth) {
        CAems = static_cast<float>(Slew(CAems, wA, 0.2));
        CGin = static_cast<float>(Slew(CGin, wG, 0.2));
        CLpf = static_cast<float>(Slew(CLpf, lpf, 6000));
    } else {
        CAems = static_cast<float>(wA);
        CGin = static_cast<float>(wG);
        CLpf = static_cast<float>(lpf);
    }
    double g = e.Vol / 32000.0;
    VolAems = Ftol(ed.VolAems * static_cast<double>(CAems) * g);
    VolGin = Ftol(ed.VolGin * static_cast<double>(CGin) * g);
    VolDcl = 0;
    VolRed = Ftol(ed.VolAems * g);
}

void EngineAudioController::HybridMotorCtl::DualMix(bool shifting)
{
    Telemetry& t = c_->Tel;
    EngineCtl& e = c_->Eng;
    ShiftingCtl& s = c_->Shift;
    AccelTransCtl& ac = c_->Accel;
    const EngineSoundRow& ed = *c_->Sel.Engine;
    const DualGinRow& dg = *c_->Sel.Dual;
    double a = ClampD(std::fabs(D) / ed.DNorm, 0, 1), b = ClampD(std::fabs(D) / dg.DNorm, 0, 1);
    bool smooth = true;
    double wA, wG;
    auto LerpA = [&](double& x, double& y) {
        x = Lerp(ed.F14, ed.F12, a);
        y = Lerp(ed.F15, ed.F13, a);
    };
    auto LerpB = [&](double& x, double& y) {
        x = Lerp(dg.F11, dg.F9, b);
        y = Lerp(dg.F12, dg.F10, b);
    };
    if (shifting) {
        switch (s.State) {
        case 2: smooth = false; LerpA(wA, wG); break;
        case 3: case 5: LerpA(wA, wG); break;
        case 7: LerpA(wA, wG); if (t.ThrOn) smooth = false; break;
        default: LerpB(wA, wG); break;
        }
    } else if (ac.State != 0) {
        if (ac.State == 1) smooth = false;
        LerpA(wA, wG);
    } else if (t.ThrOn) {
        LerpA(wA, wG);
    } else {
        LerpB(wA, wG);
    }
    if (smooth) {
        CAems = static_cast<float>(Slew(CAems, wA, 0.2));
        CGin = static_cast<float>(Slew(CGin, wG, 0.2));
    } else {
        CAems = static_cast<float>(wA);
        CGin = static_cast<float>(wG);
    }
    double gv = dclWindow_.Value(GinRpm);
    double tq = e.Torque * 0.01;
    double w = (1 - tq) * gv + tq;
    double cA = w * CAems + (1 - w), cG = CGin * w;
    CLpf = static_cast<float>(e.DistLpf);
    double g = e.Vol * 3.0518509e-05;                          // 0x78A6C8 = 1/32767
    if (c_->Opt.ReferenceDualMix) {
        // audio_engine_ref.py: windowed weights kept local, base volume not truncated.
        VolAems = Ftol((dg.VolAems0 + tq * (ed.VolAems - dg.VolAems0)) * cA * g);
    } else {
        // asm 0x470812..0x470830: the windowed weights are written back into the smoothed state,
        // and the AEMS base volume is truncated to int first (0x470879).
        CAems = static_cast<float>(cA);
        CGin = static_cast<float>(cG);
        cA = CAems;
        cG = CGin;
        int baseVol = dg.VolAems0 - Ftol(e.Torque * static_cast<double>(ed.VolAems - dg.VolAems0) * -0.0099999998);
        VolAems = Ftol(baseVol * cA * g);
    }
    VolGin = Ftol(e.Torque * static_cast<double>(ed.VolGin) * g * cG * 0.0099999998);
    VolDcl = Ftol((1 - e.Torque * 0.0099999998) * dg.VolDcl * g * cG);
    VolRed = Ftol(ed.VolAems * g);
}

} // namespace bb
