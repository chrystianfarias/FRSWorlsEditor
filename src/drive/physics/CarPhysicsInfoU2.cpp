#include "drive/physics/CarPhysicsInfoU2.h"

#include "drive/cars/CarTypeInfoU2.h"

#include <cstring>
#include <stdexcept>

namespace bb {

namespace {
// Sequential reader over buf[start .. end) (the C# BinReader subset used here).
struct Reader {
    const uint8_t* buf;
    size_t start, end, pos;
    void Seek(size_t o) { pos = start + o; }
    float F32() { float f; Need(4); std::memcpy(&f, buf + pos, 4); pos += 4; return f; }
    int S32() { int v; Need(4); std::memcpy(&v, buf + pos, 4); pos += 4; return v; }
    ::Vec3 V3() { float x = F32(), y = F32(), z = F32(); return ::Vec3(x, y, z); }
    void Need(size_t n) const { if (pos + n > end) throw std::runtime_error("CarPhysicsInfoU2: read past the end"); }
};
} // namespace

CarPhysicsInfoU2 CarPhysicsInfoU2::Read(const uint8_t* buf, size_t bufSize, size_t offset) {
    if (offset + Size > bufSize) throw std::runtime_error("CarPhysicsInfoU2: record out of range");
    Reader r{buf, offset, offset + Size, offset};
    CarPhysicsInfoU2 p;
    p.Raw.assign(buf + offset, buf + offset + Size);
    p.CG = r.V3();
    for (int i = 0; i < 4; i++) {
        r.Seek(static_cast<size_t>(0x10 + 0x30 * i));
        Wheel& w = p.Wheels[i];
        w.X = r.F32(); w.Y = r.F32(); w.RideHeight = r.F32(); w.Unknown0C = r.F32(); w.Radius = r.F32(); w.Width = r.F32();
        w.Id = r.S32(); w.RenderY = r.F32(); w.WideBodyY = r.F32();
    }
    auto T = [&r](size_t o) {
        r.Seek(o);
        Tire t;
        t.SlipAnglePeak = r.F32(); t.LoadNorm = r.F32(); t.LateralGrip = r.F32(); t.GripCoef = r.F32(); t.GripCoef2 = r.F32();
        return t;
    };
    p.FrontTire = T(0xD0);
    p.RearTire = T(0xF0);
    r.Seek(0x110);
    p.Mass = r.F32();
    p.BoxLength = r.F32();
    p.BoxWidth = r.F32();
    p.BoxHeight = r.F32();
    for (int i = 0; i < 16; i++) p.InertiaTensor[i] = r.F32();
    p.InertiaZZMul = r.F32();
    auto S = [&r](size_t o) {
        r.Seek(o);
        Suspension s;
        s.Progression = r.F32(); s.SpringRate = r.F32(); s.BumpDamping = r.F32(); s.ReboundDamping = r.F32();
        s.AntiRoll = r.F32(); s.MaxCompression = r.F32(); s.MaxExtension = r.F32(); s.Unknown1C = r.F32();
        return s;
    };
    p.FrontSuspension = S(0x170);
    p.RearSuspension = S(0x190);
    r.Seek(0x1B0);
    p.NeutralShiftFrac = r.F32();
    p.DriveTrain04 = r.F32();
    p.FinalDrive0 = r.F32();
    p.FinalDrive1 = r.F32();
    p.TorqueSplitRear = r.F32();
    p.ClutchSlipCoef = r.F32();
    p.NumForwardGears = r.S32();
    p.ClutchRate = r.F32();
    for (int i = 0; i < 8; i++) p.GearRatios[i] = r.F32();
    r.Seek(0x1F0);
    p.IdleRpm = r.F32();
    p.RedlineRpm = r.F32();
    p.MaxRpm = r.F32();
    p.EngineInertia = r.F32();
    for (int i = 0; i < 9; i++) p.Torque[i] = r.F32();
    for (int i = 0; i < 9; i++) p.TurboTorque[i] = r.F32();
    for (int i = 0; i < 3; i++) p.EngineBraking[i] = r.F32();
    r.Seek(0x260);
    p.Drag = r.F32();
    p.AeroA = r.F32();
    p.AeroB = r.F32();
    p.AeroC = r.F32();
    p.SteerParam = r.F32();
    p.BrakeTorque = r.F32();
    p.EBrakeTorque = r.F32();
    p.BrakeBiasRear = r.F32();
    p.Control280 = r.F32();
    p.Control284 = r.F32();
    p.GroundDamping = r.F32();
    p.AirDamping = r.F32();
    p.Damping290 = r.F32();
    r.Seek(0x2A0);
    p.NitrousCapacity = r.F32();
    p.NitrousShots = r.S32();
    p.Nitrous2A8 = r.F32();
    p.NitrousTorqueMul = r.F32();
    return p;
}

CarPhysicsInfoU2 CarPhysicsInfoU2::FromCarType(const CarTypeInfoU2& carType) {
    return Read(carType.Raw.data(), carType.Raw.size(), CarTypeInfoOffset);
}

} // namespace bb
