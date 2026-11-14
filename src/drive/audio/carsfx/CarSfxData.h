#pragma once

// Port of the CarSfxData part of NFSU2Unity Core/Audio/CarSfx/CarSfxData.cs (the tables). CarSfxBanks there loads
// AemsBank objects for the AEMS virtual machine, which is not ported: EngineSound loads the shift banks itself.

#include <string>
#include <vector>

namespace bb {

class Speed2Image;

// Row of g_SND_SWEETNERDATASET (0x7F3040, 43 x 12 B): shift-sweetener bank and its two levels.
struct SweetenerRow {
    std::string Bank;
    // vol1: CAR_SWTN (disengage / engage); vol2: CAR_Sputter (backfire).
    int Vol1 = 0, Vol2 = 0;
};

// Static car-SFX tables of SPEED2.EXE v1.2 that the engine tables (EngineSoundData) do not cover,
// read from the user's executable at runtime (read-only): sweeteners, the four skid parameter sets (0x7F2F20)
// and the road-noise / wind constants (docs/re/audio-notes/F3-sfx.md, docs/re/port-status-audio-engine.md).
class CarSfxData {
public:
    static constexpr unsigned SweetenerVa = 0x7F3040, SkidParamsVa = 0x7F2F20, RoadNoiseGainVa = 0x7ECF4C;
    static constexpr int SweetenerCount = 43;

    SweetenerRow Sweeteners[SweetenerCount];
    // [set][18]: SFXCTL_Skid parameters (+0x18/+0x1C/+0x30/+0x34 are ints, stored here as floats).
    float SkidParams[4][18] = {};
    // dword_7ECF4C: road-noise terrain loop gains per sample (index 4 = dword_7ECF5C, the base loops).
    int RoadNoiseGain[10] = {};
    std::vector<int> CosTable;

    CarSfxData(const Speed2Image& exe, const int* cosTable);

    // GetCartypeEngGroup 0x458F00 == 2 (trucks / V8): sound mappings 9, 13, 14, 20, 21.
    static bool IsTruckGroup(int mapping)
    {
        return mapping == 9 || mapping == 13 || mapping == 14 || mapping == 20 || mapping == 21;
    }
};

} // namespace bb
