#pragma once

// Port of NFSU2Unity Core/Audio/AbkBank.cs.

#include "drive/audio/EaBnk.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bb {

// Entry of an ABK player sample table (12 bytes).
struct AbkSampleRef {
    // 0 = RAM (BNK index), 1 = streamed (.ast offset), 2 = streamed with loop segment.
    uint8_t Type = 0;
    uint8_t Priority = 0;
    // BNK sound index (type 0) or SCHl offset in the companion .ast (types 1/2).
    uint32_t Index = 0;
    uint32_t LoopOffset = 0;
};

// AEMS module descriptor (0x3C bytes + 4 per player/controller).
struct AbkModule {
    int Offset = 0;
    int NumPlayers = 0;
    int NumControllers = 0;
    // Absolute offset of the module's data (x86 code + player records).
    int DataOffset = 0;
    // Player record offsets, relative to DataOffset.
    std::vector<int> Players;
    // Sample table (absolute offset) referenced by each player.
    std::vector<int> PlayerSampleTables;
};

// Csis class exported by the bank (interface table at header +0x38).
struct AbkInterface {
    uint32_t ModuleOffset = 0;
    uint16_t SystemCrc = 0;
    uint16_t InterfaceCrc = 0;
    uint32_t Hash = 0;
    std::string Name;
};

// EA AEMS bank (.abk, magic "ABKC"), as shipped with NFSU2 PC: a relocatable x86 module region
// (AEMS "players"/class code, relocated at load time) followed by an embedded BNKl with the RAM samples
// and relocation/interface tables. See docs/re/audio-formats.md for the byte layout.
class AbkBank {
public:
    std::vector<uint8_t> Data;
    uint8_t Version = 0, AimexMajor = 0, AimexMinor = 0, AimexRev = 0;
    int NumModules = 0;
    int TotalSize = 0;
    int ModuleRegionSize = 0;
    int ModulesTable = 0;
    int BnkOffset = 0;
    int BnkSize = 0;
    int RelocTableA = 0, RelocTableB = 0, InterfaceTable = 0;
    std::vector<AbkModule> Modules;
    std::vector<AbkInterface> Interfaces;
    // Distinct sample tables (absolute offsets) in traversal order.
    std::vector<int> SampleTables;
    std::map<int, std::vector<AbkSampleRef>> SampleTableEntries;
    std::unique_ptr<EaBnk> Bnk;

    // Takes ownership of the file bytes. Throws std::runtime_error on a malformed bank.
    explicit AbkBank(std::vector<uint8_t> data);
    AbkBank(const AbkBank&) = delete;
    AbkBank& operator=(const AbkBank&) = delete;

    std::unique_ptr<PcmSound> Decode(int bnkIndex) const { return Bnk->Decode(bnkIndex); }

private:
    int U32(int o) const;
};

} // namespace bb
