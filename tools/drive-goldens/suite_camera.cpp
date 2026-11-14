// Golden suite for the drive camera (golden/cam_*.json, gen_golden_camera.py): CubicCameraMover ctor + per-frame
// MaybeCameraShake / [SetPovType / SetLookBack] / Update / UpdateCameraShakers on a synthetic car trajectory.
// After the ctor and after every frame, every modelled field of the camera, the mover, its five curves, the
// averager, the shakers and the car's nitrous fields is compared bit for bit with the memory recorded from
// SPEED2.EXE. Port of NFSU2Unity tools/harness/csharp/SuiteCamera.cs.

#include "golden.h"

#include "drive/camera/CameraCar.h"
#include "drive/camera/CameraData.h"
#include "drive/camera/CameraEffects.h"
#include "drive/camera/CameraProjection.h"
#include "drive/camera/Cubic.h"
#include "drive/camera/CubicCameraMover.h"
#include "drive/camera/ViewCamera.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/engine/Nitrous.h"

#include <memory>

using namespace golden;

namespace {

float Fx(std::string_view hex8) { return bb::X87::Bits(Hex(hex8)); }
void Fv(std::string_view hex, float* r, int n) { for (int i = 0; i < n; i++) r[i] = Fx(hex.substr(static_cast<size_t>(8 * i), 8)); }

struct Cmp {
    std::string Diff;
    int Fields = 0, Bad = 0;
    const Bytes* img = nullptr;
    std::string region;
    Cmp* parent = nullptr;
    Cmp* Root() { return parent ? parent->Root() : this; }
    Cmp On(const std::string& r, const Bytes& i) { Cmp c; c.region = r; c.img = &i; c.parent = this; return c; }
    void Report(const std::string& name, const std::string& want, const std::string& got) {
        Cmp* r = Root();
        r->Bad++;
        if (r->Diff.size() < 400) r->Diff += region + "." + name + " want " + want + " got " + got + "; ";
    }
    static std::string X8(uint32_t v) { char t[16]; std::snprintf(t, sizeof t, "%08X", v); return t; }
    void F(int o, float v, const std::string& name) {
        Root()->Fields++;
        uint32_t w = RdU(*img, o), g = bb::X87::Bits(v);
        if (w != g) Report(name + "@" + HexX(static_cast<uint32_t>(o)), X8(w), X8(g));
    }
    void Fa(int o, const float* v, int n, const std::string& name) {
        for (int i = 0; i < n; i++) F(o + 4 * i, v[i], name + "[" + std::to_string(i) + "]");
    }
    void I(int o, int v, const std::string& name) {
        Root()->Fields++;
        int w = RdI(*img, o);
        if (w != v) Report(name + "@" + HexX(static_cast<uint32_t>(o)), std::to_string(w), std::to_string(v));
    }
    void S16(int o, int v, const std::string& name) {
        Root()->Fields++;
        int16_t w; std::memcpy(&w, img->data() + o, 2);
        if (w != static_cast<int16_t>(v)) Report(name + "@" + HexX(static_cast<uint32_t>(o)), std::to_string(w), std::to_string(v));
    }
    void U16(int o, int v, const std::string& name) {
        Root()->Fields++;
        uint16_t w = RdU16(img->data(), o);
        if (w != static_cast<uint16_t>(v)) Report(name + "@" + HexX(static_cast<uint32_t>(o)), std::to_string(w), std::to_string(v));
    }
    void B(int o, int v, const std::string& name) {
        Root()->Fields++;
        uint8_t w = (*img)[static_cast<size_t>(o)];
        if (w != static_cast<uint8_t>(v)) Report(name + "@" + HexX(static_cast<uint32_t>(o)), std::to_string(w), std::to_string(v));
    }
    void NonZero(int o, bool v, const std::string& name) {
        Root()->Fields++;
        bool w = RdU(*img, o) != 0;
        if (w != v) Report(name + "@" + HexX(static_cast<uint32_t>(o)), w ? "True" : "False", v ? "True" : "False");
    }
};

void Key(Cmp& c, int b, const bb::CameraKey& k) {
    c.Fa(b, k.Matrix, 16, "M"); c.Fa(b + 0x40, k.Position, 3, "Pos"); c.Fa(b + 0x50, k.Direction, 3, "Dir");
    c.Fa(b + 0x60, k.Target, 3, "Tgt"); c.Fa(b + 0x70, k.Noise, 16, "Noise");
    c.F(b + 0xB0, k.TargetDistance, "TDist"); c.F(b + 0xB4, k.FocalDistance, "Focal"); c.F(b + 0xB8, k.DepthOfField, "Dof");
    c.F(b + 0xBC, k.NearZ, "Near"); c.F(b + 0xC0, k.FarZ, "Far"); c.U16(b + 0xC4, k.Fov, "Fov");
}

void CompareCamera(Cmp& c, const bb::ViewCamera& cam) {
    Key(c, 0, cam.Current); Key(c, 0xD0, cam.Previous);
    c.Fa(0x1E0, cam.PositionRate, 4, "PosRate"); c.Fa(0x1F0, cam.DirectionRate, 4, "DirRate");
    c.Fa(0x200, cam.TargetRate, 4, "TgtRate"); c.Fa(0x210, cam.NoiseRate, 16, "NoiseRate");
    c.Fa(0x250, cam.ScalarRate, 5, "ScalarRate"); c.U16(0x264, cam.FovRate, "FovRate");
    c.B(0x270, cam.ResetVelocities ? 1 : 0, "Reset"); c.F(0x274, cam.ElapsedTime, "Elapsed");
    c.NonZero(0x280, cam.HiddenCar != nullptr, "HiddenCar");
}

void LoadCamera(const Bytes& img, bb::ViewCamera& cam) {
    auto G = [&](int o) { return RdF(img, o); };
    auto& k = cam.Current;
    for (int i = 0; i < 16; i++) k.Matrix[i] = G(4 * i);
    for (int i = 0; i < 4; i++) { k.Position[i] = G(0x40 + 4 * i); k.Direction[i] = G(0x50 + 4 * i); k.Target[i] = G(0x60 + 4 * i); }
    for (int i = 0; i < 16; i++) k.Noise[i] = G(0x70 + 4 * i);
    k.TargetDistance = G(0xB0); k.FocalDistance = G(0xB4); k.DepthOfField = G(0xB8); k.NearZ = G(0xBC); k.FarZ = G(0xC0);
    k.Fov = RdU16(img.data(), 0xC4);
    for (int i = 0; i < 4; i++) { cam.PositionRate[i] = G(0x1E0 + 4 * i); cam.DirectionRate[i] = G(0x1F0 + 4 * i); cam.TargetRate[i] = G(0x200 + 4 * i); }
    cam.FovRate = RdU16(img.data(), 0x264);
}

void CompareCubic1(Cmp& c, int b, const bb::Cubic1D& q, const std::string& n) {
    c.F(b, q.Val, n + ".Val"); c.F(b + 4, q.dVal, n + ".dVal"); c.F(b + 8, q.Des, n + ".Des"); c.F(b + 0xC, q.dDes, n + ".dDes");
    c.F(b + 0x10, q.A, n + ".A"); c.F(b + 0x14, q.B, n + ".B"); c.F(b + 0x18, q.C, n + ".C"); c.F(b + 0x1C, q.D, n + ".D");
    c.F(b + 0x20, q.T, n + ".T"); c.F(b + 0x24, q.Duration, n + ".Dur"); c.F(b + 0x28, q.TMax, n + ".TMax");
    c.S16(b + 0x2C, q.State, n + ".State"); c.S16(b + 0x2E, q.Flag, n + ".Flag");
}

void CompareCubic3(Cmp& c, const bb::Cubic3D& q, const std::string& n) {
    CompareCubic1(c, 0, q.X, n + ".X"); CompareCubic1(c, 0x30, q.Y, n + ".Y"); CompareCubic1(c, 0x60, q.Z, n + ".Z");
}

// Returns an empty string when every field matches.
std::string Compare(const JVal& o, const bb::CubicCameraMover& m, const bb::CameraContext& ctx, const bb::CarState& cs, int& fields) {
    // Keep the decoded images alive for the whole comparison.
    std::map<std::string, Bytes> imgs;
    auto H = [&](const char* k) -> const Bytes& { return imgs[k] = FromHex(o[k].s); };
    Cmp root; root.region = "cam"; root.img = &H("cam");
    CompareCamera(root, *m.Camera);
    Cmp mv = root.On("mover", H("mover"));
    mv.I(0x10, m.ViewId, "ViewId"); mv.I(0x30, m.Field30, "F30"); mv.I(0x48, m.PovType, "Pov"); mv.I(0x4C, m.EffectivePov, "EffPov");
    mv.B(0x50, m.LagEnabled ? 1 : 0, "Lag"); mv.B(0x51, m.LookBack ? 1 : 0, "LookBack"); mv.B(0x52, m.Snap ? 1 : 0, "Snap");
    mv.B(0x53, m.NoDof ? 1 : 0, "NoDof"); mv.B(0x54, m.Field54, "F54"); mv.I(0x58, m.GroundTimer, "GroundTimer");
    { Cmp c = root.On("fov", H("fov")); CompareCubic1(c, 0, m.Fov, "Fov"); }
    { Cmp c = root.On("eye", H("eye")); CompareCubic3(c, m.Eye, "Eye"); }
    { Cmp c = root.On("look", H("look")); CompareCubic3(c, m.Look, "Look"); }
    { Cmp c = root.On("fwd", H("fwd")); CompareCubic3(c, m.Forward, "Fwd"); }
    { Cmp c = root.On("up", H("up")); CompareCubic3(c, m.Up, "Up"); }
    Cmp av = root.On("avg", H("avg"));
    av.B(5, m.Averager.Size, "N"); av.B(6, m.Averager.Count, "Count"); av.B(7, m.Averager.Index, "Index");
    av.Fa(0x10, m.Averager.Sum, 3, "Sum"); av.Fa(0x20, m.Averager.Avg, 3, "Avg");
    Cmp ab = root.On("avgbuf", H("avgbuf"));
    for (int i = 0; i < 4; i++) {
        const float v[3] = { m.Averager.Buffer[static_cast<size_t>(4 * i)], m.Averager.Buffer[static_cast<size_t>(4 * i + 1)],
                             m.Averager.Buffer[static_cast<size_t>(4 * i + 2)] };
        ab.Fa(16 * i, v, 3, "Buf" + std::to_string(i));
    }
    Cmp sh = root.On("shakers", H("shakers"));
    for (int i = 0; i < 2; i++) {
        const auto& s = ctx.Shakers[i]; int b = 0x20 * i;
        std::string n = std::to_string(i);
        sh.Fa(b, s.Dir, 3, "Dir" + n); sh.NonZero(b + 0x10, s.Active, "Active" + n); sh.F(b + 0x14, s.T, "T" + n);
        sh.F(b + 0x18, s.Duration, "Dur" + n); sh.F(b + 0x1C, s.Flicker, "Flicker" + n);
    }
    Cmp nos = root.On("nos", H("nos"));
    nos.I(0, cs.NosCapacity, "Cap"); nos.I(4, cs.NosShot, "Shot"); nos.I(8, cs.NosRemaining, "Rem"); nos.I(0xC, cs.NosStartTime, "Start");
    fields = root.Fields;
    return root.Bad == 0 ? std::string() : root.Diff;
}

void SetFrame(bb::CameraCar& car, bb::CameraContext& ctx, const JVal& f) {
    ctx.WorldTimer = f["wt"].I32();
    ctx.WorldTimeElapsed = Fx(f["dt"].s);
    Fv(f["pos"].s, car.Pos, 3);
    Fv(f["geom"].s, car.GeomPos, 3);
    Fv(f["vel"].s, car.Vel, 3);
    Fv(f["acc"].s, car.Accel, 3);
    float R[16];
    Fv(f["R"].s, R, 16);
    std::memcpy(car.R, R, sizeof R);
    car.UpR2[0] = R[8]; car.UpR2[1] = R[9]; car.UpR2[2] = R[10];
    car.Speed = Fx(f["speed"].s);
    car.WheelsOnGround = f["wheels"].I32();
    int i = 0;
    for (const JVal& t : f["terrain"].a) car.WheelTerrain[i++] = t.I32();
    car.Throttle = Fx(f["throttle"].s);
}

class SuiteCamera : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("cam_", 0) == 0; }

    void Evaluate(bb::BWareMath&, const std::string& goldenName, GoldenCase& gc, std::string& want, std::string& got) override {
        if (!data_) {
            exe_ = std::make_unique<bb::Speed2Image>(ExePath());
            data_ = std::make_unique<bb::CameraData>(*exe_);
            env_ = std::make_unique<bb::PhysicsEnvironment>(*exe_);
        }
        const JVal& c = *gc.Root;
        if (goldenName == "cam_Projection") {
            bb::CameraKey key;
            key.NearZ = Fx(c["zn"].s); key.FarZ = Fx(c["zf"].s); key.Fov = static_cast<uint16_t>(c["fov"].I32());
            auto p = bb::CameraProjection::Build(*data_, key, c["view"].I32(), c["w"].I32(), c["h"].I32());
            auto mtx = p.Matrix();
            want = c["proj"].Str();
            got = HexFloats(mtx.data(), 16);
            return;
        }
        bb::CameraContext ctx;
        const JVal& fl = c["flags"];
        ctx.OwnFlags.DragByte = static_cast<uint8_t>(fl["drag"].I32());
        ctx.OwnFlags.DriftByte = static_cast<uint8_t>(fl["drift"].I32());
        ctx.OwnFlags.BurnoutByte = static_cast<uint8_t>(fl["burnout"].I32());
        ctx.PlayerCinematic = c["cinematic"].I32();
        std::vector<uint8_t> ctiCam(0x7F0, 0);
        Bytes camBytes = FromHex(c["cti_cam"].s);
        std::memcpy(ctiCam.data() + 0x720, camBytes.data(), camBytes.size());
        bb::CarState cs;
        bb::CameraInfo cameras = bb::CameraInfo::FromCarType(ctiCam);
        bb::CameraCar car;
        car.Cameras = &cameras; car.Mode = c["mode"].I32();
        car.OnTopology = c["topo"].I32() != 0; car.HasRideInfo = c["ride"].I32() != 0;
        car.RideInfo20 = Fx(c["ride20"].s); car.DriverFlag2FB = c["drvflag"].I32() != 0;
        bb::PhysicsEnvironment* env = env_.get();
        car.NitrousOn = [&cs, &ctx, env] { return bb::Nitrous::IsNitrousOn(cs, ctx.WorldTimer, env->Engine); };
        bb::ViewCamera cam;
        LoadCamera(FromHex(c["cam0"].s), cam);
        SetFrame(car, ctx, c["ctor_frame"]);
        bb::CubicCameraMover m(*data_, ctx, cam, 1, car, c["pov"].I32(), c["disable_lag"].I32() != 0, c["lookback"].I32() != 0, true);
        int nf = 0;
        std::string first = Compare(c["ctor"], m, ctx, cs, nf);
        std::string where = first.empty() ? std::string() : "ctor: " + first;
        int k = 0;
        for (const JVal& f : c["frames"].a) {
            k++;
            if (!where.empty()) break;
            SetFrame(car, ctx, f);
            if (const JVal* ns = f.Get("nos_start")) {
                cs.NosCapacity = (*ns)[0].I32(); cs.NosShot = (*ns)[1].I32(); cs.NosRemaining = (*ns)[2].I32(); cs.NosStartTime = (*ns)[3].I32();
            }
            ctx.Shakers[0].Maybe(*data_, car.Accel, false, ctx.PlayerCinematic);
            const JVal& ev = f["ev"];
            if (const JVal* pv = ev.Get("pov")) m.SetPovType(pv->I32());
            if (const JVal* lb = ev.Get("lookback")) m.SetLookBack(lb->I32() != 0);
            m.Update();
            ctx.Shakers[0].Update(*data_, ctx.WorldTimeElapsed);
            ctx.Shakers[1].Update(*data_, ctx.WorldTimeElapsed);
            std::string d = Compare(f["out"], m, ctx, cs, nf);
            if (!d.empty()) where = "frame " + std::to_string(k) + ": " + d;
        }
        want = "all fields";
        got = where.empty() ? "all fields" : c["name"].Str() + " " + where;
    }

private:
    std::unique_ptr<bb::Speed2Image> exe_;
    std::unique_ptr<bb::CameraData> data_;
    std::unique_ptr<bb::PhysicsEnvironment> env_;
};

Register reg(new SuiteCamera);

} // namespace
