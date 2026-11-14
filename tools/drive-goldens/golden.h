#pragma once

// Shared types of the C++ golden runner (port of NFSU2Unity tools/harness/csharp Suites.cs).
// Each suite file registers one Suite object; main.cpp dispatches every golden file to the suite that
// handles its name.

#include "json.h"

#include "drive/numerics/BWareMath.h"
#include "drive/numerics/X87.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace golden {

using Bytes = std::vector<uint8_t>;

uint32_t Hex(std::string_view s);              // "0x..." -> uint32 (also accepts no prefix)
uint64_t Hex64(std::string_view s);
Bytes FromHex(std::string_view s);              // hex string (either case) -> bytes
std::string HexBytes(const uint8_t* b, size_t n);           // lower case
inline std::string HexBytes(const Bytes& b) { return HexBytes(b.data(), b.size()); }
std::string HexUpper(const uint8_t* b, size_t n);           // upper case (C# Convert.ToHexString)
std::string HexFloats(const float* f, size_t n);            // lower case bytes of the floats
std::string F32(float f);                                   // "0x%08X" of the bits
std::string Hex8(uint32_t v);                               // "0x%08X"
std::string Hex4(uint32_t v);                               // "0x%04X"
std::string HexX(uint32_t v);                               // "%X" (no prefix)

inline float RdF(const uint8_t* b, int o) { float v; std::memcpy(&v, b + o, 4); return v; }
inline int RdI(const uint8_t* b, int o) { int v; std::memcpy(&v, b + o, 4); return v; }
inline uint32_t RdU(const uint8_t* b, int o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; }
inline uint16_t RdU16(const uint8_t* b, int o) { uint16_t v; std::memcpy(&v, b + o, 2); return v; }
inline void WrF(uint8_t* b, int o, float v) { std::memcpy(b + o, &v, 4); }
inline void WrI(uint8_t* b, int o, int v) { std::memcpy(b + o, &v, 4); }
inline void WrU(uint8_t* b, int o, uint32_t v) { std::memcpy(b + o, &v, 4); }
inline void WrU16(uint8_t* b, int o, uint16_t v) { std::memcpy(b + o, &v, 2); }
inline float RdF(const Bytes& b, int o) { return RdF(b.data(), o); }
inline int RdI(const Bytes& b, int o) { return RdI(b.data(), o); }
inline uint32_t RdU(const Bytes& b, int o) { return RdU(b.data(), o); }
inline void WrF(Bytes& b, int o, float v) { WrF(b.data(), o, v); }
inline void WrI(Bytes& b, int o, int v) { WrI(b.data(), o, v); }
inline void Rd(const Bytes& b, int o, float* dst, int n) { for (int i = 0; i < n; i++) dst[i] = RdF(b, o + 4 * i); }
inline void Wr(Bytes& b, int o, const float* src, int n) { for (int i = 0; i < n; i++) WrF(b, o + 4 * i, src[i]); }

struct GoldenCase {
    const JVal* Root = nullptr;
    std::map<int, Bytes> Bufs;
    std::map<int, std::vector<std::pair<int, int>>> Fixups;   // (offset, buffer id)

    const JVal& Arg(int i) const { return (*Root)["args"][i]; }
    float F(int i) const { return bb::X87::Bits(Hex(Arg(i)["v"].s)); }
    uint32_t U(int i) const { return Hex(Arg(i)["v"].s); }
    int BufId(int i) const { return Arg(i)["id"].I32(); }
    std::vector<float> Vec(int i, int n) const {
        const Bytes& b = Bufs.at(BufId(i));
        std::vector<float> f(static_cast<size_t>(n));
        for (int k = 0; k < n; k++) f[static_cast<size_t>(k)] = RdF(b, 4 * k);
        return f;
    }
    int ThisId() const { return (*Root)["this"]["id"].I32(); }
    const JVal& Out() const { return (*Root)["out"]; }
    std::string OutBuf(int id) const { return Out()["bufs"][std::to_string(id)].Str(); }
};

class Suite {
public:
    virtual ~Suite() = default;
    virtual bool Handles(const std::string& goldenName) const = 0;
    // Called once per golden file before its cases (root = the whole document; path = file path).
    virtual void BeginFile(const std::string& /*path*/, const JVal& /*root*/) {}
    virtual void Evaluate(bb::BWareMath& math, const std::string& name, GoldenCase& c, std::string& want, std::string& got) = 0;
};

std::vector<Suite*>& Registry();
struct Register { explicit Register(Suite* s) { Registry().push_back(s); } };

// Installed game executable (BLACKBOX_GAME_DIR or F:\Games\NFSU2).
std::wstring ExePath();

} // namespace golden
