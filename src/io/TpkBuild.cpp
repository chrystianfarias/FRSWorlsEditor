#include "io/TpkBuild.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr size_t kHeaderEntry = 124;
constexpr size_t kFormatEntry = 32;
constexpr size_t kHashEntry   = 8;

// Inside a 124-byte texture header.
constexpr size_t kName          = 0x0C;   // 20 bytes, zero terminated
constexpr size_t kNameCapacity  = 20;
constexpr size_t kHash          = 0x24;
// 0x28 holds something the texture belongs to rather than the texture itself;
// it is copied from the template and never written.
constexpr size_t kDataOffset    = 0x30;
constexpr size_t kPaletteOffset = 0x34;
constexpr size_t kDataSize      = 0x38;
constexpr size_t kPaletteSize   = 0x3C;
constexpr size_t kBaseLevelSize = 0x40;
constexpr size_t kWidth         = 0x44;
constexpr size_t kHeight        = 0x46;
constexpr size_t kWidthShift    = 0x48;   // log2 of the width
constexpr size_t kHeightShift   = 0x49;
constexpr size_t kMipCount      = 0x4E;

constexpr size_t kFormatFourCc = 0x14;

// Textures in the pixel block are laid end to end, with no alignment between
// them.
//
// That is what the shipped packs do: in this section's first pack the second
// texture ends at 131 104 and the third begins there, and 131 104 is not a
// multiple of 128. Aligning a new texture to 128, as this did at first, leaves
// a gap of zeros in a block the game may well be walking by cumulative size
// rather than by each entry's offset.

void Write32(std::vector<u8>& out, size_t at, u32 value) {
    std::memcpy(out.data() + at, &value, 4);
}

void Write16(std::vector<u8>& out, size_t at, u16 value) {
    std::memcpy(out.data() + at, &value, 2);
}

u32 Read32(const std::vector<u8>& in, size_t at) {
    u32 value = 0;
    std::memcpy(&value, in.data() + at, 4);
    return value;
}

u8 Log2(u32 value) {
    u8 shift = 0;
    while ((1u << shift) < value && shift < 31) ++shift;
    return shift;
}

// A header to model a new one on.
//
// Two thirds of the 124 bytes are fields whose meaning is not established, and
// inventing values for them would be guessing. Copying them from a texture the
// game already loads is not a guess: whatever they mean, these are values the
// game accepts for a texture of this kind. Only the fields whose meaning is
// known are then overwritten.
size_t TemplateFor(const std::vector<u8>& headers, size_t count, u32 fourcc,
                   const std::vector<u8>& formats) {
    for (size_t i = 0; i < count; ++i)
        if ((i + 1) * kFormatEntry <= formats.size() &&
            Read32(formats, i * kFormatEntry + kFormatFourCc) == fourcc)
            return i;
    return 0;   // no texture of that format in the pack; any entry will do
}

} // namespace

bool BuildTpkTables(const TpkTables& original, const std::vector<NewTexture>& added,
                    TpkTables& out, std::wstring& error) {
    if (original.headers.size() % kHeaderEntry) {
        error = L"The texture pack's header table is not a whole number of "
                L"entries, so this section's pack was not understood.";
        return false;
    }

    const size_t count = original.headers.size() / kHeaderEntry;
    if (original.formats.size() < count * kFormatEntry) {
        error = L"The texture pack has fewer format entries than textures.";
        return false;
    }

    out = original;

    // Where the existing pixels end, which is where new ones can start.
    size_t pixelEnd = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = i * kHeaderEntry;
        pixelEnd = std::max<size_t>(
            pixelEnd, Read32(original.headers, entry + kDataOffset) +
                      Read32(original.headers, entry + kDataSize));
        pixelEnd = std::max<size_t>(
            pixelEnd, Read32(original.headers, entry + kPaletteOffset) +
                      Read32(original.headers, entry + kPaletteSize));
    }

    for (const NewTexture& texture : added) {
        // Does the pack already hold this name? Then it is being re-imported.
        size_t slot = SIZE_MAX;
        const size_t slots = out.headers.size() / kHeaderEntry;
        for (size_t i = 0; i < slots; ++i)
            if (Read32(out.headers, i * kHeaderEntry + kHash) == texture.hash) {
                slot = i;
                break;
            }

        const bool replacing = slot != SIZE_MAX;
        if (!replacing) {
            const size_t model = TemplateFor(original.headers, count,
                                             texture.fourcc, original.formats);

            slot = slots;
            out.headers.insert(out.headers.end(),
                               original.headers.begin() + model * kHeaderEntry,
                               original.headers.begin() + (model + 1) * kHeaderEntry);
            out.formats.insert(out.formats.end(),
                               original.formats.begin() + model * kFormatEntry,
                               original.formats.begin() + (model + 1) * kFormatEntry);
        }

        // The pixels always go at the end, even when replacing: a texture that
        // grew cannot be written back over the one it replaces, and the room
        // its old copy occupied is not worth reclaiming for a handful of
        // kilobytes in a section that is rebuilt wholesale anyway.
        const size_t dataAt = std::max(out.pixels.size(), pixelEnd);
        out.pixels.resize(dataAt, 0);
        out.pixels.insert(out.pixels.end(), texture.pixels.begin(), texture.pixels.end());

        const size_t entry = slot * kHeaderEntry;
        u8* header = out.headers.data() + entry;

        std::memset(header + kName, 0, kNameCapacity);
        std::memcpy(header + kName, texture.name.c_str(),
                    std::min(texture.name.size(), kNameCapacity - 1));

        Write32(out.headers, entry + kHash, texture.hash);

        // The four bytes after the hash are left as the template had them.
        // They are not the texture's hash and not a constant either: of the
        // three textures in this section, two share a value and one differs, so
        // whatever they identify is a category rather than the texture. Writing
        // the hash there, as this did at first, was inventing a meaning.
        Write32(out.headers, entry + kDataOffset, (u32)dataAt);
        Write32(out.headers, entry + kDataSize, (u32)texture.pixels.size());
        Write32(out.headers, entry + kPaletteOffset, 0);
        Write32(out.headers, entry + kPaletteSize, 0);
        Write32(out.headers, entry + kBaseLevelSize, texture.baseLevelSize);
        Write16(out.headers, entry + kWidth, (u16)texture.width);
        Write16(out.headers, entry + kHeight, (u16)texture.height);
        out.headers[entry + kWidthShift]  = Log2(texture.width);
        out.headers[entry + kHeightShift] = Log2(texture.height);
        out.headers[entry + kMipCount]    = (u8)texture.mipCount;

        Write32(out.formats, slot * kFormatEntry + kFormatFourCc, texture.fourcc);

        LOG_INFO("texture '%s' %s pack slot %zu: %ux%u, %u levels, %zu bytes at %zu",
                 texture.name.c_str(), replacing ? "replaces" : "added at",
                 slot, texture.width, texture.height, texture.mipCount,
                 texture.pixels.size(), dataAt);
    }

    // --- the three tables, sorted together ----------------------------------
    //
    // The shipped packs keep two invariants at once: the headers run in
    // ascending order of hash, and the hash table runs exactly parallel to
    // them. A new texture cannot honour both without moving the textures
    // already there, so this first tried appending and leaving the order
    // alone -- and the game drew nothing at all for the new texture, which is
    // what a binary search that cannot find a hash looks like. The order is
    // not decoration; it is how the game finds a texture.
    //
    // Sorting is safe now for a reason that had nothing to do with sorting:
    // the earlier attempt crashed on the object header's texture count, a byte
    // that was never being updated. With that fixed, the tables can be put in
    // the order the game expects.
    const size_t total = out.headers.size() / kHeaderEntry;

    std::vector<size_t> order(total);
    for (size_t i = 0; i < total; ++i) order[i] = i;

    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return Read32(out.headers, a * kHeaderEntry + kHash) <
               Read32(out.headers, b * kHeaderEntry + kHash);
    });

    std::vector<u8> headers(out.headers.size());
    std::vector<u8> formats(out.formats.size());

    for (size_t i = 0; i < total; ++i) {
        std::memcpy(headers.data() + i * kHeaderEntry,
                    out.headers.data() + order[i] * kHeaderEntry, kHeaderEntry);
        std::memcpy(formats.data() + i * kFormatEntry,
                    out.formats.data() + order[i] * kFormatEntry, kFormatEntry);
    }

    out.headers = std::move(headers);
    out.formats = std::move(formats);

    // The four bytes beside each hash stay at zero: they are zero for every
    // texture in the shipped packs, so they are somewhere the loader fills in,
    // not an index into anything.
    out.hashTable.assign(total * kHashEntry, 0);
    for (size_t i = 0; i < total; ++i)
        Write32(out.hashTable, i * kHashEntry,
                Read32(out.headers, i * kHeaderEntry + kHash));

    LOG_INFO("texture pack rebuilt: %zu textures (was %zu), %zu bytes of pixels "
             "(was %zu)", total, count, out.pixels.size(), original.pixels.size());
    return true;
}
