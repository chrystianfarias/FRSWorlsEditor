// Port of NFSU2Unity Core/Audio/Engine/AemsCarBank.cs.

#include "drive/audio/engine/AemsCarBank.h"

#include "drive/audio/engine/AemsCarBankTables.g.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

namespace bb {

namespace {

std::vector<uint8_t> FromBase64(const char* s)
{
    static int8_t map[256];
    static bool init = false;
    if (!init) {
        std::fill(std::begin(map), std::end(map), static_cast<int8_t>(-1));
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) map[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char* p = s; *p; ++p) {
        int v = map[static_cast<uint8_t>(*p)];
        if (v < 0) continue; // '=' padding
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

std::vector<uint16_t> Unpack(const char* b64)
{
    std::vector<uint8_t> bytes = FromBase64(b64);
    std::vector<uint16_t> r(bytes.size() / 2);
    for (size_t i = 0; i < r.size(); i++) r[i] = static_cast<uint16_t>(bytes[2 * i] | bytes[2 * i + 1] << 8);
    return r;
}

std::mutex g_cacheLock;
std::map<int, std::shared_ptr<const AemsCarBank>> g_cache;

} // namespace

int AemsCarBank::Count() { return AemsCarBankTables::Count; }

std::shared_ptr<const AemsCarBank> AemsCarBank::Get(int bank)
{
    std::lock_guard<std::mutex> lock(g_cacheLock);
    auto it = g_cache.find(bank);
    if (it != g_cache.end()) return it->second;
    if (bank < 0 || bank >= Count()) throw std::out_of_range("AemsCarBank: no tables for bank " + std::to_string(bank));
    std::vector<uint16_t> g = Unpack(AemsCarBankTables::Gain[bank]);
    std::vector<uint16_t> p = Unpack(AemsCarBankTables::Pitch[bank]);
    size_t n = static_cast<size_t>(GridPoints) * Voices;
    if (g.size() < 2 * n || p.size() < n) throw std::runtime_error("AemsCarBank: bad baked table");
    std::vector<uint16_t> g0(g.begin(), g.begin() + static_cast<ptrdiff_t>(n));
    std::vector<uint16_t> g1(g.begin() + static_cast<ptrdiff_t>(n), g.begin() + static_cast<ptrdiff_t>(2 * n));
    auto b = std::make_shared<const AemsCarBank>(bank, std::move(g0), std::move(g1), std::move(p));
    g_cache[bank] = b;
    return b;
}

int AemsCarBank::IndexFromName(const std::string& bankName)
{
    std::string s = bankName;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    size_t i = s.find("CAR_");
    if (i == std::string::npos || i + 6 > s.size()) return -1;
    char a = s[i + 4], b = s[i + 5];
    if (!std::isdigit(static_cast<unsigned char>(a)) || !std::isdigit(static_cast<unsigned char>(b))) return -1;
    return (a - '0') * 10 + (b - '0');
}

bool AemsCarBank::IsRpmPitched(int voice) const
{
    uint16_t hi = pitch_[static_cast<size_t>(GridPoints - 1) * Voices + voice];
    uint16_t mid = pitch_[static_cast<size_t>(GridPoints / 2) * Voices + voice];
    return hi != NoPitch && hi != mid;
}

void AemsCarBank::Evaluate(const int* d, int* vol, int* pitchOut, int step) const
{
    int rpm = std::max(0, std::min(10000, d[1]));
    int load = d[3];
    int stride = step / GridStep;
    int k0 = rpm / step;
    if (k0 * step >= 10000) k0 = 10000 / step - 1;
    int p0 = k0 * stride, p1 = (k0 + 1) * stride;
    double t = (rpm - k0 * step) / static_cast<double>(step);
    double ang = 3.14159265358979323846 / 2 * load / 1024.0;
    double lOff = std::cos(ang), lOn = 1 - std::cos(ang);
    double g5 = (32767 + d[5]) / 32767.0;
    for (int v = 0; v < Voices; v++) {
        double a0 = gain0_[static_cast<size_t>(p0) * Voices + v], b0 = gain0_[static_cast<size_t>(p1) * Voices + v];
        double v0 = a0 + (b0 - a0) * t;
        if (v < 7) {
            double a1 = gain1_[static_cast<size_t>(p0) * Voices + v], b1 = gain1_[static_cast<size_t>(p1) * Voices + v];
            double v1 = a1 + (b1 - a1) * t;
            vol[v] = static_cast<int>((v0 * lOff + v1 * lOn) * g5);
        } else {
            vol[v] = static_cast<int>(v0 * d[25] / 32767.0);         // s8: baked with d25 = 0x7FFF
        }
        int pa = pitch_[static_cast<size_t>(p0) * Voices + v], pb = pitch_[static_cast<size_t>(p1) * Voices + v];
        if (pa == NoPitch || pb == NoPitch) pitchOut[v] = 0x1000;
        else pitchOut[v] = static_cast<int>(std::nearbyint(pa + (pb - pa) * t) * (1 + d[16] / 16000.0));
    }
}

int AemsCarVoices::Advance(double dt, const int* d)
{
    acc_ += dt;
    int n = 0;
    while (acc_ >= TickSeconds - 1e-9) {
        acc_ -= TickSeconds;
        Tick(d);
        n++;
    }
    return n;
}

void AemsCarVoices::Tick(const int* d)
{
    AemsTicks++;
    Bank->Evaluate(d, Target, Pitch);
    for (int v = 0; v < AemsCarBank::Voices; v++) {
        int g = Target[v];
        if (g > GateThreshold) {
            if (!on_[v]) {
                on_[v] = true;
                attack_[v] = 0;
                if (!Playing[v]) {
                    Playing[v] = true;
                    StartCount[v]++;
                }
            }
            if (attack_[v] < AttackTicks) attack_[v]++;
            Volume[v] = attack_[v] < AttackTicks ? g * attack_[v] / AttackTicks : g;
            release_[v] = 0;
        } else {
            if (on_[v]) {
                on_[v] = false;
                release_[v] = 0;
            }
            Volume[v] = 0;
            if (Playing[v] && ++release_[v] >= ReleaseTicks) Playing[v] = false;
        }
    }
}

} // namespace bb
