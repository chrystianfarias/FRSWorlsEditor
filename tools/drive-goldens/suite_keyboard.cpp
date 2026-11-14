// Golden suite for the keyboard / digital-button input layer (goldens kbd_*, gen_golden_keyboard.py):
// mapping functions 0x571B10/0x571BD0/0x571C10/0x571C70/0x571CC0 and sub_57B0B0 sequences.
// Port of NFSU2Unity tools/harness/csharp/SuiteKeyboard.cs.

#include "golden.h"

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/input/KeyboardAxis.h"

#include <memory>

using namespace golden;

namespace {

class SuiteKeyboard : public Suite {
public:
    bool Handles(const std::string& n) const override { return n.rfind("kbd_", 0) == 0; }

    void Evaluate(bb::BWareMath&, const std::string& name, GoldenCase& c, std::string& want, std::string& got) override {
        const JVal& inp = (*c.Root)["in"];
        if (name == "kbd_map") Map(inp, (*c.Root)["out"], want, got);
        else if (name == "kbd_poll") Poll(inp, want, got);
        else throw std::runtime_error("unsupported " + name);
    }

private:
    std::unique_ptr<bb::JoyMapTable> table_;
    bb::JoyMapTable& Table() {
        if (!table_) { bb::Speed2Image exe(ExePath()); table_ = std::make_unique<bb::JoyMapTable>(exe); }
        return *table_;
    }

    static uint32_t U(const JVal& o, const char* k) { return Hex(o[k].s); }
    static int I(const JVal& o, const char* k) { return static_cast<int>(U(o, k)); }
    static std::string H(int v) { return Hex8(static_cast<uint32_t>(v)); }

    static bb::JoyState State(const Bytes& b) {
        bb::JoyState s;
        s.W0 = RdU(b, 0); s.W1 = RdU(b, 4); s.W2 = RdU(b, 8); s.W3 = RdU(b, 12);
        return s;
    }

    void Map(const JVal& inp, const JVal& o, std::string& want, std::string& got) {
        Bytes raw = FromHex(inp["desc"].s);
        uint32_t va = U(inp, "descVa");
        bb::JoyMapEntry* d = nullptr;
        std::unique_ptr<bb::JoyMapEntry> own;
        std::string tableCheck;
        if (va >= bb::JoyMapTable::TableVa && va < bb::JoyMapTable::TableVa + 0x20 * 100) {
            d = Table().AtVa(va);
            if (d == nullptr || raw.size() != 0x20 || std::memcmp(d->Raw, raw.data(), 0x20) != 0) tableCheck = " TABLE-MISMATCH";
        } else {
            own = bb::JoyMapEntry::FromBytes(raw.data());
            d = own.get();
        }
        bb::JoyState cur = State(FromHex(inp["cur"].s)), prev = State(FromHex(inp["prev"].s));
        bb::JoyNode n;
        n.Event = I(inp, "ev"); n.Desc = d; n.Last = I(inp, "last"); n.Force = I(inp, "force");
        uint32_t ret;
        if (inp["call"].s == "test") ret = bb::JoyMapFunctions::Test(*d, cur, prev);
        else ret = bb::JoyMapFunctions::Map(n, cur, prev, *d, I(inp, "devcount"));
        want = "ret=" + o["ret"].Str() + " last=" + o["last"].Str() + " force=" + o["force"].Str();
        got = "ret=" + Hex8(ret) + " last=" + H(n.Last) + " force=" + H(n.Force) + tableCheck;
    }

    void Poll(const JVal& inp, std::string& want, std::string& got) {
        const JVal& ev = inp["events"];
        const JVal& ds = inp["descs"];
        int devcount = I(inp, "devcount");
        bb::JoyPort port;
        port.Port = I(inp, "port"); port.DeviceCount = devcount; port.PendingCount = devcount; port.Debounce = 0;
        for (size_t i = 0; i < ev.Size(); i++) {
            bb::JoyNode n;
            n.Event = static_cast<int>(Hex(ev.a[i].s));
            uint32_t dv = Hex(ds.a[i].s);
            n.Desc = dv == 0 ? nullptr : Table().AtVa(dv);
            n.Callback = 0x00400000 + n.Event; n.Param = 0x1000 + static_cast<int>(i); n.Last = 0; n.Force = 1;
            port.Nodes.push_back(n);
        }
        bb::KeyboardDevice kb;
        bb::JoyState cur = bb::JoyState::Released(), prev;
        std::vector<bb::JoyEventRecord> q;
        int k = 0;
        for (const JVal& f : inp["frames"].a) {
            uint64_t mask = Hex64(f["mask"].s);
            for (int b = 0; b < 64; b++) kb.KeyEvent(b, ((mask >> b) & 1) == 0);
            bb::JoyState::BeginFrame(cur, prev, kb.Mask);
            q.clear();
            int ret = port.Poll(I(f, "dc"), cur, prev, q);
            const JVal& o = f["out"];
            std::string w = "ret=" + o["ret"].Str() + " q=";
            for (size_t i = 0; i < o["q"].Size(); i++) { if (i) w += ","; w += o["q"].a[i].Str(); }
            w += " nodes=";
            for (size_t i = 0; i < o["nodes"].Size(); i++) { if (i) w += ","; w += o["nodes"].a[i].Str(); }
            w += " joy=";
            for (size_t i = 0; i < o["joy"].Size(); i++) { if (i) w += ","; w += o["joy"].a[i].Str(); }
            std::string g = "ret=" + H(ret) + " q=";
            for (size_t i = 0; i < q.size(); i++) { if (i) g += ","; g += q[i].ToString(); }
            g += " nodes=";
            for (size_t i = 0; i < port.Nodes.size(); i++) { if (i) g += ","; g += H(port.Nodes[i].Last) + "/" + H(port.Nodes[i].Force); }
            g += " joy=" + H(port.DeviceCount) + "," + H(port.PendingCount) + "," + H(port.Debounce);
            if (w != g) { want = "frame " + std::to_string(k) + ": " + w; got = "frame " + std::to_string(k) + ": " + g; return; }
            k++;
        }
        want = got = "ok";
    }
};

Register reg(new SuiteKeyboard);

} // namespace
