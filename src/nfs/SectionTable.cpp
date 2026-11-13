#include "nfs/SectionTable.h"

#include <cstring>

#include "core/Log.h"

#include <algorithm>

namespace nfs {

namespace {

constexpr size_t kSectionEntrySize = 80;

// Field offsets inside one table entry.
constexpr size_t kOffName         = 0x00;  // four ASCII characters, zero padded
constexpr size_t kOffNumber       = 0x08;
constexpr size_t kOffStreamOffset = 0x14;
constexpr size_t kOffStreamSize   = 0x18;
constexpr size_t kOffGeometrySize = 0x1C;
constexpr size_t kOffOrder        = 0x20;
constexpr size_t kOffPosition     = 0x24;
constexpr size_t kOffHash         = 0x30;

std::string DecodeName(u32 packed) {
    char text[5] = {};
    for (int i = 0; i < 4; ++i) {
        const char c = (char)((packed >> (i * 8)) & 0xFF);
        if (c == '\0') break;
        text[i] = c;
    }
    return text;
}

} // namespace

bool SectionTable::Parse(ByteSpan bunFile) {
    sections_.clear();

    ChunkReader reader(bunFile, 0);
    Chunk chunk;
    ByteSpan table;
    u64 chunkDataOffset = 0;

    while (reader.Next(chunk)) {
        if (chunk.id == kChunkSectionTable) {
            table = chunk.data;
            chunkDataOffset = chunk.dataOffset;
            break;
        }
    }

    if (table.empty()) {
        LOG_ERROR("section table (chunk %08X) not found", (unsigned)kChunkSectionTable);
        return false;
    }

    const size_t count = table.size / kSectionEntrySize;
    sections_.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const ByteSpan entry = table.sub(i * kSectionEntrySize, kSectionEntrySize);

        Section section;
        section.tableEntryOffset = chunkDataOffset + i * kSectionEntrySize;
        section.id           = entry.read<u32>(kOffName);
        section.name         = DecodeName(section.id);
        section.number       = entry.read<u32>(kOffNumber);
        section.streamOffset = entry.read<u32>(kOffStreamOffset);
        section.streamSize   = entry.read<u32>(kOffStreamSize);
        section.geometrySize = entry.read<u32>(kOffGeometrySize);
        section.order        = entry.read<u32>(kOffOrder);
        section.hash         = entry.read<u32>(kOffHash);
        section.position     = { entry.read<float>(kOffPosition),
                                 entry.read<float>(kOffPosition + 4),
                                 entry.read<float>(kOffPosition + 8) };

        section.originalStreamOffset = section.streamOffset;
        section.originalStreamSize   = section.streamSize;
        section.originalOtherSize    = section.geometrySize;

        sections_.push_back(std::move(section));
    }

    LOG_INFO("section table: %zu sections", sections_.size());
    return !sections_.empty();
}

void SectionTable::ApplyBytes(u64 fileOffset, const u8* data, size_t size) {
    for (Section& section : sections_) {
        if (!section.tableEntryOffset) continue;

        const u64 entryBegin = section.tableEntryOffset;
        const u64 entryEnd   = entryBegin + kSectionEntrySize;
        if (fileOffset + size <= entryBegin || fileOffset >= entryEnd) continue;

        u8 entry[kSectionEntrySize];
        std::memcpy(entry, &section.id, 0);   // silence an unused warning on some builds

        // Rebuild the entry's bytes from what was parsed, lay the patch over
        // them, and read the fields back.
        std::memset(entry, 0, sizeof(entry));
        std::memcpy(entry + kOffStreamOffset, &section.streamOffset, 4);
        std::memcpy(entry + kOffStreamSize, &section.streamSize, 4);
        std::memcpy(entry + kOffGeometrySize, &section.geometrySize, 4);

        const u64 from = fileOffset > entryBegin ? fileOffset : entryBegin;
        const u64 to   = fileOffset + size < entryEnd ? fileOffset + size : entryEnd;
        std::memcpy(entry + (from - entryBegin), data + (from - fileOffset),
                    (size_t)(to - from));

        std::memcpy(&section.streamOffset, entry + kOffStreamOffset, 4);
        std::memcpy(&section.streamSize, entry + kOffStreamSize, 4);
        std::memcpy(&section.geometrySize, entry + kOffGeometrySize, 4);

        LOG_INFO("section %s repointed to %u, %u bytes", section.name.c_str(),
                 section.streamOffset, section.streamSize);
    }
}

const Section* SectionTable::FindByName(const std::string& name) const {
    for (const Section& section : sections_)
        if (section.name == name) return &section;
    return nullptr;
}

const Section* SectionTable::FindByNumber(u32 number) const {
    for (const Section& section : sections_)
        if (section.number == number) return &section;
    return nullptr;
}

namespace {

bool IsPositioned(const Section& section) {
    // The first entries describe global data (textures, shared geometry, the
    // backdrop) and carry no position; including them would drag any box built
    // from the table towards the origin.
    return section.position.x != 0.0f || section.position.y != 0.0f;
}

} // namespace

Aabb SectionTable::Bounds() const {
    Aabb bounds;
    for (const Section& section : sections_)
        if (IsPositioned(section)) bounds.Add(section.position);
    return bounds;
}

float SectionTable::TypicalSectionSpacing() const {
    std::vector<float> nearest;
    nearest.reserve(sections_.size());

    for (const Section& section : sections_) {
        if (!IsPositioned(section)) continue;

        float best = 0.0f;
        for (const Section& other : sections_) {
            if (&other == &section || !IsPositioned(other)) continue;
            const float distance = Length(other.position - section.position);
            if (distance > 0.0f && (best == 0.0f || distance < best)) best = distance;
        }
        if (best > 0.0f) nearest.push_back(best);
    }

    if (nearest.empty()) return 0.0f;

    const size_t middle = nearest.size() / 2;
    std::nth_element(nearest.begin(), nearest.begin() + middle, nearest.end());
    return nearest[middle];
}

} // namespace nfs
