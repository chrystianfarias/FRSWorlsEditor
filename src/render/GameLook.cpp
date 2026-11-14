#include "render/GameLook.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gamelook {

namespace {

constexpr uint32_t kVs11 = 0xFFFE0101u;
constexpr uint32_t kPs11 = 0xFFFF0101u;
constexpr uint32_t kEndToken = 0x0000FFFFu;
constexpr uint32_t kOpTex = 0x42, kOpDef = 0x51, kOpDcl = 0x1F;

bool Contains(const uint32_t* tokens, size_t count, const char* text) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(tokens);
    const size_t n = count * 4, k = std::strlen(text);
    for (size_t i = 0; i + k <= n; ++i)
        if (std::memcmp(p + i, text, k) == 0) return true;
    return false;
}

// Length in dwords of the token stream starting at `at` (a version token), 0 when it runs off.
size_t ShaderLength(const uint32_t* t, size_t count, size_t at) {
    size_t i = at + 1;
    while (i < count) {
        const uint32_t token = t[i];
        if ((token & 0xFFFFu) == 0xFFFEu) {   // comment
            i += 1 + (token >> 16);
            continue;
        }
        if (token == kEndToken) return i + 1 - at;
        ++i;
    }
    return 0;
}

// Calls `visit(opcode, firstParameter, parameterCount, instructionToken)` for every instruction.
template <typename Visit>
void ForEachInstruction(const std::vector<uint32_t>& tokens, Visit visit) {
    size_t i = 1;
    while (i < tokens.size()) {
        const uint32_t token = tokens[i];
        if (token == kEndToken) break;
        if ((token & 0xFFFFu) == 0xFFFEu) { i += 1 + (token >> 16); continue; }
        const uint32_t op = token & 0xFFFFu;
        size_t n = 0;
        if (op == kOpDef) n = 5;
        else
            while (i + 1 + n < tokens.size() && (tokens[i + 1 + n] & 0x80000000u)) ++n;
        visit(op, i + 1, n, token);
        i += 1 + n;
    }
}

bool HasInstruction(const std::vector<uint32_t>& tokens, uint32_t opcode) {
    bool found = false;
    ForEachInstruction(tokens, [&](uint32_t op, size_t, size_t, uint32_t) { found |= op == opcode; });
    return found;
}

// Every vs_1_1 / ps_1_1 the effect resource holds (each stored after a dword with its byte count).
bool ShadersOf(HMODULE module, const wchar_t* resource, std::vector<std::vector<uint32_t>>& vertex,
               std::vector<std::vector<uint32_t>>& pixel) {
    HRSRC info = ::FindResourceW(module, resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!info) return false;
    const DWORD size = ::SizeofResource(module, info);
    HGLOBAL handle = ::LoadResource(module, info);
    const uint8_t* data = handle ? static_cast<const uint8_t*>(::LockResource(handle)) : nullptr;
    if (!data || size < 8) return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, tokens.size() * 4);
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] != kVs11 && tokens[i] != kPs11) continue;
        const size_t n = ShaderLength(tokens.data(), tokens.size(), i);
        if (n == 0 || tokens[i - 1] != n * 4) continue;
        (tokens[i] == kVs11 ? vertex : pixel)
            .emplace_back(tokens.begin() + (std::ptrdiff_t)i, tokens.begin() + (std::ptrdiff_t)(i + n));
        i += n - 1;
    }
    return true;
}

template <typename Pred>
bool Pick(const std::vector<std::vector<uint32_t>>& list, Pred pred, std::vector<uint32_t>& out) {
    for (const auto& shader : list)
        if (pred(shader)) {
            out = shader;
            return true;
        }
    return false;
}

std::string RegisterName(uint32_t t, bool pixel, bool destination) {
    const uint32_t type = ((t >> 28) & 0x7u) | ((t >> 8) & 0x18u);
    const uint32_t number = t & 0x7FFu;
    char text[32];
    switch (type) {
        case 0: std::snprintf(text, sizeof text, "r%u", number); break;
        case 1: std::snprintf(text, sizeof text, "v%u", number); break;
        case 2: std::snprintf(text, sizeof text, "c%u", number); break;
        case 3: std::snprintf(text, sizeof text, pixel ? "t%u" : "a%u", number); break;
        case 4: std::snprintf(text, sizeof text, "%s", number == 0 ? "oPos" : number == 1 ? "oFog" : "oPts"); break;
        case 5: std::snprintf(text, sizeof text, "oD%u", number); break;
        case 6: std::snprintf(text, sizeof text, "oT%u", number); break;
        default: std::snprintf(text, sizeof text, "?%u_%u", type, number); break;
    }
    std::string name = text;
    if (destination) {
        const uint32_t mask = (t >> 16) & 0xFu;
        if (mask != 0xFu) {
            name += '.';
            for (int k = 0; k < 4; ++k)
                if (mask & (1u << k)) name += "xyzw"[k];
        }
    } else {
        const uint32_t swizzle = (t >> 16) & 0xFFu;
        if (swizzle != 0xE4u) {
            const int c[4] = { (int)(swizzle & 3), (int)((swizzle >> 2) & 3), (int)((swizzle >> 4) & 3),
                               (int)((swizzle >> 6) & 3) };
            name += '.';
            if (c[0] == c[1] && c[1] == c[2] && c[2] == c[3]) name += "xyzw"[c[0]];
            else
                for (int k = 0; k < 4; ++k) name += "xyzw"[c[k]];
        }
        const uint32_t modifier = (t >> 24) & 0xFu;
        if (modifier == 1) name = "-" + name;
        else if (modifier != 0) name = "(mod" + std::to_string(modifier) + ")" + name;
    }
    return name;
}

const char* OpName(uint32_t op) {
    switch (op) {
        case 0x00: return "nop";
        case 0x01: return "mov";
        case 0x02: return "add";
        case 0x03: return "sub";
        case 0x04: return "mad";
        case 0x05: return "mul";
        case 0x06: return "rcp";
        case 0x07: return "rsq";
        case 0x08: return "dp3";
        case 0x09: return "dp4";
        case 0x0A: return "min";
        case 0x0B: return "max";
        case 0x0C: return "slt";
        case 0x0D: return "sge";
        case 0x0E: return "exp";
        case 0x0F: return "log";
        case 0x10: return "lit";
        case 0x11: return "dst";
        case 0x12: return "lrp";
        case 0x13: return "frc";
        case kOpDcl: return "dcl";
        case kOpTex: return "tex";
        case kOpDef: return "def";
        default: return "op?";
    }
}

} // namespace

bool ExtractWorldEffects(const std::wstring& exePath, WorldEffects& out, std::string& error) {
    out = WorldEffects{};
    HMODULE module = ::LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        error = "cannot open SPEED2.EXE";
        return false;
    }
    bool ok = true;
    std::vector<std::vector<uint32_t>> vs, ps;
    auto has = [](const char* text) {
        return [text](const std::vector<uint32_t>& s) { return Contains(s.data(), s.size(), text); };
    };
    auto withTex = [](const std::vector<uint32_t>& s) { return HasInstruction(s, kOpTex); };

    // IDI_WORLD_FX (rva 0x4BFD70, 13100 bytes): pass p0's VS reads FogValue and TextureOffset (pass p1's
    // is the depth-of-field mask with FocalRange); p0's PS samples t0 (p1's only copies v0).
    if (!ShadersOf(module, L"IDI_WORLD_FX", vs, ps) ||
        !Pick(vs, [&](const std::vector<uint32_t>& s) { return has("FogValue")(s) && has("TextureOffset")(s); },
              out.world.vertexShader) ||
        !Pick(ps, withTex, out.world.pixelShader)) {
        error = "IDI_WORLD_FX: no technique world pass p0 shaders";
        ok = false;
    }
    vs.clear();
    ps.clear();
    // IDI_WORLDREFLECT_FX (rva 0x4CEC68, 14808 bytes): one VS (FogValue) and one PS (reflected_sampler).
    if (ok && (!ShadersOf(module, L"IDI_WORLDREFLECT_FX", vs, ps) || !Pick(vs, has("FogValue"), out.reflect.vertexShader) ||
               !Pick(ps, has("reflected_sampler"), out.reflect.pixelShader))) {
        error = "IDI_WORLDREFLECT_FX: no technique world pass p0 shaders";
        ok = false;
    }
    vs.clear();
    ps.clear();
    // IDI_WORLDNOFOG_FX (rva 0x4C3400, 10812 bytes): VS with TextureOffset only.
    if (ok && (!ShadersOf(module, L"IDI_WORLDNOFOG_FX", vs, ps) ||
               !Pick(vs,
                     [&](const std::vector<uint32_t>& s) {
                         return has("TextureOffset")(s) && !has("FogValue")(s) && !has("FocalRange")(s);
                     },
                     out.noFog.vertexShader) ||
               !Pick(ps, withTex, out.noFog.pixelShader))) {
        error = "IDI_WORLDNOFOG_FX: no technique world pass p0 shaders";
        ok = false;
    }
    vs.clear();
    ps.clear();
    // IDI_FILTER_FX (rva 0x4DB2D0): pass p3's PS samples four textures (filter_sampler3), its VS declares a
    // position and four texture coordinates.
    if (ok && ShadersOf(module, L"IDI_FILTER_FX", vs, ps)) {
        auto dclCount = [](const std::vector<uint32_t>& s) {
            int n = 0;
            ForEachInstruction(s, [&](uint32_t op, size_t, size_t, uint32_t) { n += op == kOpDcl; });
            return n;
        };
        if (!Pick(vs, [&](const std::vector<uint32_t>& s) { return dclCount(s) == 5; }, out.filter.vertexShader) ||
            !Pick(ps, has("filter_sampler3"), out.filter.pixelShader))
            out.filter = EffectCode{};
    }
    ::FreeLibrary(module);
    return ok;
}

std::vector<std::string> Disassemble(const std::vector<uint32_t>& tokens) {
    std::vector<std::string> lines;
    if (tokens.empty()) return lines;
    const bool pixel = tokens[0] == kPs11;
    lines.push_back(pixel ? "ps_1_1" : "vs_1_1");
    ForEachInstruction(tokens, [&](uint32_t op, size_t first, size_t count, uint32_t instruction) {
        std::string line;
        if (instruction & 0x40000000u) line += '+';
        if (op == kOpDef && count == 5) {
            char text[128];
            float v[4];
            std::memcpy(v, &tokens[first + 1], sizeof v);
            std::snprintf(text, sizeof text, "def %s, %g, %g, %g, %g", RegisterName(tokens[first], pixel, true).c_str(),
                          v[0], v[1], v[2], v[3]);
            lines.push_back(line + text);
            return;
        }
        if (op == kOpDcl && count == 2) {
            const uint32_t usage = tokens[first] & 0x1Fu;
            const char* name = usage == 0 ? "position" : usage == 3 ? "normal" : usage == 5 ? "texcoord"
                             : usage == 10 ? "color" : "usage?";
            lines.push_back(line + "dcl_" + name + " " + RegisterName(tokens[first + 1], pixel, true));
            return;
        }
        line += OpName(op);
        if (count > 0) {
            const uint32_t shift = (tokens[first] >> 24) & 0xFu;
            if (shift == 1) line += "_x2";
            else if (shift == 2) line += "_x4";
            else if (shift == 0xF) line += "_d2";
            if ((tokens[first] >> 20) & 1u) line += "_sat";
            line += ' ';
            for (size_t k = 0; k < count; ++k) {
                if (k) line += ", ";
                line += RegisterName(tokens[first + k], pixel, k == 0);
            }
        }
        lines.push_back(line);
    });
    return lines;
}

u32 GameHash(const char* text) {
    u32 h = 0xFFFFFFFFu;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) h = h * 33u + *p;
    return h;
}

FogConstants MakeFogConstants(const FogState& fog) {
    FogConstants c;
    c.value[0] = 250.0f;
    c.value[1] = 1.0f / (250.0f - fog.start);
    const float density = 1.0f - fog.factor;
    c.value[2] = density <= 0.6f ? density : 0.6f;
    c.value[3] = 0.0f;
    // sub_614300 truncates each field into a byte (__ftol2), eDisplayFrame scales each byte by 0.75.
    auto channel = [](float v) {
        const int byte = (int)v & 0xFF;
        return (D3DCOLOR)((int)((float)byte * 0.75f) & 0xFF);
    };
    c.colour = 0xFF000000u | (channel(fog.r) << 16) | (channel(fog.g) << 8) | channel(fog.b);
    return c;
}

// ---------------------------------------------------------------------------
// Fog parameter maps
// ---------------------------------------------------------------------------

namespace {

struct Span {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

template <typename Visit>
void ForEachChunk(Span span, Visit visit) {
    size_t at = 0;
    while (at + 8 <= span.size) {
        uint32_t id = 0, size = 0;
        std::memcpy(&id, span.data + at, 4);
        std::memcpy(&size, span.data + at + 4, 4);
        if (at + 8 + size > span.size) break;
        visit(id, Span{ span.data + at + 8, size });
        at += 8 + (size_t)size;
    }
}

} // namespace

bool FogMap::Load(const std::wstring& bunPath) {
    sets_.clear();
    tree_.clear();
    haveLast_ = false;
    HANDLE file = ::CreateFileW(bunPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // Walk the top-level chunks, reading only the parameter maps.
    std::vector<uint8_t> chunk;
    LARGE_INTEGER position{};
    for (;;) {
        uint32_t header[2] = {};
        DWORD got = 0;
        if (!::ReadFile(file, header, 8, &got, nullptr) || got != 8) break;
        if (header[0] == 0x8003B600u) {
            chunk.resize(header[1]);
            if (!::ReadFile(file, chunk.data(), header[1], &got, nullptr) || got != header[1]) chunk.clear();
            break;
        }
        LARGE_INTEGER skip{};
        skip.QuadPart = header[1];
        if (!::SetFilePointerEx(file, skip, &position, FILE_CURRENT)) break;
    }
    ::CloseHandle(file);
    if (chunk.empty()) return false;

    const u32 wanted = GameHash("Normal Fog");
    ForEachChunk(Span{ chunk.data(), chunk.size() }, [&](uint32_t id, Span layer) {
        if (id != 0x8003B601u || !sets_.empty()) return;
        Span header, types, offsets, data, q8, q16;
        ForEachChunk(layer, [&](uint32_t sub, Span s) {
            switch (sub) {
                case 0x3B602: header = s; break;
                case 0x3B603: types = s; break;
                case 0x3B604: offsets = s; break;
                case 0x3B605: data = s; break;
                case 0x3B607: q8 = s; break;
                case 0x3B608: q16 = s; break;
                default: break;
            }
        });
        if (header.size < 0x24) return;
        uint32_t h[9];
        std::memcpy(h, header.data, sizeof h);
        if (h[0] != wanted) return;
        float bounds[4];
        std::memcpy(bounds, header.data + 4, sizeof bounds);
        const uint32_t nodes = h[5], count = h[6], stride = h[7], fields = h[8];
        if (fields < 5 || types.size != 4 * fields || offsets.size != 4 * fields || data.size != (size_t)count * stride)
            return;
        std::vector<uint16_t> tree;
        if (q8.size == 4ull * nodes && nodes) {
            for (size_t i = 0; i < q8.size; ++i) tree.push_back(q8.data[i]);
        } else if (q16.size == 8ull * nodes && nodes) {
            tree.resize(nodes * 4ull);
            std::memcpy(tree.data(), q16.data, q16.size);
        }
        std::vector<std::vector<float>> sets(count, std::vector<float>(8, 0.0f));
        for (uint32_t s = 0; s < count; ++s)
            for (uint32_t f = 0; f < fields && f < 8; ++f) {
                int32_t type = 0, offset = 0;
                std::memcpy(&type, types.data + 4 * f, 4);
                std::memcpy(&offset, offsets.data + 4 * f, 4);
                if (offset < 0 || (size_t)offset + 4 > stride) continue;
                const uint8_t* p = data.data + (size_t)s * stride + offset;
                if (type == 0) std::memcpy(&sets[s][f], p, 4);
                else {
                    int32_t v = 0;
                    std::memcpy(&v, p, 4);
                    sets[s][f] = (float)v;
                }
            }
        minX_ = bounds[0];
        minY_ = bounds[1];
        maxX_ = bounds[2];
        maxY_ = bounds[3];
        tree_ = std::move(tree);
        sets_ = std::move(sets);
    });
    return !sets_.empty();
}

int FogMap::SetIndexAt(float x, float y) const {
    if (sets_.empty()) return -1;
    if (tree_.empty()) return 0;
    // GetParameterSetIndexFromQuadData16 (0x5F0A30): its out-of-bounds test can never pass, so a point
    // outside the bounds walks into the edge cells.
    float minX = minX_, minY = minY_, maxX = maxX_, maxY = maxY_;
    size_t n = 0;
    for (int guard = 0; guard < 64; ++guard) {
        if (n * 4 + 3 >= tree_.size()) return 0;
        const uint16_t* c = &tree_[n * 4];
        if (c[0] == 0) {
            const int set = c[1];
            return set < (int)sets_.size() ? set : 0;
        }
        const float mx = (minX + maxX) * 0.5f, my = (minY + maxY) * 0.5f;
        if (x < mx) {
            maxX = mx;
            if (y < my) { n = c[0]; maxY = my; }
            else { n = c[1]; minY = my; }
        } else {
            minX = mx;
            if (y < my) { n = c[2]; maxY = my; }
            else { n = c[3]; minY = my; }
        }
    }
    return 0;
}

FogState FogMap::Sample(float x, float y) {
    FogState fog;   // the exe's fallback struct 0x800E18 when there is no map
    if (sets_.empty()) return fog;
    const std::vector<float>& set = sets_[(size_t)(std::max)(0, SetIndexAt(x, y))];
    if (!haveLast_ || smoothed_.size() != set.size()) {
        smoothed_ = set;
    } else {
        const float dx = x - lastX_, dy = y - lastY_;
        const float t = (std::min)(std::sqrt(dx * dx + dy * dy) / 50.0f, 1.0f);
        for (size_t i = 0; i < set.size(); ++i) smoothed_[i] = smoothed_[i] * (1.0f - t) + set[i] * t;
    }
    haveLast_ = true;
    lastX_ = x;
    lastY_ = y;
    // Fields: 0 factor, 1 start, 2 blue, 3 green, 4 red (sub_614300 0x614437..0x6145B2).
    fog.factor = smoothed_[0];
    fog.start = smoothed_[1];
    fog.b = smoothed_[2];
    fog.g = smoothed_[3];
    fog.r = smoothed_[4];
    return fog;
}

} // namespace gamelook
