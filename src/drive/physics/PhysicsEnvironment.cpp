#include "drive/physics/PhysicsEnvironment.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace bb {

// ------------------------------------------------------------------ PhysicsEnvironment

namespace {
const Speed2Image& VerifyFirst(const Speed2Image& exe) {
    WheelK::Verify(exe);
    Graph::Verify(exe);
    return exe;
}
} // namespace

PhysicsEnvironment::PhysicsEnvironment(const Speed2Image& exe, const BWareMath* math)
    : Math(math != nullptr ? *math : BWareMath(VerifyFirst(exe))),
      Tweaks(WheelTweaks::Load(exe)),
      Tables(exe),
      Engine(exe),
      Rb(exe),
      Mover(exe) {
    if (math != nullptr) VerifyFirst(exe);
    World.UglyTimestepHack = exe.F32(0x7FC800);
    World.TestRPM = exe.F32(0x7FBED8);
}

// ------------------------------------------------------------------ FlatGround

float FlatGround::GetElevation(void* /*topology*/, const float* /*point*/, int& terrain, float* normal, bool& valid) {
    terrain = TerrainType;
    normal[0] = 0.0f; normal[1] = 0.0f; normal[2] = 1.0f;
    valid = true;
    return Height;
}

float FlatGround::GetRoadNoise(const float* /*pointXY*/, int /*terrain*/) { return 0.0f; }

// ------------------------------------------------------------------ MoverData

namespace {
// Points written by the static initialisers 0x77DFB0 (dword_86B5E8..) and 0x77E040 (dword_86B5B0..):
// mov dword ptr [addr], imm32. Only the last y differs (1.0 for the racer-AI graph, 0.0 otherwise).
const uint32_t DriftBits[13] = {
    0x00000000, 0x00000000, 0x3E860A92, 0x3DCCCCCD, 0x3F060A92, 0x3EE66666, 0x3F490FDB, 0x3F59999A,
    0x3F860A92, 0x3F733333, 0x3FC6D3F2, 0x3F933333, 0x3FC90FDB,
};
} // namespace

MoverData::MoverData(const Speed2Image& e) {
    UseNewSuspensionCalibration = static_cast<int>(e.U32(0x7FC06C));
    Gravity = F(e, 0x7FBEF0); HydraulicCgMix = F(e, 0x7842A4); Quarter = F(e, 0x784D28); Half = F(e, 0x784260);
    MaximumTimeStep = F(e, 0x7FC070);
    for (int t = 0; t < WheelTables::TerrainCount; t++) TerrainDrag[t] = F(e, 0x800F10 + static_cast<uint32_t>(WheelTables::TerrainStride * t));
    ModeOneGravity = F(e, 0x7FC084); ModeOneMaxThrottle = F(e, 0x7A1E4C); RestThrottle = F(e, 0x78758C);
    RestPitch = F(e, 0x7A1A1C); CatchupEnabled = static_cast<int>(e.U32(0x7FC0A0)); OneMs = F(e, 0x7A1824);
    CounterSteerYawClamp = F(e, 0x7A1A18); PointOne = F(e, 0x784358); RwdSplit = F(e, 0x7850EC);
    MpsToMph = F(e, 0x78426C); RearSplitMph = F(e, 0x787590);
    PilotOffGrip = F(e, 0x7FC060); EBrakeGripFall = F(e, 0x7A1A08); EBrakeGripMin = F(e, 0x7A1A0C);
    EBrakeGripRise = F(e, 0x7A1A04); RainGrip = F(e, 0x7A19E4); LaunchThrottle = F(e, 0x7850EC);
    LaunchMph = F(e, 0x784560); LaunchBrakeMul = F(e, 0x7844F4); EBrakeBoostMin = F(e, 0x784554);
    DriftSlip = F(e, 0x7A1A10); DriftMph = F(e, 0x7A1A14);
    for (int i = 0; i < 4; i++) { HandbrakeMask[i] = F(e, 0x7FC074 + 4 * static_cast<uint32_t>(i)); NosFriction[i] = F(e, 0x7A17A4 + 4 * static_cast<uint32_t>(i)); }
    HitGripMaxSec = F(e, 0x7A5650);
    RecentHitGrip = GraphAt(e, 0x802570, 4);
    RgbEBrake = F(e, 0x7FC064); RgbMaxSlipDeg = F(e, 0x7FC068); DegToRad = F(e, 0x784244);
    RgbSpeedMin = F(e, 0x86B318); RgbValueMin = F(e, 0x86B314); RgbSpeedMax = F(e, 0x7FC058);
    RgbValueMax = F(e, 0x7FC054); RgbMax = F(e, 0x7FC05C);
    DousingDamping = F(e, 0x7A1A5C); RoofResetSec = F(e, 0x7A1A78); DampSpeed = F(e, 0x7A1A60); DampSpin = F(e, 0x7A1A64);
    DampTilt = F(e, 0x7A1A68); RestSpeed = F(e, 0x7A1A70); RestSpin = F(e, 0x7A1A74); TriggerBounceImpulse = F(e, 0x7A1A00);
    DynoGate = F(e, 0x7A1A20); JumpMinSpin = F(e, 0x7A1A50);
    JumpStabilization = GraphAt(e, 0x7FC8F8, 3);
    ForceYaw = GraphAt(e, 0x802590, 3);
    ForceYawSpeedMin = F(e, 0x7A5668); ForceYawSpeedMax = F(e, 0x7A566C); ForceYawGain = F(e, 0x7A5664);
    DriftYawAi = DriftGraph(1.0f);
    DriftYaw = DriftGraph(0.0f);
    DriftRearFriction = WheelTable::Read(e, 0x7FC910);
    AngToRad = F(e, 0x784240); Pi = F(e, 0x7A1788);
    HydHeightScale = F(e, 0x7FC09C); HydMinHeight = F(e, 0x7FC08C); HydRate = F(e, 0x7FC098);
    HydMinCompression = F(e, 0x7FC090); HydVelGain = F(e, 0x7FC088); HydBase = F(e, 0x7FC094);
    DynoSpring = F(e, 0x7A1A54); DynoDamper = F(e, 0x7A1A58);
    {
        std::vector<uint8_t> b = e.Bytes(0x7A20B0, 8);
        std::memcpy(&DynoMinSpeed, b.data(), 8);
    }
    SteerToDeg = F(e, 0x784248); SteerVisRate = F(e, 0x7A1A28); Deg180 = F(e, 0x784510); Deg360 = F(e, 0x78450C);
    F65536 = F(e, 0x784238); RadToRpm = F(e, 0x7A1D28); TestRpmOff = F(e, 0x784258);
    RpmKeep = F(e, 0x7A20A8); RpmNew = F(e, 0x7A20A4);
    for (int i = 0; i < 8; i++) SmoothRPMDecel[i] = F(e, 0x7A1A2C + 4 * static_cast<uint32_t>(i));
    SpinDiv = F(e, 0x7A1A24); RpmToRad = F(e, 0x7A1D38);
    for (int i = 0; i < 3; i++) PerfectShiftRangeMultipliers[i] = F(e, 0x7A19E8 + 4 * static_cast<uint32_t>(i));
    VerifyDriftInit(e);
}

Graph MoverData::GraphAt(const Speed2Image& e, uint32_t pts, int n) {
    std::vector<float> p(static_cast<size_t>(2 * n));
    for (int i = 0; i < 2 * n; i++) p[i] = F(e, pts + 4 * static_cast<uint32_t>(i));
    return Graph(std::move(p), n);
}

Graph MoverData::DriftGraph(float lastY) {
    std::vector<float> p(14);
    for (int i = 0; i < 13; i++) p[i] = X87::Bits(DriftBits[i]);
    p[13] = lastY;
    return Graph(std::move(p), 7);
}

void MoverData::VerifyDriftInit(const Speed2Image& e) {
    for (int g = 0; g < 2; g++) {
        uint32_t code = g == 0 ? 0x77DFB0u : 0x77E040u, data = g == 0 ? 0x86B5E8u : 0x86B5B0u;
        for (int i = 0; i < 14; i++) {
            uint32_t ins = code + 10 * static_cast<uint32_t>(i);
            uint32_t want = i < 13 ? DriftBits[i] : (g == 0 ? 0x3F800000u : 0u);
            if (e.U16(ins) != 0x05C7 || e.U32(ins + 2) != data + 4 * static_cast<uint32_t>(i) || e.U32(ins + 6) != want) {
                char text[96];
                std::snprintf(text, sizeof text, "drift graph initialiser differs at 0x%08X", ins);
                throw std::runtime_error(text);
            }
        }
    }
}

} // namespace bb
