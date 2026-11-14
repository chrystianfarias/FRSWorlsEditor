// Port of NFSU2Unity Core/Camera/CameraData.cs.

#include "drive/camera/CameraData.h"

#include "drive/camera/CameraMath.h"

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace bb {

std::string Pov::ToString() const {
    std::ostringstream s;
    s << "type " << Type << " angle " << Angle << " x " << X << " y " << Y << " z " << Z;
    return s.str();
}

namespace {

template <typename T>
T ReadAt(const std::vector<uint8_t>& b, int o) {
    if (o < 0 || static_cast<size_t>(o) + sizeof(T) > b.size())
        throw std::runtime_error("CameraInfo: CarTypeInfo record too short");
    T v;
    std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}

} // namespace

CameraInfo CameraInfo::FromCarType(const std::vector<uint8_t>& cti, int offset) {
    CameraInfo ci;
    int b = offset + 0x720;
    ci.Count[0] = ReadAt<int32_t>(cti, b);
    ci.Count[1] = ReadAt<int32_t>(cti, b + 4);
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 6; i++) {
            int o = b + 0x10 + s * 0x60 + i * 0x10;
            Pov p;
            p.Type = ReadAt<int16_t>(cti, o); p.Angle = ReadAt<uint16_t>(cti, o + 2);
            p.X = ReadAt<float>(cti, o + 4); p.Z = ReadAt<float>(cti, o + 8); p.Y = ReadAt<float>(cti, o + 0xC);
            ci.Sets[s][i] = p;
        }
    return ci;
}

bool CameraInfo::GetPov(int type, bool splitScreen, Pov& pov) const {
    int s = splitScreen ? 1 : 0;
    for (int pass = 0; pass < 2; pass++) {
        int want = pass == 0 ? type : 0;
        for (int i = 0; i < Count[s] && i < 6; i++)
            if (Sets[s][i].Type == want) { pov = Sets[s][i]; return true; }
    }
    pov = Pov();
    return false;
}

CubicPovData CubicPovData::Lerp(const CubicPovData& next, const CubicPovData& cur, float t) {
    CubicPovData o;
    const float* n = next.F; const float* c = cur.F; float* r = o.F;
    float omt = (F24(CameraMath::One) - t).Store();
    F24 e1 = F24(omt) * c[1] + F24(t) * n[1];
    F24 e2 = F24(omt) * c[2] + F24(t) * n[2];
    float e3 = (F24(omt) * c[3] + F24(t) * n[3]).Store();
    F24 e0 = F24(t) * n[0] + F24(omt) * c[0];
    r[3] = e3; r[0] = e0.Store(); r[1] = e1.Store(); r[2] = e2.Store();
    float tmp[3];
    float a[3];
    float outv[3];
    for (int k = 1; k <= 10; k++) {
        int i = 4 * k;
        tmp[0] = (F24(t) * n[i]).Store(); tmp[1] = (F24(n[i + 1]) * t).Store(); tmp[2] = (F24(n[i + 2]) * t).Store();
        a[0] = c[i]; a[1] = c[i + 1]; a[2] = c[i + 2];
        BWareMath2::ScaleAdd(outv, tmp, a, omt);
        r[i] = outv[0]; r[i + 1] = outv[1]; r[i + 2] = outv[2];
        // the fourth float of blocks 1..10 is not written by the original (stack garbage, never read)
    }
    return o;
}

CubicPovData PovDataTable::Lookup(float x) const {
    float f = ((F24(x) - X0) * InvStep).Store();
    int i = X87::FtoL2Lo(f);
    if (i < 0) return Entries[0].Clone();
    if (i >= Count - 1) return Entries[Count - 1].Clone();
    F24 fi = F24::MulDouble(CameraMath::One, i);
    if (!BWareMath2::LeOrNaN(fi, f)) fi = fi - CameraMath::One;
    float frac = (F24(f) - fi).Store();
    return CubicPovData::Lerp(Entries[i + 1], Entries[i], frac);
}

CameraData::CameraData(const Speed2Image& exe) : Exe(&exe), Math(exe) {
    BWareMath2::CheckConstants(exe);
    for (int t = 0; t < 6; t++) {
        uint32_t b = 0x7EC610 + static_cast<uint32_t>(t) * 20;
        PovDataTable& tbl = PovTables[t];
        tbl.Count = static_cast<int>(exe.U32(b)); tbl.X0 = exe.F32(b + 4); tbl.X1 = exe.F32(b + 8); tbl.InvStep = exe.F32(b + 12);
        uint32_t p = exe.U32(b + 16);
        tbl.Entries.assign(static_cast<size_t>(tbl.Count), CubicPovData());
        for (int e = 0; e < tbl.Count; e++)
            for (int i = 0; i < 44; i++) tbl.Entries[e].F[i] = exe.F32(p + static_cast<uint32_t>(e * 0xB0 + 4 * i));
        PovFov[t] = exe.U16(0x7EB5B4 + 2 * static_cast<uint32_t>(t));
        NoiseAmpByPov[t] = exe.F32(0x7EBE90 + 4 * static_cast<uint32_t>(t));
        NoiseFreqByPov[t] = exe.F32(0x7EBEA8 + 4 * static_cast<uint32_t>(t));
    }
    NearClipInCar = exe.F32(0x7EB5B0); NearClip = exe.F32(0x784260);
    PovLookupX = exe.F32(0x787154); LookUpOffset = exe.F32(0x78715C); HighliteScale = exe.F32(0x787210);
    EyeLookScale = exe.F32(0x78721C); AspectFovScale = exe.F32(0x78721C);
    FocalDistance = exe.F32(0x78707C); DepthOfField = exe.F32(0x787078); NoiseScaleNotDrag = exe.F32(0x7870B8);
    ReflectScale = exe.F32(0x7855D0); ForwardFadeTime = exe.F32(0x787158); TickToSec = exe.F32(0x784268);
    CubicTMax = exe.F32(0x787150); SeedSnapDistance = exe.F32(0x78714C);
    for (uint32_t i = 0; i < 4; i++) DriftSpeedGraph[i] = exe.F32(0x7EC420 + 4 * i);
    for (uint32_t i = 0; i < 8; i++) BirdsEye[i] = exe.F32(0x7EC400 + 4 * i);
    BirdsEyeAngle[0] = exe.U16(0x787080); BirdsEyeAngle[1] = exe.U16(0x787082);
    for (uint32_t i = 0; i < 3; i++) { BurnoutEye[i] = exe.F32(0x7EC5F0 + 4 * i); DriverFlagEye[i] = exe.F32(0x7EC600 + 4 * i); }
    NoiseDefaultMaxSpeed = exe.F32(0x784258); NoiseTableSpan = exe.F32(0x7844F8); Quarter = exe.F32(0x784D28);
    TerrainAmpScale = exe.F32(0x7870BC); NosAmpAdd = exe.F32(0x7870B0); NosFreqMul = exe.F32(0x7870B4);
    for (uint32_t i = 0; i < 20; i++) NoiseSpeedTable[i] = exe.F32(0x7EC430 + 4 * i);
    for (uint32_t i = 0; i < 16; i++) NoiseK[i] = exe.F32(0x7EC480 + 4 * i);
    Half = exe.F32(0x786F6C); Inv2p30 = exe.F32(0x787580); K65536 = exe.F32(0x784238); NoiseRefFov = exe.U16(0x7EC1F0);
    uint32_t cp = exe.U32(0x7FB7A0); int cn = static_cast<int>(exe.U32(0x7FB7A4));
    ShakeCurve.assign(static_cast<size_t>(2 * cn), 0.0f);
    for (int i = 0; i < 2 * cn; i++) ShakeCurve[i] = exe.F32(cp + 4 * static_cast<uint32_t>(i));
    ShakeRotX = exe.F32(0x7FB740); ShakeRotY = exe.F32(0x7FB744); ShakeRotZ = exe.F32(0x7FB748);
    for (uint32_t i = 0; i < 3; i++) ShakeAccelScale[i] = exe.F32(0x7FB750 + 4 * i);
    ShakeMin = exe.F32(0x79DCD4); ShakeMax = exe.F32(0x79DCD8); ShakeAmpLo = exe.F32(0x79DCDC); ShakeAmpHi = exe.F32(0x79DCE0);
    ShakeDurLo = exe.F32(0x79DCE4); ShakeDurHi = exe.F32(0x79DCE8); ShakeFlicker = exe.F32(0x79DC68);
    ShakeCineAmp = exe.F32(0x7850EC); ShakeCineDur = exe.F32(0x78A0EC);
    ProjXScale = exe.F32(0x7A27DC); ProjYScaleA = exe.F32(0x7A27E0); ProjYScaleB = exe.F32(0x7A27D8);
    PlayerFarZ = X87::Bits(0x466A6000u); SpecialAspect = X87::Bits(0x3F19A956u);
}

} // namespace bb
