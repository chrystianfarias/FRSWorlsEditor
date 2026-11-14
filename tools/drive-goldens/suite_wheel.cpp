// Golden suite for the wheel / tyre / suspension / steering slice (golden/wheel_*.json, gen_golden_wheel.py).
// Port of NFSU2Unity tools/harness/csharp/SuiteWheel.cs. Each case names its buffers (case.roles); the objects
// are rebuilt from the input bytes, the port runs, and its fields are written back over a copy of the input
// bytes, so every byte of every object is compared with the state the exe left. Globals of the case (case.mem)
// go to WheelTweaks; world/RigidBody callees are mocked (case.mock) and the arguments the port passes are
// compared with what the exe passed (out.capture).

#include "golden.h"

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/Graph.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/Wheel.h"

#include <algorithm>
#include <memory>

using namespace golden;
using namespace bb;

namespace {

struct Ctx {
    GoldenCase* C = nullptr;
    std::map<std::string, int> Roles;
    std::map<int, Bytes> Got;
    const Bytes& In(const std::string& role) const { return C->Bufs.at(Roles.at(role)); }
    Bytes& Out(const std::string& role) { return Got.at(Roles.at(role)); }
    bool Has(const std::string& role) const { return Roles.count(role) != 0; }
};

WheelInfo ReadWheelInfo(const Bytes& b, int o = 0) {
    WheelInfo wi;
    wi.Radius = RdF(b, o + 0x10);
    wi.Index = RdI(b, o + 0x18);
    Rd(b, o, wi.Pos, 3);
    return wi;
}

TireInfo ReadTire(const Bytes& b) {
    TireInfo t;
    t.MaxSlipAngle = RdF(b, 0); t.LoadRef = RdF(b, 4); t.LateralScale = RdF(b, 8); t.MuLong = RdF(b, 0xC); t.MuLat = RdF(b, 0x10);
    return t;
}

SuspensionInfo ReadSuspInfo(const Bytes& b) {
    SuspensionInfo s;
    s.Progression = RdF(b, 0); s.SpringRate = RdF(b, 4); s.BumpDamping = RdF(b, 8); s.ReboundDamping = RdF(b, 0xC);
    s.AntiRoll = RdF(b, 0x10); s.MaxCompression = RdF(b, 0x14); s.MinCompression = RdF(b, 0x18);
    return s;
}

class MockWorld : public IWheelWorld {
public:
    float Elev = 0, Noise = 0; int TerrainV = 0; float Normal[3] = {}; bool Valid = false;
    uint8_t Cap[0x40] = {};

    float GetElevation(void* topology, const float* point, int& terrain, float* normal, bool& valid) override {
        for (int i = 0; i < 3; i++) WrF(Cap, 4 * i, point[i]);
        WrI(Cap, 12, static_cast<int>(reinterpret_cast<intptr_t>(topology)));
        terrain = TerrainV; for (int i = 0; i < 3; i++) normal[i] = Normal[i]; valid = Valid;
        return Elev;
    }
    float GetRoadNoise(const float* pointXY, int terrain) override {
        WrF(Cap, 16, pointXY[0]); WrF(Cap, 20, pointXY[1]); WrI(Cap, 24, terrain);
        return Noise;
    }
};

std::string Diff(const std::string& a, const std::string& b, std::string& bOut) {
    auto split = [](const std::string& s) {
        std::vector<std::string> r; size_t p = 0;
        for (;;) { size_t q = s.find('|', p); r.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
        return r;
    };
    auto pa = split(a), pb = split(b);
    for (size_t i = 0; i < std::min(pa.size(), pb.size()); i++)
        if (pa[i] != pb[i]) {
            size_t eqp = pa[i].find('='); int eq = eqp == std::string::npos ? 0 : static_cast<int>(eqp) + 1;
            int k = eq;
            while (k < static_cast<int>(std::min(pa[i].size(), pb[i].size())) && pa[i][k] == pb[i][k]) k++;
            int off = (k - eq) / 8 * 4;
            int s = eq + 2 * off;
            std::string head = (eq > 0 ? pa[i].substr(0, eq - 1) + "+0x" + HexX(static_cast<uint32_t>(off)) : std::string("ret")) + ":";
            auto sub = [&](const std::string& x) {
                int st = std::min(s, static_cast<int>(x.size()));
                int n = std::max(0, std::min(16, static_cast<int>(x.size()) - s));
                return x.substr(static_cast<size_t>(st), static_cast<size_t>(n));
            };
            bOut = head + sub(pb[i]);
            return head + sub(pa[i]);
        }
    bOut = b; return a;
}

class SuiteWheel : public Suite {
    std::unique_ptr<Speed2Image> exe_;
    std::unique_ptr<PhysicsEnvironment> env_;
    WheelTweaks defaults_;

public:
    bool Handles(const std::string& n) const override { return n.rfind("wheel_", 0) == 0; }

    void Evaluate(BWareMath& math, const std::string& name, GoldenCase& c, std::string& want, std::string& got) override {
        if (!env_) {
            exe_ = std::make_unique<Speed2Image>(ExePath());
            env_ = std::make_unique<PhysicsEnvironment>(*exe_, &math);
            defaults_ = WheelTweaks::Load(*exe_);
        }
        PhysicsEnvironment& env = *env_;
        env.Tweaks = defaults_;
        env.World = World();
        env.World.UglyTimestepHack = X87::Bits(0x3F800000u);
        // ApplyMem
        if (const JVal* mem = c.Root->Get("mem"))
            for (const JVal& e : mem->a) {
                uint32_t va = Hex(e[0].s);
                Bytes d = FromHex(e[1].s);
                switch (va) {
                case 0x7FC7F0: env.Tweaks.UseNewTireForces = RdI(d, 0); break;
                case 0x7FC7FC: env.Tweaks.WheelForceFrame = RdI(d, 0); break;
                case 0x7FC800: env.World.UglyTimestepHack = RdF(d, 0); break;
                case 0x86B36C: env.World.NoWheels = RdI(d, 0); break;
                case 0x89E7D8: env.World.Flags.DragByte = d[0]; break;
                case 0x890108: env.World.Flags.TweakIsDragRace = RdI(d, 0); break;
                default: throw std::runtime_error("global " + Hex8(va));
                }
            }
        MockWorld mock;
        if (const JVal* j = c.Root->Get("mock"); j && j->IsObj() && j->Get("elev")) {
            mock.Elev = static_cast<float>((*j)["elev"].Dbl());
            mock.Noise = static_cast<float>((*j)["noise"].Dbl());
            mock.TerrainV = (*j)["terrain"].I32();
            mock.Valid = (*j)["valid"].I32() != 0;
            int k = 0;
            for (const JVal& e : (*j)["normal"].a) mock.Normal[k++] = static_cast<float>(e.Dbl());
        }
        env.Terrain = &mock;

        Ctx x; x.C = &c;
        const JVal& roles = (*c.Root)["roles"];
        for (size_t i = 0; i < roles.k.size(); i++) x.Roles[std::string(roles.k[i])] = roles.a[i].I32();
        for (auto& kv : c.Bufs) x.Got[kv.first] = kv.second;
        const JVal& o = c.Out();
        std::string wantRet, gotRet;
        bool cmpCapture = false;
        int capLen = 0;

        // Objects the wheel/car state point to (kept alive for the case).
        CarPhysicsInfo cpi;
        WheelInfo wi; TireInfo ti; ControlInfo ci;
        std::unique_ptr<CarState> cs;
        std::unique_ptr<Wheel> w;
        if (x.Has("cs")) {
            const Bytes& b = x.In("cs");
            int fxId = -1;
            for (auto& f : c.Fixups.at(x.Roles["cs"])) if (f.first == 0) { fxId = f.second; break; }
            cpi.DriveTrain.RearTorqueSplit = fxId >= 0 ? RdF(c.Bufs.at(fxId), 0x1C0) : std::numeric_limits<float>::quiet_NaN();
            cs = std::make_unique<CarState>();
            cs->Cpi = &cpi;
            cs->Topology = reinterpret_cast<void*>(static_cast<intptr_t>(RdI(b, 4)));
            cs->ForwardSpeed = RdF(b, 0x3EC); cs->PilotFlag = RdI(b, 0x424); cs->WheelsOnGround = RdI(b, 0x3E4);
            for (int i = 0; i < 4; i++) {
                cs->WheelOnGround[i] = b[static_cast<size_t>(0x350 + i)];
                cs->WheelTerrain[i] = RdI(b, 0x354 + 4 * i); cs->MaxWheelTerrain[i] = RdI(b, 0x364 + 4 * i);
            }
        }
        if (x.Has("wheel")) {
            const Bytes& b = x.In("wheel");
            wi = ReadWheelInfo(x.In("wi")); ti = ReadTire(x.In("ti"));
            ci.BrakeTorqueScale = RdF(x.In("ci"), 4); ci.EBrakeTorqueScale = RdF(x.In("ci"), 8);
            w = std::make_unique<Wheel>(&env, &wi, &ti, &ci, cs.get());
            w->Angle = RdF(b, 0x24); w->Omega = RdF(b, 0x28); w->Inertia = RdF(b, 0x2C);
            Rd(b, 0x30, w->Spin, 16); Rd(b, 0x70, w->World, 16); Rd(b, 0xB0, w->Local, 16); Rd(b, 0xF0, w->ContactVel, 3);
            w->LastSlip = RdF(b, 0x100); w->LastLatVel = RdF(b, 0x104); w->Torque = RdF(b, 0x108);
            w->BrakeLocked = RdI(b, 0x10C); w->RollState = RdI(b, 0x110); w->Terrain = RdI(b, 0x114); w->SlipVel = RdF(b, 0x118);
            w->OnGround = b[0x11C]; w->NetTorqueFilt = RdF(b, 0x120); w->Fx = RdF(b, 0x124); w->Fy = RdF(b, 0x128);
            Rd(b, 0x130, w->ForceWorld, 3); Rd(b, 0x140, w->ContactPt, 3); Rd(b, 0x150, w->GroundNormal, 3);
            w->Sliding = b[0x160]; w->Load = RdF(b, 0x164); w->RoadNoise = RdF(b, 0x168); w->SlipCopy = RdF(b, 0x16C);
            w->GripUsage = RdF(b, 0x170); w->FlatDrag = RdF(b, 0x174); w->Brake = RdF(b, 0x178); w->EBrake = RdF(b, 0x17C);
            w->GripScale = RdF(b, 0x180); w->GripScale2 = RdF(b, 0x184); w->LongGripScale = RdF(b, 0x188); w->RideOffset = RdF(b, 0x18C);
            w->LongForceScale = RdF(b, 0x190); w->LatForceScale = RdF(b, 0x194);
        }

        // Suspension-case objects (kept alive until written back).
        std::unique_ptr<RigidBody> rb;
        std::unique_ptr<TwoWheelAckermanSteering> st;
        SuspensionInfo si, siPartner;
        std::unique_ptr<Suspension> susp, partner;
        WheelInfo wis[4];

        auto readSusp = [&](const Bytes& b, Wheel* wh, RigidBody* r, TwoWheelAckermanSteering* s, const SuspensionInfo* inf) {
            auto p = std::make_unique<Suspension>(RdI(b, 0xAC), r, wh, s, inf);
            p->Compression = RdF(b, 0x1C); p->CompressionRate = RdF(b, 0x20); p->SpringForce = RdF(b, 0x24);
            Rd(b, 0x30, p->WheelPosLocal, 4); Rd(b, 0x40, p->LocalNoSteer, 16); p->Touching = RdI(b, 0x80); p->Preload = RdF(b, 0x88);
            Rd(b, 0x90, p->Attach, 3); p->ExchangeRet = RdF(b, 0xB0);
            return p;
        };

        if (name == "wheel_GraphGetValue") {
            const Bytes& g = x.In("graph"); const Bytes& pts = x.In("pts");
            std::vector<float> pf(pts.size() / 4);
            for (size_t i = 0; i < pf.size(); i++) pf[i] = RdF(pts, static_cast<int>(4 * i));
            Graph gr(pf, RdI(g, 4));
            wantRet = o["f32"].Str(); gotRet = F32(gr.GetValue(c.F(0)).Store());
        } else if (name == "wheel_SetWheelTouchingGround") {
            cs->SetWheelTouchingGround(static_cast<int>(c.U(0)), static_cast<uint8_t>(c.U(1)));
        } else if (name == "wheel_SetWheelTerrain") {
            cs->SetWheelTerrain(static_cast<int>(c.U(0)), static_cast<int>(c.U(1)));
        } else if (name == "wheel_GetPilotFactor") {
            wantRet = o["f32"].Str(); gotRet = F32(w->GetPilotFactor().Store());
        } else if (name == "wheel_CheckForBrakeLock") {
            w->CheckForBrakeLock(c.F(0));
        } else if (name == "wheel_IsDriveWheel") {
            wantRet = o["eax"].Str();
            gotRet = Hex8(cs->IsDriveWheel(static_cast<int>(c.U(0))) ? 1u : 0u);
        } else if (name == "wheel_HorizGroundForce") {
            float r[3] = {};
            w->GetHorizontalGroundForceInCarFrame(r);
            Wr(x.Out("out"), 0, r, 3);
        } else if (name == "wheel_GetLateralForce") {
            wantRet = o["f32"].Str(); gotRet = F32(w->GetLateralForce(c.F(0), c.F(1)).Store());
        } else if (name == "wheel_GetDerivative") {
            float d[2] = {RdF(x.In("d"), 0), RdF(x.In("d"), 4)};
            w->GetDerivative(d);
            Wr(x.Out("d"), 0, d, 2);
        } else if (name == "wheel_ComputeAux") {
            w->ComputeAuxiliaries();
        } else if (name == "wheel_ExchangeZForces") {
            wantRet = o["f32"].Str();
            gotRet = F32(w->ExchangeZForces(static_cast<int>(c.U(0)), c.F(1)));
        } else if (name == "wheel_GuessGroundForce") {
            w->GuessGroundForce(static_cast<int>(c.U(1)));
        } else if (name == "wheel_FinalizeGroundForce") {
            w->FinalizeGroundForce();
        } else if (name == "wheel_CheckTerrain") {
            float n[3]; Rd(x.In("nrm"), 0, n, 3);
            int terr = RdI(x.In("terr"), 0);
            float pt[3]; Rd(x.In("pt"), 0, pt, 3);
            float mat[16]; Rd(x.In("mat"), 0, mat, 16);
            wantRet = o["f32"].Str();
            gotRet = F32(w->CheckTerrain(n, terr, pt, mat).Store());
            Wr(x.Out("nrm"), 0, n, 3);
            WrI(x.Out("terr"), 0, terr);
            cmpCapture = true; capLen = 28;
        } else if (name == "wheel_SetSteering") {
            const Bytes& sb = x.In("steer"); const Bytes& cp = x.In("cpi");
            for (int i = 0; i < 4; i++) wis[i] = ReadWheelInfo(cp, 0x10 + 0x30 * i);
            TwoWheelAckermanSteering s(wis);
            s.Angle = RdU16(sb.data(), 0x90);
            Rd(sb, 0x10, s.Mat[0], 16); Rd(sb, 0x50, s.Mat[1], 16);
            s.SetSteering(static_cast<uint16_t>(c.U(0)));
            Bytes& ob = x.Out("steer");
            Wr(ob, 0x10, s.Mat[0], 16); Wr(ob, 0x50, s.Mat[1], 16);
            WrU16(ob.data(), 0x90, s.Angle);
        } else if (name == "wheel_SuspCalibrate" || name == "wheel_SuspComputeAux" || name == "wheel_GroundPenalty") {
            rb = std::make_unique<RigidBody>(env.Rb);
            rb->ReadFrom(x.In("rb").data());
            if (x.Has("steer")) {
                st = std::make_unique<TwoWheelAckermanSteering>(nullptr);
                Rd(x.In("mats"), 0, st->Mat[0], 16); Rd(x.In("mats"), 64, st->Mat[1], 16);
            }
            si = ReadSuspInfo(x.In("si"));
            susp = readSusp(x.In("susp"), w.get(), rb.get(), st.get(), &si);
            if (x.Has("partner")) { partner = readSusp(x.In("partner"), nullptr, rb.get(), nullptr, nullptr); susp->Partner = partner.get(); }
            uint8_t* cap = mock.Cap;
            rb->ApplyImpulseTrace = [cap](const float* j, const float* r) {
                for (int i = 0; i < 3; i++) { WrF(cap, 32 + 4 * i, j[i]); WrF(cap, 44 + 4 * i, r[i]); }
            };
            if (name == "wheel_SuspCalibrate") susp->Calibrate(c.F(0), c.F(1));
            else if (name == "wheel_SuspComputeAux") { susp->ComputeAuxiliaries(); cmpCapture = true; capLen = 28; }
            else {
                wantRet = o["f32"].Str(); gotRet = F32(susp->ApplyGroundPenaltyImpulse().Store());
                cmpCapture = true; capLen = 56;
            }
            Bytes& b = x.Out("susp");
            WrF(b, 0x1C, susp->Compression); WrF(b, 0x20, susp->CompressionRate); WrF(b, 0x24, susp->SpringForce);
            Wr(b, 0x30, susp->WheelPosLocal, 4); Wr(b, 0x40, susp->LocalNoSteer, 16); WrI(b, 0x80, susp->Touching); WrF(b, 0x88, susp->Preload);
            Wr(b, 0x90, susp->Attach, 3); WrI(b, 0xAC, susp->Index); WrF(b, 0xB0, susp->ExchangeRet);
        } else {
            throw std::runtime_error("unsupported " + name);
        }

        if (w) {
            Bytes& b = x.Out("wheel");
            WrF(b, 0x24, w->Angle); WrF(b, 0x28, w->Omega); WrF(b, 0x2C, w->Inertia);
            Wr(b, 0x30, w->Spin, 16); Wr(b, 0x70, w->World, 16); Wr(b, 0xB0, w->Local, 16); Wr(b, 0xF0, w->ContactVel, 3);
            WrF(b, 0x100, w->LastSlip); WrF(b, 0x104, w->LastLatVel); WrF(b, 0x108, w->Torque);
            WrI(b, 0x10C, w->BrakeLocked); WrI(b, 0x110, w->RollState); WrI(b, 0x114, w->Terrain); WrF(b, 0x118, w->SlipVel);
            b[0x11C] = w->OnGround; WrF(b, 0x120, w->NetTorqueFilt); WrF(b, 0x124, w->Fx); WrF(b, 0x128, w->Fy);
            Wr(b, 0x130, w->ForceWorld, 3); Wr(b, 0x140, w->ContactPt, 3); Wr(b, 0x150, w->GroundNormal, 3);
            b[0x160] = w->Sliding; WrF(b, 0x164, w->Load); WrF(b, 0x168, w->RoadNoise); WrF(b, 0x16C, w->SlipCopy);
            WrF(b, 0x170, w->GripUsage); WrF(b, 0x174, w->FlatDrag); WrF(b, 0x178, w->Brake); WrF(b, 0x17C, w->EBrake);
            WrF(b, 0x180, w->GripScale); WrF(b, 0x184, w->GripScale2); WrF(b, 0x188, w->LongGripScale); WrF(b, 0x18C, w->RideOffset);
            WrF(b, 0x190, w->LongForceScale); WrF(b, 0x194, w->LatForceScale);
        }
        if (cs) {
            Bytes& b = x.Out("cs");
            WrI(b, 0x3E4, cs->WheelsOnGround);
            for (int i = 0; i < 4; i++) {
                b[static_cast<size_t>(0x350 + i)] = cs->WheelOnGround[i];
                WrI(b, 0x354 + 4 * i, cs->WheelTerrain[i]); WrI(b, 0x364 + 4 * i, cs->MaxWheelTerrain[i]);
            }
        }
        env.Terrain = nullptr;
        // Pointer fields hold VM addresses patched in by the loader (fixups): take them from the recorded output.
        for (auto& kv : c.Fixups) {
            Bytes rec = FromHex(c.OutBuf(kv.first));
            for (auto& f : kv.second) std::memcpy(x.Got[kv.first].data() + f.first, rec.data() + f.first, 4);
        }

        std::string sw = wantRet, sg = gotRet;
        for (auto& kv : x.Roles) {   // std::map: ordinal key order (as OrderBy on ASCII role names)
            sw += "|" + kv.first + "=" + c.OutBuf(kv.second);
            sg += "|" + kv.first + "=" + HexBytes(x.Got[kv.second]);
        }
        if (cmpCapture) {
            sw += "|cap=" + o["capture"].Str().substr(0, static_cast<size_t>(2 * capLen));
            sg += "|cap=" + HexBytes(mock.Cap, sizeof mock.Cap).substr(0, static_cast<size_t>(2 * capLen));
        }
        want = sw; got = sg;
        if (want != got) { std::string g2; want = Diff(want, got, g2); got = g2; }
    }
};

static Register reg(new SuiteWheel);

} // namespace
