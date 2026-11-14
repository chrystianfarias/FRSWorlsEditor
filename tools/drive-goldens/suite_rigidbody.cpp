// Golden suite for the rigid body / aerodynamics area (golden/rb_*.json, gen_golden_rigidbody.py).
// Port of NFSU2Unity tools/harness/csharp/SuiteRigidBody.cs.
// Ports under test: BWareMath2 and RigidBody, Aerodynamics, CarCollisionBody.

#include "golden.h"

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/CarState.h"
#include "drive/physics/World.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/rigidbody/RigidBodyData.h"

#include <memory>

using namespace golden;
using namespace bb;

namespace {

class SuiteRigidBody : public Suite {
    std::unique_ptr<RigidBodyData> data_;
    const RigidBodyData& Data() {
        if (!data_) { Speed2Image exe(ExePath()); data_ = std::make_unique<RigidBodyData>(exe); }
        return *data_;
    }

    static std::string Hex(const float* f, size_t n) { return HexFloats(f, n); }
    static std::string HexB(const Bytes& b, size_t from = 0) { return HexBytes(b.data() + from, b.size() - from); }
    static std::string OutFrom(const GoldenCase& c, int id, size_t from) { return c.OutBuf(id).substr(2 * from); }
    static int Fix(const GoldenCase& c, int id, int off) {
        auto it = c.Fixups.find(id);
        if (it == c.Fixups.end()) return -1;
        for (auto& f : it->second) if (f.first == off) return f.second;
        return -1;
    }
    static bool ArgIsBuf(const GoldenCase& c, int i) { return c.Arg(i)["t"].s == "buf"; }

    // x87 80-bit image (hex, as loader.py prints it) of an exact x87 value held in a double.
    static std::string F80(double v) {
        int64_t bits; std::memcpy(&bits, &v, 8);
        int sign = bits < 0 ? 0x8000 : 0;
        int e = static_cast<int>((bits >> 52) & 0x7FF);
        uint64_t m = static_cast<uint64_t>(bits) & 0xFFFFFFFFFFFFFULL;
        int se; uint64_t mant;
        if (e == 0 && m == 0) { se = sign; mant = 0; }
        else if (e == 0x7FF) { se = sign | 0x7FFF; mant = 0x8000000000000000ULL | (m << 11); }
        else if (e == 0) {
            int sh = 0;
            while ((m & (1ULL << 52)) == 0) { m <<= 1; sh++; }
            se = sign | (1 - 1023 - sh + 16383); mant = m << 11;
        } else { se = sign | (e - 1023 + 16383); mant = (0x10000000000000ULL | m) << 11; }
        char t[40];
        std::snprintf(t, sizeof t, "%04X%016llX", se & 0xFFFF, static_cast<unsigned long long>(mant));
        return t;
    }

    // st0 result: f32 always; the raw 80-bit value too unless NaN (NaN payloads are not modelled).
    static void St0(const GoldenCase& c, F24 r, std::string& want, std::string& got) {
        const JVal& o = c.Out();
        got = golden::F32(r.Store());
        want = o["f32"].Str();
        if (!std::isnan(r.V)) { want += " " + o["f80"].Str(); got += " " + F80(r.V); }
    }

    struct LoadedRb {
        std::unique_ptr<RigidBody> rb;
        std::unique_ptr<RigidBodyInfo> info;
    };

    LoadedRb LoadRb(const GoldenCase& c, int id) {
        LoadedRb l;
        l.rb = std::make_unique<RigidBody>(Data());
        l.rb->ReadFrom(c.Bufs.at(id).data());
        int info = Fix(c, id, 0x14);
        if (info >= 0) {
            l.info = std::make_unique<RigidBodyInfo>(RigidBodyInfo::FromBytes(c.Bufs.at(info).data()));
            l.rb->Info = l.info.get();
        }
        return l;
    }

    static std::string RbImage(const GoldenCase& c, int id, const RigidBody& rb) {
        Bytes b = c.Bufs.at(id);
        rb.WriteTo(b.data());
        return HexB(b, 0x18);
    }

public:
    bool Handles(const std::string& n) const override { return n.rfind("rb_", 0) == 0; }

    void Evaluate(BWareMath&, const std::string& name, GoldenCase& c, std::string& want, std::string& got) override {
        if (name == "rb_GraphGetValueTo") {
            int th = c.ThisId();
            int count = RdI(c.Bufs.at(th), 4);
            const Bytes& pb = c.Bufs.at(Fix(c, th, 0));
            std::vector<float> pts(pb.size() / 4);
            for (size_t i = 0; i < pts.size(); i++) pts[i] = RdF(pb, static_cast<int>(4 * i));
            int ob = c.BufId(0);
            Bytes res = c.Bufs.at(ob);
            float v = 0;
            if (BWareMath2::GraphGetValueTo(pts.data(), count, c.F(1), v)) WrF(res, 0, v);
            want = c.OutBuf(ob); got = HexB(res);
            return;
        }
        if (name == "rb_Lerp576740") {
            float r = BWareMath2::Lerp576740(c.Vec(1, 1)[0], c.Vec(2, 1)[0], c.F(3));
            want = c.OutBuf(c.BufId(0)); got = Hex(&r, 1);
            return;
        }
        if (name == "rb_bClamp") { St0(c, F24(BWareMath2::Clamp(c.F(0), c.F(1), c.F(2))), want, got); return; }
        if (name == "rb_bMin") { St0(c, F24(BWareMath2::Min(c.F(0), c.F(1))), want, got); return; }
        if (name == "rb_bSinCos") {
            float s = 0, co = 0;
            BWareMath2::SinCos(static_cast<uint16_t>(c.U(2)), s, co);
            want = c.OutBuf(c.BufId(0)) + c.OutBuf(c.BufId(1)); got = Hex(&s, 1) + Hex(&co, 1);
            return;
        }
        if (name == "rb_bScaleAdd") {
            float o[3] = {};
            auto a = c.Vec(1, 3), b = c.Vec(2, 3);
            BWareMath2::ScaleAdd(o, a.data(), b.data(), c.F(3));
            want = c.OutBuf(c.BufId(0)); got = Hex(o, 3);
            return;
        }
        if (name == "rb_bRotateVector" || name == "rb_eMulVectorX87") {
            float o[3] = {};
            auto m = c.Vec(1, 16), v = c.Vec(2, 3);
            if (name == "rb_bRotateVector") BWareMath2::RotateVector(o, m.data(), v.data());
            else BWareMath2::MulVector3X87(o, m.data(), v.data());
            want = c.OutBuf(c.BufId(0)); got = Hex(o, 3);
            return;
        }
        if (name == "rb_bNormalizeX87") {
            float o[3] = {};
            auto v = c.Vec(1, 3);
            BWareMath2::NormalizeX87(o, v.data());
            want = c.OutBuf(c.BufId(0)); got = Hex(o, 3);
            return;
        }
        if (name == "rb_eMulMatrixX87") {
            float o[16] = {};
            auto a = c.Vec(1, 16), b = c.Vec(2, 16);
            BWareMath2::MulMatrixX87(o, a.data(), b.data());
            want = c.OutBuf(c.BufId(0)); got = Hex(o, 16);
            return;
        }
        if (name == "rb_bInvertMatrix" || name == "rb_bTransposeMatrix" || name == "rb_eOrthonormalizeMatrix") {
            float o[16] = {};
            auto m = c.Vec(1, 16);
            if (name == "rb_bInvertMatrix") BWareMath2::InvertMatrix(o, m.data());
            else if (name == "rb_bTransposeMatrix") BWareMath2::TransposeMatrix(o, m.data());
            else BWareMath2::OrthonormalizeMatrix(o, m.data());
            want = c.OutBuf(c.BufId(0)); got = Hex(o, 16);
            return;
        }
        if (name == "rb_SkewTimesMatrix") {
            float o[16] = {};
            auto w = c.Vec(1, 3), r = c.Vec(2, 16);
            RigidBody::SkewTimesMatrix(o, 0, w.data(), r.data());
            want = c.OutBuf(c.BufId(0)); got = Hex(o, 16);
            return;
        }
        if (name == "rb_AddTorque" || name == "rb_ApplyForce" || name == "rb_ApplyImpulse" || name == "rb_ApplyImpulseScaled" ||
            name == "rb_SetCGOffset" || name == "rb_CalculateInertiaTensor" || name == "rb_ComputeAuxiliaries") {
            int th = c.ThisId();
            auto l = LoadRb(c, th);
            RigidBody& rb = *l.rb;
            if (name == "rb_AddTorque") rb.AddTorque(c.Vec(0, 3).data());
            else if (name == "rb_ApplyForce") rb.ApplyForce(c.Vec(0, 3).data(), c.Vec(1, 3).data());
            else if (name == "rb_ApplyImpulse") rb.ApplyImpulse(c.Vec(0, 3).data(), c.Vec(1, 3).data());
            else if (name == "rb_ApplyImpulseScaled") rb.ApplyImpulseWithScaledInverseInertiaTensor(c.Vec(0, 3).data(), c.Vec(1, 3).data(), c.F(2));
            else if (name == "rb_SetCGOffset") rb.SetCGOffset(c.Vec(0, 3).data());
            else if (name == "rb_CalculateInertiaTensor") rb.CalculateInertiaTensor();
            else rb.ComputeAuxiliaries();
            want = OutFrom(c, th, 0x18); got = RbImage(c, th, rb);
            return;
        }
        if (name == "rb_GetDerivative") {
            int th = c.ThisId();
            auto l = LoadRb(c, th);
            auto d = c.Vec(0, 32);
            l.rb->GetDerivative(d.data());
            want = OutFrom(c, th, 0x18) + " d=" + c.OutBuf(c.BufId(0));
            got = RbImage(c, th, *l.rb) + " d=" + Hex(d.data(), 32);
            return;
        }
        if (name == "rb_Substep") {
            int th = c.ThisId();
            auto l = LoadRb(c, th);
            const JVal& sub = (*c.Root)["substep"];
            int n = sub["n"].I32();
            bool orth = sub["orth"].b;
            float h = c.F(0);
            for (int k = 0; k < n; k++) {
                float scratch[RigidBody::StateSize] = {};   // the harness zeroes the stack: d[3,23,27,29..31] = 0
                l.rb->Substep(h, scratch);
                if (orth) BWareMath2::OrthonormalizeMatrix(l.rb->R, l.rb->R);
            }
            want = OutFrom(c, th, 0x18); got = RbImage(c, th, *l.rb);
            return;
        }
        if (name == "rb_AeroResolveForces") {
            int th = c.ThisId();
            const Bytes& ab = c.Bufs.at(th);
            const Bytes& ib = c.Bufs.at(Fix(c, th, 0));
            int rbId = Fix(c, th, 4);
            auto l = LoadRb(c, rbId);
            AeroInfo info;
            info.Drag = RdF(ib, 0); info.DownforceFront = RdF(ib, 4); info.DownforceRear = RdF(ib, 8); info.CalibK = RdF(ib, 12);
            Aerodynamics aero;
            aero.Info = &info;
            aero.Rb = l.rb.get();
            aero.SpeedScale = RdF(ab, 8); aero.DragScale = RdF(ab, 0xC);
            aero.Throttle = RdF(ab, 0x10); aero.GroundFrac = RdF(ab, 0x14);
            aero.Unk18 = RdF(ab, 0x18); aero.Unk1C = RdF(ab, 0x1C);
            aero.Enable = RdF(ab, 0x30); aero.TerrainDrag = RdF(ab, 0x34);
            for (int i = 0; i < 4; i++) aero.DragForce[i] = RdF(ab, 0x20 + 4 * i);
            RaceModeFlags mode;
            if (const JVal* gl = c.Root->Get("globals"))
                for (const JVal& g : gl->a) {
                    uint32_t va = golden::Hex(g[0].s);
                    Bytes v = FromHex(g[1].s);
                    if (va == RaceModeFlags::VA_DragByte) mode.DragByte = v[0];
                    else if (va == RaceModeFlags::VA_DriftByte) mode.DriftByte = v[0];
                    else if (va == RaceModeFlags::VA_TweakIsDragRace) mode.TweakIsDragRace = RdI(v, 0);
                    else if (va == RaceModeFlags::VA_TweakDriftPhysics) mode.TweakDriftPhysics = RdI(v, 0);
                    else if (va == RaceModeFlags::VA_TweakIsDriftRace) mode.TweakIsDriftRace = RdI(v, 0);
                }
            bool hasOut = ArgIsBuf(c, 0);
            std::vector<float> outF;
            if (hasOut) outF = c.Vec(0, 1);
            aero.ResolveForces(hasOut ? outF.data() : nullptr, mode, Data());
            Bytes a2 = ab;
            for (int i = 0; i < 4; i++) WrF(a2, 0x20 + 4 * i, aero.DragForce[i]);
            want = OutFrom(c, th, 8) + " rb=" + OutFrom(c, rbId, 0x18) + (hasOut ? " out=" + c.OutBuf(c.BufId(0)) : "");
            got = HexB(a2, 8) + " rb=" + RbImage(c, rbId, *l.rb) + (hasOut ? " out=" + Hex(outF.data(), 1) : "");
            return;
        }
        if (name == "rb_CcbGetMassInverse" || name == "rb_CcbGetFriction" || name == "rb_CcbGetBounciness") {
            int th = c.ThisId();
            Car car;
            CarCollisionBody& ccb = *car.Collision;
            std::unique_ptr<RigidBody> rb;
            int rbId = Fix(c, th, 0x68);
            if (rbId >= 0) { rb = std::make_unique<RigidBody>(Data()); rb->ReadFrom(c.Bufs.at(rbId).data()); ccb.Rb = rb.get(); }
            const Bytes& carB = c.Bufs.at(Fix(c, th, 0x130));
            ccb.SetCarWheelsOnGround(RdI(carB, 0x424));
            ccb.SetCar428(RdI(carB, 0x428));
            F24 r;
            if (name == "rb_CcbGetMassInverse") r = ccb.GetMassInverse();
            else {
                CollisionBodyView other;
                bool has = false;
                if (ArgIsBuf(c, 0)) {
                    int oid = c.BufId(0);
                    other.Type = c.Bufs.at(oid)[0x11];
                    other.NormalZ = RdF(c.Bufs.at(Fix(c, oid, 0x60)), 0x78);
                    has = true;
                }
                r = name == "rb_CcbGetFriction" ? ccb.GetFriction(has ? &other : nullptr) : ccb.GetBounciness(has ? &other : nullptr);
            }
            St0(c, r, want, got);
            return;
        }
        throw std::runtime_error("no port registered for golden '" + name + "'");
    }
};

golden::Register reg(new SuiteRigidBody);

} // namespace
