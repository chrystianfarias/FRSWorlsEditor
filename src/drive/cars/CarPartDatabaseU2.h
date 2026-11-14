#pragma once

// Port of NFSU2Unity Core/Cars/CarPartDatabaseU2.cs: the car part database
// (GLOBAL\GLOBALB.BUN chunks 0x80034602 and 0x34607). Addresses refer to SPEED2.EXE v1.2.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bb {

namespace carbin { struct Chunk; }

// CarPartAttribute (8 bytes, chunk 0x34605). Values are raw u32s (hash, int, colour...).
struct CarPartAttribute {
    uint32_t NameHash = 0;
    uint32_t Value = 0;
};

// CarPartModelTable (0x24 bytes, chunk 0x3460A). See GetModelNameHash (0x60FF80).
struct CarPartModelTable {
    bool Templated = false;        // +0x00
    uint16_t ConcatString = 0;     // +0x02, string index (x4), 0xFFFF = none
    uint32_t Entries[8] = {};      // +0x04: [row * 4 + lod]; a solid hash, or a string index when templated
};

// CarPart (14 bytes, chunk 0x34604). The array is sorted by TypeIndex.
struct CarPart {
    uint32_t PartNameHash = 0;     // +0x00
    int8_t PartId = 0;             // +0x04, CarPartID
    uint8_t LevelGroup = 0;        // +0x05, bits 7..5 upgrade level, bits 4..0 group
    uint8_t PartialHashSelector = 0; // +0x06: 0 -> 0xFFFFFFFF, 1 -> type hash, else BRAND_NAME
    uint8_t TypeIndex = 0;         // +0x07
    uint16_t NameOffset = 0;       // +0x08, string index (x4)
    uint16_t AttributeList = 0;    // +0x0A, 0xFFFF = none
    uint16_t ModelTable = 0;       // +0x0C, 0xFFFF = none

    int UpgradeLevel() const { return LevelGroup >> 5; }
    int Group() const { return LevelGroup & 0x1F; }
};

// Parts are referenced by index into Parts; -1 means "no part".
class CarPartDatabaseU2 {
public:
    static constexpr uint32_t PackChunkId = 0x80034602;
    static constexpr uint32_t SlotTypesChunkId = 0x00034607;
    static constexpr int NumSlots = 170;

    static constexpr uint32_t AttrBrandName = 0xEBB03E66;
    static constexpr uint32_t AttrInnerRadius = 0xEB0101E2;
    static constexpr uint32_t AttrOuterRadius = 0xCE7D8DB5;
    static constexpr uint32_t AttrTextureName = 0x10C98090;
    static constexpr uint32_t AttrLightMaterialName = 0x6BA02C05;
    static constexpr uint32_t AttrPartHide = 0xE80A3B62;
    static const uint32_t AttrExcludeUG1;
    static const uint32_t AttrExcludeUG2;
    static const uint32_t AttrExcludeSUV;
    static const uint32_t AttrHoodUnder;

    std::vector<CarPartAttribute> Attributes;
    std::vector<CarPartModelTable> ModelTables;
    std::vector<uint32_t> TypeNameHashes;
    std::vector<CarPart> Parts;

    // DefaultSlotTypeNameTable[170][2] (0xFFFFFFFF = the car itself, 0 = empty).
    std::vector<uint32_t> DefaultSlotTypes;
    struct SlotTypeOverride {
        uint32_t CarNameHash = 0;
        int32_t Slot = 0;
        uint32_t Type0 = 0, Type1 = 0;
    };
    std::vector<SlotTypeOverride> SlotTypeOverrides;

    // Throws std::runtime_error when GLOBALB lacks the chunks.
    static CarPartDatabaseU2 Read(const std::vector<uint8_t>& globalB);

    // String at a string index (offsets are in 4-byte units); empty for 0xFFFF.
    std::string String(int index) const;
    std::string NameOf(int part) const { return String(Parts[part].NameOffset); }
    uint32_t TypeHashOf(int part) const { return TypeNameHashes[Parts[part].TypeIndex]; }

    int GetNextAppliedAttribute(int part, uint32_t hash, int prev = -1) const;
    uint32_t GetAttribute(int part, uint32_t hash, uint32_t fallback = 0) const;
    bool HasAttribute(int part, uint32_t hash) const { return GetNextAppliedAttribute(part, hash) >= 0; }
    uint32_t GetBrandNameHash(int part) const { return GetAttribute(part, AttrBrandName); }
    uint32_t GetTextureName(int part) const { return GetAttribute(part, AttrTextureName); }
    uint32_t GetLightMaterialName(int part) const { return GetAttribute(part, AttrLightMaterialName); }

    // Solid name hash of a part's model; 0 when there is no model for that row/LOD.
    uint32_t ModelNameHash(int part, int row, int lod) const;

    std::pair<uint32_t, uint32_t> GetTypesFromSlot(int slot, uint32_t carNameHash) const;
    int GetCarPartDirect(uint32_t typeHash, int partId, uint32_t partHash, int prev, int stage) const;
    int NewGetCarPart(uint32_t carNameHash, int slot, uint32_t partHash, int prev, int stage) const;
    int FindPartWithLevel(uint32_t carNameHash, int slot, int level) const;

private:
    void ReadSlotTypes(const std::vector<uint8_t>& buf, const carbin::Chunk& c);
    void ReadPack(const std::vector<uint8_t>& buf, const carbin::Chunk& pack);
    int ScanTypeIndex(uint8_t key) const;

    std::vector<uint8_t> strings_;
    std::vector<int16_t> attributeLists_;
};

// Hard-coded car lists in SPEED2.EXE.
struct CarListsU2 {
    static bool IsUG1(uint32_t carNameHash);
    static bool IsUG2(uint32_t carNameHash);
    static bool IsSUV(uint32_t h) { return h == 0x0219C94A || h == 0x6126DD4D || h == 0xCF704771; }
};

} // namespace bb
