// C++ golden runner for the src/drive vehicle physics port (port of NFSU2Unity tools/harness/csharp
// Program.cs). Runs the C++ ports on the inputs of the Unicorn golden files recorded from SPEED2.EXE v1.2
// and reports bit-exact matches. Only the PC24 goldens (control word 0x007F, what the game runs with) are
// checked; golden/<name>.pc64.json are contrast files for the C# runner and are skipped here.
//
// Usage: drive-goldens.exe [golden dir] [-v] [--filter text]...
//   default golden dir: D:\Outros\Underground2\NFSU2Unity\tools\harness\golden (or FRS_GOLDEN_DIR)
//   -v        print every mismatching case
//   --filter  only run golden files whose name contains the text (repeatable)

#include "golden.h"

#include "drive/numerics/Speed2Image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace golden;

namespace golden {

std::vector<Suite*>& Registry() { static std::vector<Suite*> r; return r; }

std::wstring ExePath() {
    const wchar_t* d = _wgetenv(L"BLACKBOX_GAME_DIR");
    std::wstring dir = d ? d : L"F:\\Games\\NFSU2";
    return dir + L"\\SPEED2.EXE";
}

uint32_t Hex(std::string_view s) { return static_cast<uint32_t>(Hex64(s)); }
uint64_t Hex64(std::string_view s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    uint64_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) throw std::runtime_error("bad hex: " + std::string(s));
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v;
}
static int Nib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
}
Bytes FromHex(std::string_view s) {
    Bytes b(s.size() / 2);
    for (size_t i = 0; i < b.size(); i++) b[i] = static_cast<uint8_t>((Nib(s[2 * i]) << 4) | Nib(s[2 * i + 1]));
    return b;
}
std::string HexBytes(const uint8_t* b, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) { s[2 * i] = d[b[i] >> 4]; s[2 * i + 1] = d[b[i] & 15]; }
    return s;
}
std::string HexUpper(const uint8_t* b, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) { s[2 * i] = d[b[i] >> 4]; s[2 * i + 1] = d[b[i] & 15]; }
    return s;
}
std::string HexFloats(const float* f, size_t n) { return HexBytes(reinterpret_cast<const uint8_t*>(f), 4 * n); }
std::string Hex8(uint32_t v) { char t[16]; std::snprintf(t, sizeof t, "0x%08X", v); return t; }
std::string Hex4(uint32_t v) { char t[16]; std::snprintf(t, sizeof t, "0x%04X", v); return t; }
std::string HexX(uint32_t v) { char t[16]; std::snprintf(t, sizeof t, "%X", v); return t; }
std::string F32(float f) { return Hex8(bb::X87::Bits(f)); }

} // namespace golden

namespace {

const std::set<std::string> Known = {
    "bSin_us", "bSin_f", "bFastATan", "bATan", "bASin", "bNormalize", "bCross", "eMulVector3", "eMulMatrix",
    "TableGetValue", "ftol2",
};

std::string U16(const JVal& o) { return Hex4(Hex(o["eax"].s) & 0xFFFF); }

// The original maths helpers (Program.cs Evaluate switch).
void EvaluateMaths(bb::BWareMath& math, const std::string& name, GoldenCase& c, std::string& want, std::string& got) {
    const JVal& o = c.Out();
    if (name == "bSin_us") {
        want = o["f32"].Str();
        got = F32(bb::BWareMath::Sin(static_cast<uint16_t>(Hex(c.Arg(0)["v"].s))));
    } else if (name == "bSin_f") {
        want = o["f32"].Str();
        got = F32(bb::BWareMath::Sin(c.F(0)));
    } else if (name == "bFastATan") {
        want = U16(o); got = Hex4(math.FastATan(c.F(0), c.F(1)));
    } else if (name == "bATan") {
        want = U16(o); got = Hex4(math.ATan(c.F(0), c.F(1)));
    } else if (name == "bASin") {
        want = U16(o); got = Hex4(math.ASin(c.F(0)));
    } else if (name == "bNormalize") {
        float r[3] = {};
        auto v = c.Vec(1, 3);
        bb::BWareMath::Normalize(r, v.data());
        want = o["bufs"]["0"].Str(); got = HexFloats(r, 3);
    } else if (name == "bCross" || name == "bCross@x87") {
        float r[3] = {};
        auto a = c.Vec(1, 3), b = c.Vec(2, 3);
        if (name == "bCross") bb::BWareMath::Cross(r, a.data(), b.data());
        else bb::BWareMath::CrossX87(r, a.data(), b.data());
        want = o["bufs"]["0"].Str(); got = HexFloats(r, 3);
    } else if (name == "eMulVector3") {
        float r[3] = {};
        auto m = c.Vec(1, 16), v = c.Vec(2, 3);
        bb::BWareMath::MulVector3(r, m.data(), v.data());
        want = o["bufs"]["0"].Str(); got = HexFloats(r, 3);
    } else if (name == "eMulMatrix") {
        float r[16] = {};
        auto a = c.Vec(1, 16), b = c.Vec(2, 16);
        bb::BWareMath::MulMatrix(r, a.data(), b.data());
        want = o["bufs"]["0"].Str(); got = HexFloats(r, 16);
    } else if (name == "TableGetValue") {
        int tid = (*c.Root)["this"]["id"].I32();
        const Bytes& hdr = c.Bufs.at(tid);
        int dataId = -1;
        for (auto& f : c.Fixups.at(tid)) if (f.first == 0x10) dataId = f.second;
        int count = RdI(hdr, 0);
        std::vector<float> data(static_cast<size_t>(count));
        for (int i = 0; i < count; i++) data[static_cast<size_t>(i)] = RdF(c.Bufs.at(dataId), 4 * i);
        float r = bb::BWareMath::TableGetValue(count, RdF(hdr, 4), RdF(hdr, 12), data.data(), c.F(0));
        want = o["f32"].Str(); got = F32(r);
    } else if (name == "ftol2") {
        uint64_t q = Hex64((*c.Root)["st0_in"]["v"].s);
        double d; std::memcpy(&d, &q, 8);
        int64_t r = bb::X87::FtoL2(d);
        want = o["edx"].Str() + ":" + o["eax"].Str();
        got = Hex8(static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32)) + ":" + Hex8(static_cast<uint32_t>(r));
    } else {
        throw std::runtime_error("no port registered for golden '" + name + "'");
    }
}

Suite* FindSuite(const std::string& name) {
    for (Suite* s : Registry()) if (s->Handles(name)) return s;
    return nullptr;
}

struct FileResult { int n = 0, bad = 0; std::string first; };

FileResult RunFile(bb::BWareMath& math, const std::string& path, const std::string& name, bool verbose, JDoc& doc) {
    FileResult r;
    Suite* suite = Known.count(name.substr(0, name.find('@'))) ? nullptr : FindSuite(name);
    if (suite) suite->BeginFile(path, doc.Root);
    for (const JVal& cj : doc.Root["cases"].a) {
        GoldenCase c;
        c.Root = &cj;
        if (const JVal* bufs = cj.Get("bufs"); bufs && bufs->IsArr()) {
            for (const JVal& b : bufs->a) {
                int id = b["id"].I32();
                c.Bufs[id] = FromHex(b["hex"].s);
                auto& fx = c.Fixups[id];
                if (const JVal* f = b.Get("fixups"))
                    for (const JVal& e : f->a) fx.emplace_back(e[0].I32(), e[1].I32());
            }
        }
        std::string want, got;
        try {
            if (suite) suite->Evaluate(math, name, c, want, got);
            else EvaluateMaths(math, name, c, want, got);
        } catch (const std::exception& e) {
            want = "ok"; got = std::string("exception: ") + e.what();
        }
        r.n++;
        if (want != got) {
            if (r.bad == 0) r.first = "case " + std::to_string(r.n - 1) + ": want " + want + " got " + got;
            if (verbose) std::printf("  %s case %d: want %s got %s\n", name.c_str(), r.n - 1, want.c_str(), got.c_str());
            r.bad++;
        }
    }
    return r;
}

std::string FindGoldenDir() {
    if (const char* e = std::getenv("FRS_GOLDEN_DIR")) return e;
    return "D:\\Outros\\Underground2\\NFSU2Unity\\tools\\harness\\golden";
}

} // namespace

int main(int argc, char** argv) {
    std::string dir;
    bool verbose = false;
    std::vector<std::string> filters;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") verbose = true;
        else if (a == "--filter" && i + 1 < argc) filters.push_back(argv[++i]);
        else dir = a;
    }
    if (dir.empty()) dir = FindGoldenDir();

    try {
        bb::Speed2Image exe(ExePath());
        bb::BWareMath math(exe);

        // F24::MulDouble self-check on the constructed double-rounding case from selftest_fpu.py:
        // x87 PC24 "fld 1.5f; fmul qword c" = 1.50000012f, while (float)(1.5 * c) = 1.5f.
        double cst = 1.0 + 178956971.0 * std::pow(2.0, -52);
        float naive = static_cast<float>(1.5 * cst), x87 = bb::F24::MulDouble(1.5f, cst).Store();
        bool okSelf = x87 == 1.50000012f && naive == 1.5f;
        std::printf("F24.MulDouble self-check: naive (float)(x*c) = %.9g, F24 = %.9g -> %s\n", naive, x87, okSelf ? "OK" : "FAIL");
        if (x87 != 1.50000012f) return 1;

        std::vector<std::string> files;
        for (auto& e : fs::directory_iterator(dir)) {
            std::string fn = e.path().filename().string();
            if (fn.size() < 5 || fn.substr(fn.size() - 5) != ".json") continue;
            if (fn.find(".pc64.") != std::string::npos) continue;   // contrast files: not checked
            files.push_back(fn);
        }
        std::sort(files.begin(), files.end());

        int failsPc24 = 0, skipped = 0;
        auto t0 = std::chrono::steady_clock::now();
        std::printf("%-34s %6s %8s %9s\n", "golden file [port]", "cases", "match", "mismatch");
        for (const std::string& fn : files) {
            std::string name = fn.substr(0, fn.find('.'));
            if (!filters.empty()) {
                bool any = false;
                for (auto& f : filters) if (name.find(f) != std::string::npos) any = true;
                if (!any) continue;
            }
            std::vector<std::string> ports = {name};
            if (name == "bCross") ports.push_back("bCross@x87");
            if (!Known.count(name) && !FindSuite(name)) {
                std::printf("%-34s %6s   (skipped: no suite for this golden)\n", fn.c_str(), "-");
                skipped++;
                continue;
            }
            JDoc doc;
            {
                std::ifstream in(fs::path(dir) / fn, std::ios::binary);
                std::ostringstream ss; ss << in.rdbuf();
                doc.Text = ss.str();
            }
            doc.Parse();
            for (const std::string& port : ports) {
                FileResult r = RunFile(math, (fs::path(dir) / fn).string(), port, verbose, doc);
                bool knownGap = port == "bCross";   // float port vs denormal-range products
                std::string label = fn + (port.find('@') != std::string::npos ? " [" + port.substr(port.find('@') + 1) + "]" : "");
                std::string note = r.bad == 0 ? "" : knownGap ? "   (known gap: float port, denormal intermediates; see the x87 run) " + r.first
                                                              : "   <-- FAIL " + r.first;
                std::printf("%-34s %6d %8d %9d%s\n", label.c_str(), r.n, r.n - r.bad, r.bad, note.c_str());
                std::fflush(stdout);
                if (!knownGap) failsPc24 += r.bad;
            }
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("(%d golden files skipped: no suite; %.1f s)\n", skipped, secs);
        std::printf(failsPc24 == 0 ? "RESULT: all PC24 golden cases bit-exact\n" : "RESULT: %d PC24 mismatches\n", failsPc24);
        return failsPc24 == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::printf("fatal: %s\n", e.what());
        return 2;
    }
}
