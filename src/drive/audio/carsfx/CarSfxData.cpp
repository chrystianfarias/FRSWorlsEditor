// Port of NFSU2Unity Core/Audio/CarSfx/CarSfxData.cs (CarSfxData).

#include "drive/audio/carsfx/CarSfxData.h"

#include "drive/numerics/Speed2Image.h"

namespace bb {

namespace {
std::string CStr(const Speed2Image& exe, uint32_t va)
{
    std::string s;
    for (uint32_t i = 0; i < 128; i++) {
        uint8_t b = exe.Bytes(va + i, 1)[0];
        if (b == 0) break;
        s += static_cast<char>(b);
    }
    return s;
}
} // namespace

CarSfxData::CarSfxData(const Speed2Image& exe, const int* cosTable)
{
    if (cosTable != nullptr) CosTable.assign(cosTable, cosTable + 513);
    for (int i = 0; i < SweetenerCount; i++) {
        uint32_t a = SweetenerVa + static_cast<uint32_t>(12 * i);
        uint32_t name = exe.U32(a);
        Sweeteners[i].Bank = name == 0 ? std::string() : CStr(exe, name);
        Sweeteners[i].Vol1 = static_cast<int32_t>(exe.U32(a + 4));
        Sweeteners[i].Vol2 = static_cast<int32_t>(exe.U32(a + 8));
    }
    for (int k = 0; k < 4; k++) {
        for (int j = 0; j < 18; j++) {
            uint32_t a = SkidParamsVa + static_cast<uint32_t>(0x48 * k + 4 * j);
            bool isInt = j == 6 || j == 7 || j == 12 || j == 13;    // +0x18 +0x1C +0x30 +0x34
            SkidParams[k][j] = isInt ? static_cast<float>(static_cast<int32_t>(exe.U32(a))) : exe.F32(a);
        }
    }
    for (int i = 0; i < 10; i++) RoadNoiseGain[i] = static_cast<int32_t>(exe.U32(RoadNoiseGainVa + static_cast<uint32_t>(4 * i)));
}

} // namespace bb
