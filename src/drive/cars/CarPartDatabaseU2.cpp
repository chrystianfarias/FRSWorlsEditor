#include "drive/cars/CarPartDatabaseU2.h"

#include "drive/cars/CarBin.h"
#include "drive/cars/CarSlotsU2.h"

#include <stdexcept>

namespace bb {

const uint32_t CarPartDatabaseU2::AttrExcludeUG1 = carbin::Hash("EXCLUDE_UG1");
const uint32_t CarPartDatabaseU2::AttrExcludeUG2 = carbin::Hash("EXCLUDE_UG2");
const uint32_t CarPartDatabaseU2::AttrExcludeSUV = carbin::Hash("EXCLUDE_SUV");
const uint32_t CarPartDatabaseU2::AttrHoodUnder = carbin::Hash("HOODUNDER");

// ------------------------------------------------------------------ loading

CarPartDatabaseU2 CarPartDatabaseU2::Read(const std::vector<uint8_t>& globalB) {
    CarPartDatabaseU2 db;
    bool pack = false, types = false;
    for (const carbin::Chunk& c : carbin::Parse(globalB)) {
        if (c.Id == PackChunkId) {
            db.ReadPack(globalB, c);
            pack = true;
        } else if (c.Id == SlotTypesChunkId) {
            db.ReadSlotTypes(globalB, c);
            types = true;
        }
    }
    if (!pack || !types) throw std::runtime_error("GLOBALB without 0x80034602/0x34607");
    return db;
}

// LoaderCarInfo 0x636C96: defaults at chunk+8 (0x550 bytes), overrides after them (16 bytes each).
void CarPartDatabaseU2::ReadSlotTypes(const std::vector<uint8_t>& buf, const carbin::Chunk& c) {
    carbin::Reader r = carbin::ChunkReader(buf, c);
    DefaultSlotTypes.assign(NumSlots * 2, 0);
    for (size_t i = 0; i < DefaultSlotTypes.size(); i++) DefaultSlotTypes[i] = r.U32();
    SlotTypeOverrides.resize((size_t)((c.Size - 0x550) >> 4));
    for (SlotTypeOverride& o : SlotTypeOverrides) {
        o.CarNameHash = r.U32();
        o.Slot = r.S32();
        o.Type0 = r.U32();
        o.Type1 = r.U32();
    }
}

// LoaderCarInfo 0x636CD6.
void CarPartDatabaseU2::ReadPack(const std::vector<uint8_t>& buf, const carbin::Chunk& pack) {
    for (const carbin::Chunk& c : pack.Children) {
        carbin::Reader r = carbin::ChunkReader(buf, c);
        switch (c.Id) {
            case 0x34606:
                strings_.assign(buf.begin() + c.DataOffset(), buf.begin() + c.End());
                break;
            case 0x3460C:
                attributeLists_.resize((size_t)(c.Size / 2));
                for (int16_t& v : attributeLists_) v = r.S16();
                break;
            case 0x34605:
                Attributes.resize((size_t)(c.Size / 8));
                for (CarPartAttribute& a : Attributes) {
                    a.NameHash = r.U32();
                    a.Value = r.U32();
                }
                break;
            case 0x3460A:
                ModelTables.resize((size_t)(c.Size / 0x24));
                for (CarPartModelTable& t : ModelTables) {
                    t.Templated = r.U8() != 0;
                    r.Skip(1);
                    t.ConcatString = r.U16();
                    for (int k = 0; k < 8; k++) t.Entries[k] = r.U32();
                }
                break;
            case 0x3460B:
                TypeNameHashes.resize((size_t)(c.Size / 4));
                for (uint32_t& h : TypeNameHashes) h = r.U32();
                break;
            case 0x34604:
                Parts.resize((size_t)(c.Size / 14));
                for (CarPart& p : Parts) {
                    p.PartNameHash = r.U32();
                    p.PartId = r.S8();
                    p.LevelGroup = r.U8();
                    p.PartialHashSelector = r.U8();
                    p.TypeIndex = r.U8();
                    p.NameOffset = r.U16();
                    p.AttributeList = r.U16();
                    p.ModelTable = r.U16();
                }
                break;
            default:
                break;
        }
    }
}

// ------------------------------------------------------------------ accessors

std::string CarPartDatabaseU2::String(int index) const {
    if (index == 0xFFFF) return {};
    return carbin::CString(strings_, index * 4);
}

// CarPart::GetNextAppliedAttribute (0x60FEF0).
int CarPartDatabaseU2::GetNextAppliedAttribute(int part, uint32_t hash, int prev) const {
    const int ofs = Parts[part].AttributeList;
    if (ofs == 0xFFFF || ofs >= (int)attributeLists_.size()) return -1;
    const int n = attributeLists_[ofs];
    int i = 0;
    if (prev >= 0) {
        while (i < n) {
            const int a = attributeLists_[ofs + 1 + i];
            i++;
            if (a == prev) break;
        }
        if (i == n) return -1;
    }
    for (; i < n; i++) {
        const int a = attributeLists_[ofs + 1 + i];
        if (hash == 0 || Attributes[a].NameHash == hash) return a;
    }
    return -1;
}

// CarPart::GetAppliedAttributeUParam (0x61B500).
uint32_t CarPartDatabaseU2::GetAttribute(int part, uint32_t hash, uint32_t fallback) const {
    const int a = GetNextAppliedAttribute(part, hash);
    return a >= 0 ? Attributes[a].Value : fallback;
}

uint32_t CarPartDatabaseU2::ModelNameHash(int part, int row, int lod) const {
    const CarPart& p = Parts[part];
    if (p.ModelTable == 0xFFFF) return 0;
    const CarPartModelTable& t = ModelTables[p.ModelTable];
    const uint32_t partial = p.PartialHashSelector == 0   ? 0xFFFFFFFFu
                             : p.PartialHashSelector == 1 ? TypeNameHashes[p.TypeIndex]
                                                          : GetBrandNameHash(part);
    const uint32_t e = t.Entries[row * 4 + lod];
    if (e == 0xFFFFFFFFu) return 0;
    if (!t.Templated) return e;
    uint32_t h = partial;
    if (t.ConcatString != 0xFFFF) h = carbin::Hash(String(t.ConcatString), h);
    h = carbin::Hash(String((int)e), h);
    const char suffix[3] = { '_', (char)('A' + lod), 0 };
    return carbin::Hash(suffix, h);
}

// ------------------------------------------------------------------ lookups

// GetTypesFromSlot (0x6101E0).
std::pair<uint32_t, uint32_t> CarPartDatabaseU2::GetTypesFromSlot(int slot, uint32_t carNameHash) const {
    for (const SlotTypeOverride& o : SlotTypeOverrides)
        if (o.CarNameHash == carNameHash && o.Slot == slot) return { o.Type0, o.Type1 };
    uint32_t t0 = DefaultSlotTypes[slot * 2], t1 = DefaultSlotTypes[slot * 2 + 1];
    if (t0 == 0xFFFFFFFFu) t0 = carNameHash;
    if (t1 == 0xFFFFFFFFu) t1 = carNameHash;
    return { t0, t1 };
}

// ScanHashTableKey8 (0x488C50).
int CarPartDatabaseU2::ScanTypeIndex(uint8_t key) const {
    int lo = 0, hi = (int)Parts.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t v = Parts[mid].TypeIndex;
        if (key < v) hi = mid - 1;
        else if (key > v) lo = mid + 1;
        else {
            while (mid > 0 && Parts[mid - 1].TypeIndex == key) mid--;
            return mid;
        }
    }
    return -1;
}

// CarPartDatabase::GetCarPartDirect (0x61B670), single pack. stage -1 = any level.
int CarPartDatabaseU2::GetCarPartDirect(uint32_t typeHash, int partId, uint32_t partHash, int prev, int stage) const {
    const bool isUG1 = CarListsU2::IsUG1(typeHash), isUG2 = CarListsU2::IsUG2(typeHash),
               isSUV = CarListsU2::IsSUV(typeHash);
    int p;
    if (prev >= 0)
        p = prev + 1;
    else {
        uint8_t key = 0xFF;
        for (size_t i = 0; i < TypeNameHashes.size(); i++)
            if (TypeNameHashes[i] == typeHash) {
                key = (uint8_t)i;
                break;
            }
        p = ScanTypeIndex(key);
        if (p < 0) return -1;
    }
    // Scans to the end of the array, not just to the end of the type run.
    for (; p < (int)Parts.size(); p++) {
        const CarPart& part = Parts[p];
        if (TypeNameHashes[part.TypeIndex] != typeHash) continue;
        if (part.PartId != partId) continue;
        if (partHash != 0 && part.PartNameHash != partHash) continue;
        const uint32_t exUG1 = GetAttribute(p, AttrExcludeUG1);
        const uint32_t exUG2 = GetAttribute(p, AttrExcludeUG2);
        const uint32_t exSUV = GetAttribute(p, AttrExcludeSUV);
        if (isUG1 && exUG1 != 0) continue;
        if (isUG2 && exUG2 != 0) continue;
        if (isSUV && exSUV != 0) continue;
        if (part.UpgradeLevel() == stage || stage == -1) return p;
    }
    return -1;
}

// CarPartDatabase::NewGetCarPart (0x61B980).
int CarPartDatabaseU2::NewGetCarPart(uint32_t carNameHash, int slot, uint32_t partHash, int prev, int stage) const {
    const auto t = GetTypesFromSlot(slot, carNameHash);
    const uint32_t types[2] = { t.first, t.second };
    const int partId = CarSlotsU2::SlotToPartId[slot];
    int i0 = 0;
    if (prev >= 0) {
        const uint32_t th = TypeHashOf(prev);
        while (i0 < 2 && types[i0] != th) i0++;
        if (i0 == 2) return -1;
    }
    for (int j = i0; j < 2; j++) {
        if (types[j] == 0) continue;
        if (j != i0) prev = -1;
        const int r = GetCarPartDirect(types[j], partId, partHash, prev, stage);
        if (r >= 0) return r;
    }
    return -1;
}

// FindPartWithLevel (0x61BC80).
int CarPartDatabaseU2::FindPartWithLevel(uint32_t carNameHash, int slot, int level) const {
    for (int p = NewGetCarPart(carNameHash, slot, 0, -1, -1); p >= 0; p = NewGetCarPart(carNameHash, slot, 0, p, -1))
        if (Parts[p].UpgradeLevel() == level) return p;
    return -1;
}

// ------------------------------------------------------------------ car lists

// IsUG1 0x610020
bool CarListsU2::IsUG1(uint32_t h) {
    static const uint32_t list[] = {
        0x03455D6B, 0x00163087, 0x0000DB7C, 0x0000DC00, 0x000AC6D1, 0x0150FB80, 0x0290C04D, 0x02CA04DF,
        0x72B2E513, 0x03B8C48A, 0x04FD0522, 0x545AEC60, 0x7502DA84, 0x79AEBFEC, 0xBE48375E,
    };
    for (uint32_t v : list)
        if (v == h) return true;
    return false;
}

// IsUG2 0x6100A0
bool CarListsU2::IsUG2(uint32_t h) {
    static const uint32_t list[] = {
        0x030236EE, 0x0000ACC9, 0x00000453, 0x000006E7, 0x0000A86E, 0x0000DC01, 0x0293FAD7, 0x4B10070A,
        0x09D2EFF6, 0x2D7C9B5D, 0x35165819, 0xCF4458F8, 0xF77C13EB,
    };
    for (uint32_t v : list)
        if (v == h) return true;
    return false;
}

} // namespace bb
