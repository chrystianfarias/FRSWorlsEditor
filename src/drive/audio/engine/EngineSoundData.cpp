// Port of NFSU2Unity Core/Audio/Engine/EngineSoundData.cs.

#include "drive/audio/engine/EngineSoundData.h"

#include "drive/numerics/Speed2Image.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace bb {

namespace {

int I(const Speed2Image& exe, uint32_t va) { return static_cast<int32_t>(exe.U32(va)); }

std::string CStr(const Speed2Image& exe, uint32_t va)
{
    if (va == 0) return "";
    std::string s;
    for (uint32_t i = 0; i < 256; i++) {
        uint8_t b = exe.Bytes(va + i, 1)[0];
        if (b == 0) break;
        s += static_cast<char>(b);
    }
    return s;
}

} // namespace

std::string EngineSelection::ToString() const
{
    std::string s = "cartype " + std::to_string(CarType) + " mapping " + std::to_string(Mapping) + " exhaust " +
                    std::to_string(ExhaustLevel) + " -> engine " + std::to_string(Engine->Index) + " " +
                    Engine->BankEE + " / " + Engine->Gin;
    s += Dual != nullptr ? " + " + Dual->Gin : std::string(" (single)");
    s += ", shift " + (Shift ? std::to_string(Shift->Index) + " " + Shift->Bank : std::string()) + ", turbo " +
         (Turbo ? Turbo->Bank : std::string());
    return s;
}

EngineSoundData::EngineSoundData(const Speed2Image& exe)
{
    for (int i = 0; i < EngineCount; i++) {
        uint32_t a = EngineDataVa + static_cast<uint32_t>(i * 0x4C);
        EngineSoundRow& r = Engines[i];
        r.Index = i;
        r.BankEE = CStr(exe, exe.U32(a));
        r.BankSPU = CStr(exe, exe.U32(a + 4));
        r.AemsId = I(exe, a + 8);
        r.Gin = CStr(exe, exe.U32(a + 0xC));
        r.Dual = I(exe, a + 0x10);
        r.DualIndex = I(exe, a + 0x14);
        r.AccelIndex = I(exe, a + 0x18);
        r.GinMax = exe.F32(a + 0x1C);
        r.Idle = exe.F32(a + 0x20);
        r.VolAems = I(exe, a + 0x24);
        r.VolGin = I(exe, a + 0x28);
        r.DNorm = exe.F32(a + 0x2C);
        r.F12 = exe.F32(a + 0x30);
        r.F13 = exe.F32(a + 0x34);
        r.F14 = exe.F32(a + 0x38);
        r.F15 = exe.F32(a + 0x3C);
        r.F16 = exe.F32(a + 0x40);
        r.F17 = exe.F32(a + 0x44);
        r.LpfMin = I(exe, a + 0x48);
    }
    for (int i = 0; i < DualCount; i++) {
        uint32_t a = DualGinsuVa + static_cast<uint32_t>(i * 0x34);
        DualGinRow& r = Duals[i];
        r.Index = i;
        r.Gin = CStr(exe, exe.U32(a));
        r.VolAems0 = I(exe, a + 4);
        r.VolDcl = I(exe, a + 8);
        r.Lo = I(exe, a + 0xC);
        r.Hi = I(exe, a + 0x10);
        r.F5 = exe.F32(a + 0x14);
        r.F6 = exe.F32(a + 0x18);
        r.DNorm = exe.F32(a + 0x1C);
        r.F9 = exe.F32(a + 0x24);
        r.F10 = exe.F32(a + 0x28);
        r.F11 = exe.F32(a + 0x2C);
        r.F12 = exe.F32(a + 0x30);
    }
    for (int i = 0; i < MappingCount * 4; i++) {
        uint32_t a = CarDataMappingVa + static_cast<uint32_t>(i * 12);
        Mapping[i] = MappingEntry{I(exe, a), I(exe, a + 4), I(exe, a + 8)};
    }
    for (int i = 0; i < ShiftCount; i++) {
        uint32_t a = ShiftPatternsVa + static_cast<uint32_t>(i * 0x60);
        ShiftPatternRow& r = ShiftPatterns[i];
        r.Index = i;
        r.Bank = CStr(exe, exe.U32(a));
        for (int k = 1; k < 24; k++) {
            int x = I(exe, a + static_cast<uint32_t>(4 * k));
            // Mixed int/float row: [2], [8], [16], [23] are floats; any other dword that is not a small int is too.
            bool isFloat = k == 2 || k == 8 || k == 16 || k == 23 || std::llabs(static_cast<long long>(x)) > 0x1000000;
            r.P[k] = isFloat ? exe.F32(a + static_cast<uint32_t>(4 * k)) : static_cast<float>(x);
        }
    }
    for (int i = 0; i < AccelCount; i++) {
        uint32_t a = AccelFromIdleVa + static_cast<uint32_t>(i * 0x14);
        AccelFromIdleRow& r = AccelFromIdle[i];
        r.K = exe.F32(a);
        r.RpmUp = I(exe, a + 4);
        r.MsUp = I(exe, a + 8);
        r.MsDecay = I(exe, a + 12);
        r.MsRelease = I(exe, a + 16);
    }
    for (int i = 0; i < TurboCount; i++) {
        uint32_t a = TurboDatasetVa + static_cast<uint32_t>(i * 24);
        TurboRow& r = Turbos[i];
        r.Bank = CStr(exe, exe.U32(a));
        r.SpoolVol = I(exe, a + 4);
        r.BlowSmall = I(exe, a + 8);
        r.BlowBig = I(exe, a + 12);
        r.MaxBoost = exe.F32(a + 16);
        r.Decay = exe.F32(a + 20);
    }
    for (int i = 0; i < 513; i++) CosTable[i] = I(exe, CosTableVa + static_cast<uint32_t>(4 * i));
}

int EngineSoundData::CarTypeMapping(int carType)
{
    switch (carType) {
    case 0: return 22; case 1: return 10; case 2: return 6; case 3: return 0; case 4: return 19;
    case 5: return 5; case 6: return 23; case 7: return 7; case 8: return 14; case 9: return 21;
    case 10: return 9; case 11: return 31; case 12: return 28; case 13: return 4; case 14: return 17;
    case 15: return 30; case 16: return 12; case 17: return 3; case 18: return 24; case 19: return 8;
    case 20: return 32; case 21: return 26; case 22: return 2; case 23: return 11; case 24: return 1;
    case 25: return 13; case 26: return 20; case 27: return 29; case 29: return 25; case 30: return 15;
    default: return 18;
    }
}

int EngineSoundData::CarTypeByName(const std::string& name)
{
    static const char* const names[] = {
        "PEUGOT", "FOCUS", "COROLLA", "240SX", "MIATA", "CIVIC", "PEUGOT106", "CORSA", "HUMMER", "NAVIGATOR",
        "ESCALADE", "TIBURON", "SENTRA", "CELICA", "IS300", "SUPRA", "GOLF", "A3", "RSX", "ECLIPSE", "TT", "RX8",
        "350Z", "G35", "3000GT", "GTO", "MUSTANGGT", "SKYLINE", "LANCER", "RX7", "IMPREZAWRX"};
    std::string u = name;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (int i = 0; i < static_cast<int>(sizeof names / sizeof names[0]); i++)
        if (u == names[i]) return i;
    return -1;
}

EngineSelection EngineSoundData::Select(int carType, int exhaustLevel, int drivetrainLevel, int turboLevel,
                                        bool splitScreen) const
{
    int m = CarTypeMapping(carType);
    auto Lv = [](int x) { return x < 0 ? 0 : x > 3 ? 3 : x; };
    const EngineSoundRow& e = Engines[Mapping[m * 4 + Lv(exhaustLevel)].engine];
    EngineSelection s;
    s.CarType = carType;
    s.Mapping = m;
    s.ExhaustLevel = Lv(exhaustLevel);
    s.DrivetrainLevel = Lv(drivetrainLevel);
    s.TurboLevel = Lv(turboLevel);
    s.Engine = &e;
    s.Dual = e.Dual != 0 && !splitScreen ? &Duals[e.DualIndex] : nullptr;
    s.Shift = &ShiftPatterns[Mapping[m * 4 + Lv(drivetrainLevel)].shift];
    s.AccelFromIdle = &AccelFromIdle[std::max(0, std::min(AccelCount - 1, e.AccelIndex))];
    s.Turbo = &Turbos[Mapping[m * 4 + Lv(turboLevel)].turbo];
    return s;
}

} // namespace bb
