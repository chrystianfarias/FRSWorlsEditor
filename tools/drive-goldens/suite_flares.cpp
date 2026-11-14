// Golden suite for the car light flares (golden/flare_*.json, gen_golden_flares.py): eRenderLightFlare 0x490920 in
// normal mode against CarLightFlaresU2::Render, with the view state (camera, ViewProj, planes, focal) recorded from
// the game's own sub_5C7EC0 and the flare table patched as RenderFlares 0x61AE90 does (CarLightFlaresU2::TableForView).
// Every polygon (4 positions, colour, texture slot) and the polygon count are compared bit for bit.
// Port of NFSU2Unity tools/harness/csharp/SuiteFlares.cs.

#include "golden.h"

#include "drive/cars/CarLightFlaresU2.h"

using namespace golden;

namespace {

class SuiteFlares : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("flare_", 0) == 0; }

    void Evaluate(bb::BWareMath&, const std::string&, GoldenCase& c, std::string& want, std::string& got) override {
        const JVal& r = *c.Root;
        bb::FlareViewU2 v;
        v.ViewId = r["view"].I32();
        v.Width = r["w"].I32();
        v.Height = r["h"].I32();
        v.Focal = RdF(FromHex(r["focal"].s), 0);
        const Bytes cam = FromHex(r["cam"].s);
        Rd(cam, 0, v.CamMatrix, 16);
        Rd(cam, 0x40, v.CamPos, 3);
        Rd(cam, 0x50, v.CamDir, 3);
        Rd(FromHex(r["viewproj"].s), 0, v.ViewProj, 16);
        Rd(FromHex(r["planes"].s), 0, v.Planes, 24);
        const Bytes fb = FromHex(r["flare"].s);
        bb::LightFlareU2 f;
        f.NameHash = RdU(fb, 8);
        f.ColourTint = RdU(fb, 0xC);
        f.X = RdF(fb, 0x10); f.Y = RdF(fb, 0x14); f.Z = RdF(fb, 0x18);
        f.ReflectPosZ = RdF(fb, 0x1C);
        f.DirX = RdF(fb, 0x20); f.DirY = RdF(fb, 0x24); f.DirZ = RdF(fb, 0x28);
        f.Type = static_cast<int8_t>(fb[0x2C]);
        f.Flags = static_cast<int8_t>(fb[0x2D]);
        float mat[16];
        Rd(FromHex(r["mat"].s), 0, mat, 16);
        const float I = bb::X87::Bits(Hex(r["I"].s));
        const uint32_t colour = Hex(r["colour"].s);
        const bb::FlareTableU2 table = bb::CarLightFlaresU2::TableForView(v.ViewId, r["race"].b);
        bb::FlarePolyU2 polys[4];
        const int n = bb::CarLightFlaresU2::Render(v, f, mat, I, colour, table, polys, 4);

        const JVal& exp = r["polys"];
        std::string w = std::to_string(exp.Size()) + ":";
        for (size_t i = 0; i < exp.Size(); i++) {
            const JVal& p = exp[i];
            const Bytes xyz = FromHex(p["xyz"].s);
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < 3; j++) w += HexX8(RdU(xyz, 16 * k + 4 * j)) + ",";
            w += HexX8(RdU(FromHex(p["col"].s), 0));
            w += "/" + std::to_string(p["tex"].I32()) + ";";
        }
        std::string g = std::to_string(n) + ":";
        for (int i = 0; i < n && i < 4; i++) {
            const bb::FlarePolyU2& q = polys[i];
            const float xyz[12] = { q.X0, q.Y0, q.Z0, q.X1, q.Y1, q.Z1, q.X2, q.Y2, q.Z2, q.X3, q.Y3, q.Z3 };
            for (float x : xyz) g += HexX8(bb::X87::Bits(x)) + ",";
            g += HexX8(q.Colour) + "/" + std::to_string(q.TexIndex) + ";";
        }
        want = w;
        got = g;
    }

private:
    static std::string HexX8(uint32_t v) { char t[16]; std::snprintf(t, sizeof t, "%08X", v); return t; }
};

Register reg(new SuiteFlares);

} // namespace
