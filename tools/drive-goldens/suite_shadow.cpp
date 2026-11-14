// Golden suite for the car shadow helpers (golden/shadow_*.json, gen_golden_shadow.py):
// sub_60D350 (light vector + 1/Lz) against CarShadowU2::LightVector and sub_60D1B0 (hull corner rounding) against
// CarShadowU2::RoundCorners. Compared bit for bit.
// Port of NFSU2Unity tools/harness/csharp/SuiteShadow.cs.

#include "golden.h"

#include "drive/cars/CarShadowU2.h"

using namespace golden;

namespace {

class SuiteShadow : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("shadow_", 0) == 0; }

    void Evaluate(bb::BWareMath& math, const std::string& name, GoldenCase& c, std::string& want,
                  std::string& got) override {
        const JVal& r = *c.Root;
        if (name.rfind("shadow_Light", 0) == 0) {
            const std::vector<float> pos = Fl(r["pos"].s);
            float l[3] = {};
            const float inv = bb::CarShadowU2::LightVector(pos.data(), l, &math);
            const std::vector<float> wl = Fl(r["l"].s), wi = Fl(r["inv"].s);
            want = HexList(wl.data(), wl.size()) + HexList(wi.data(), wi.size());
            got = HexList(l, 3) + HexList(&inv, 1);
            return;
        }
        const int n = r["n"].I32();
        const std::vector<float> p = Fl(r["pts"].s);
        std::vector<std::vector<float>> pts;
        for (int i = 0; i < n; i++) pts.push_back({ p[(size_t)(3 * i)], p[(size_t)(3 * i + 1)], p[(size_t)(3 * i + 2)] });
        const auto o = bb::CarShadowU2::RoundCorners(pts);
        std::vector<float> flat;
        for (const auto& q : o) flat.insert(flat.end(), q.begin(), q.end());
        const std::vector<float> wo = Fl(r["out"].s);
        want = std::to_string(r["ret"].I32()) + ":" + HexList(wo.data(), wo.size());
        got = std::to_string(3 * n) + ":" + HexList(flat.data(), flat.size());
    }

private:
    static std::vector<float> Fl(std::string_view hex) {
        const Bytes b = FromHex(hex);
        std::vector<float> f(b.size() / 4);
        for (size_t i = 0; i < f.size(); i++) f[i] = RdF(b, (int)(4 * i));
        return f;
    }

    static std::string HexList(const float* f, size_t n) {
        std::string s;
        char t[16];
        for (size_t i = 0; i < n; i++) {
            std::snprintf(t, sizeof t, "%08X,", bb::X87::Bits(f[i]));
            s += t;
        }
        return s;
    }
};

Register reg(new SuiteShadow);

} // namespace
