// Port of NFSU2Unity Core/Audio/AbkBank.cs.

#include "drive/audio/AbkBank.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bb {

int AbkBank::U32(int o) const
{
    if (o < 0 || o + 4 > static_cast<int>(Data.size())) throw std::runtime_error("ABKC: read out of range");
    int32_t v;
    std::memcpy(&v, Data.data() + o, 4);
    return v;
}

AbkBank::AbkBank(std::vector<uint8_t> data) : Data(std::move(data))
{
    const std::vector<uint8_t>& d = Data;
    if (d.size() < 0x40 || d[0] != 'A' || d[1] != 'B' || d[2] != 'K' || d[3] != 'C')
        throw std::runtime_error("not an ABKC bank");
    Version = d[4];
    AimexMajor = d[5];
    AimexMinor = d[6];
    AimexRev = d[7];
    NumModules = d[0x0A] | d[0x0B] << 8;
    TotalSize = U32(0x14);
    ModuleRegionSize = U32(0x18);
    ModulesTable = U32(0x1C);
    BnkOffset = U32(0x20);
    BnkSize = U32(0x24);
    RelocTableA = U32(0x30);
    RelocTableB = U32(0x34);
    InterfaceTable = U32(0x38);

    auto byteAt = [&](int o) -> int {
        if (o < 0 || o >= static_cast<int>(d.size())) throw std::runtime_error("ABKC: read out of range");
        return d[o];
    };

    int mt = ModulesTable;
    for (int i = 0; i < NumModules; i++) {
        AbkModule m;
        m.Offset = mt;
        m.NumPlayers = byteAt(mt + 0x24);
        m.NumControllers = byteAt(mt + 0x27);
        m.DataOffset = U32(mt + 0x2C);
        for (int j = 0; j < m.NumPlayers; j++) {
            int po = U32(mt + 0x3C + 4 * j);
            int table = U32(m.DataOffset + po + 4);
            m.Players.push_back(po);
            m.PlayerSampleTables.push_back(table);
            if (std::find(SampleTables.begin(), SampleTables.end(), table) == SampleTables.end()) {
                SampleTables.push_back(table);
                int n = U32(table);
                std::vector<AbkSampleRef> list;
                list.reserve(static_cast<size_t>(std::max(0, n)));
                for (int k = 0; k < n; k++) {
                    int e = table + 4 + 12 * k;
                    AbkSampleRef r;
                    r.Type = static_cast<uint8_t>(byteAt(e));
                    r.Priority = static_cast<uint8_t>(byteAt(e + 1));
                    r.Index = static_cast<uint32_t>(U32(e + 4));
                    r.LoopOffset = static_cast<uint32_t>(U32(e + 8));
                    list.push_back(r);
                }
                SampleTableEntries[table] = std::move(list);
            }
        }
        mt += 0x3C + (m.NumPlayers + m.NumControllers) * 4;
        Modules.push_back(std::move(m));
    }

    if (InterfaceTable > 0 && InterfaceTable + 4 <= static_cast<int>(d.size())) {
        int n = U32(InterfaceTable);
        for (int k = 0; k < n; k++) {
            int e = InterfaceTable + 4 + 12 * k;
            int p = U32(e + 4);
            AbkInterface it;
            it.ModuleOffset = static_cast<uint32_t>(U32(e));
            it.Hash = static_cast<uint32_t>(U32(e + 8));
            it.SystemCrc = static_cast<uint16_t>(byteAt(p) | byteAt(p + 1) << 8);
            it.InterfaceCrc = static_cast<uint16_t>(byteAt(p + 2) | byteAt(p + 3) << 8);
            int s = p + 4, z = s;
            while (z < static_cast<int>(d.size()) && d[z] != 0) z++;
            it.Name.assign(reinterpret_cast<const char*>(d.data()) + s, static_cast<size_t>(z - s));
            Interfaces.push_back(std::move(it));
        }
    }

    if (BnkOffset != 0) Bnk = std::make_unique<EaBnk>(Data, BnkOffset);
}

} // namespace bb
