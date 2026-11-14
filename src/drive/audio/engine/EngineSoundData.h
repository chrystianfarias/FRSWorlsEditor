#pragma once

// Port of NFSU2Unity Core/Audio/Engine/EngineSoundData.cs.

#include <cstdint>
#include <memory>
#include <string>

namespace bb {

class Speed2Image;

// Row of g_SND_ENGINE_DATA (0x7F0DE0, 43 x 0x4C). Field names follow docs/re/audio-engine.md 2.2.
struct EngineSoundRow {
    int Index = 0;
    std::string BankEE, BankSPU;      // +0x00 / +0x04
    int AemsId = 0;                   // +0x08 -> CAR d00
    std::string Gin;                  // +0x0C accel GIN
    int Dual = 0;                     // +0x10 1 = accel + decel GIN
    int DualIndex = 0;                // +0x14 row of g_SND_DUAL_GINSU
    int AccelIndex = 0;               // +0x18 row of g_SND_ACCELFROMIDLE
    float GinMax = 0;                 // +0x1C f7
    float Idle = 0;                   // +0x20 f8
    int VolAems = 0;                  // +0x24 f9
    int VolGin = 0;                   // +0x28 f10
    float DNorm = 0;                  // +0x2C f11
    float F12 = 0, F13 = 0, F14 = 0, F15 = 0, F16 = 0, F17 = 0; // +0x30..+0x44
    int LpfMin = 0;                   // +0x48 f18
};

// Row of g_SND_DUAL_GINSU (0x7F1AA8, 37 x 0x34).
struct DualGinRow {
    int Index = 0;
    std::string Gin;                  // decel (_DCL) GIN
    int VolAems0 = 0;                 // f1 AEMS volume at T = 0
    int VolDcl = 0;                   // f2
    int Lo = 0, Hi = 0;               // f3/f4 DCL rpm window (int)
    float F5 = 0, F6 = 0;             // fade fractions (high / low end)
    float DNorm = 0;                  // f7 |dRPM| normaliser
    float F9 = 0, F10 = 0, F11 = 0, F12 = 0; // weights AEMS/GIN at b = 1 / b = 0
};

// Row of g_SND_SHIFTPATTERNS (0x7F2860, 18 x 0x60). P[1..23] as in audio-engine.md 2.4 (P[0] unused).
struct ShiftPatternRow {
    int Index = 0;
    std::string Bank;
    float P[24] = {};
};

// Row of g_SND_ACCELFROMIDLE (0x7F3248, 12 x 0x14).
struct AccelFromIdleRow {
    float K = 0;
    int RpmUp = 0, MsUp = 0, MsDecay = 0, MsRelease = 0;
};

// Row of g_SND_TURBODATASET (0x7F3338, 15 x 24 B).
struct TurboRow {
    std::string Bank;
    int SpoolVol = 0, BlowSmall = 0, BlowBig = 0;
    float MaxBoost = 0, Decay = 0;
};

// Everything the player-engine sound needs for one car: picked rows and file names (rows point into
// the EngineSoundData that made the selection, which must outlive it).
struct EngineSelection {
    int CarType = 0, Mapping = 0, ExhaustLevel = 0, DrivetrainLevel = 0, TurboLevel = 0;
    const EngineSoundRow* Engine = nullptr;
    // Null when the engine is single (f4 = 0) or in split screen.
    const DualGinRow* Dual = nullptr;
    const ShiftPatternRow* Shift = nullptr;
    const AccelFromIdleRow* AccelFromIdle = nullptr;
    const TurboRow* Turbo = nullptr;

    // SOUND\ENGINE\ relative paths.
    std::string BankPath() const { return "SOUND\\ENGINE\\" + Engine->BankEE; }
    std::string GinPath() const { return "SOUND\\ENGINE\\" + Engine->Gin; }
    std::string DclPath() const { return Dual == nullptr ? std::string() : "SOUND\\ENGINE\\" + Dual->Gin; }

    std::string ToString() const;
};

// Static engine-audio tables of SPEED2.EXE v1.2, read from the user's executable (read-only) like the
// physics tables: nothing is copied into the repo.
class EngineSoundData {
public:
    static constexpr uint32_t EngineDataVa = 0x7F0DE0, DualGinsuVa = 0x7F1AA8, CarDataMappingVa = 0x7F2230;
    static constexpr uint32_t ShiftPatternsVa = 0x7F2860, AccelFromIdleVa = 0x7F3248, TurboDatasetVa = 0x7F3338;
    static constexpr uint32_t CosTableVa = 0x7EF7F8;
    static constexpr int EngineCount = 43, DualCount = 37, MappingCount = 33, ShiftCount = 18, AccelCount = 12,
                         TurboCount = 15;

    struct MappingEntry { int engine = 0, shift = 0, turbo = 0; };

    EngineSoundRow Engines[EngineCount];
    DualGinRow Duals[DualCount];
    ShiftPatternRow ShiftPatterns[ShiftCount];
    AccelFromIdleRow AccelFromIdle[AccelCount];
    TurboRow Turbos[TurboCount];
    // [mapping * 4 + level] = (engine, shiftPattern, turboDataset).
    MappingEntry Mapping[MappingCount * 4];
    // Mixer curve table T (0x7EF7F8, 513 ints, ~32767 cos(pi/2 i/512)).
    int CosTable[513] = {};

    explicit EngineSoundData(const Speed2Image& exe);

    // GetCarTypeMapping 0x459240: CarType -> sound mapping (28 and > 30 -> 18, also traffic).
    static int CarTypeMapping(int carType);

    // CarType index by collection name (order of the CarTypeInfo table, = CarTypeInfo+0x840); -1 when unknown.
    static int CarTypeByName(const std::string& name);

    // EAXCar::CarPreSetup 0x4593B0: picks the engine row by exhaust level (0 = stock, 1 muffler, 2 cat-back,
    // 3 "blueprint the block"), the shift pattern by drivetrain level and the turbo set by turbo level.
    // splitScreen (FEDatabase == 4) forces the single (no DCL) path.
    EngineSelection Select(int carType, int exhaustLevel = 0, int drivetrainLevel = 0, int turboLevel = 0,
                           bool splitScreen = false) const;
};

} // namespace bb
