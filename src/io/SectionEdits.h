#pragma once

#include "core/Types.h"
#include "io/SectionSplice.h"
#include "io/TextureImport.h"
#include "nfs/SceneryEdit.h"

#include <string>
#include <vector>

namespace nfs { class World; struct Section; struct CollisionPack; }

// The edits in force on a section, one by one.
//
// A span overlay holds a whole section with every edit already applied, which
// is what the game needs but says nothing about which edits those are. They
// are recovered by comparing the section the editor shows with the one the
// game shipped, chunk by chunk: an object whose bytes changed is an edited
// mesh, one the shipped list lacks is a model copied in, a texture the shipped
// pack lacks was added, a changed collision pack is a collision edit, scenery
// records past the shipped ones are placements and shipped records that
// changed are objects deleted or moved. Each such difference is one item, and
// a section is rebuilt from the shipped bytes with whichever items are kept --
// which is how a single edit is reverted without touching the others, and how
// every save works, so that no kind of edit can undo another.
struct SectionEdit {
    enum class Kind {
        MeshChanged,      // an object's chunk differs from the shipped one
        ModelAdded,       // an object the shipped list does not have
        TextureAdded,
        TextureChanged,
        Collision,        // a collision pack differs
        Placement,        // a scenery record past the shipped ones
        NativeDeleted,    // a shipped scenery record with its matrix zeroed
        NativeChanged,    // a shipped scenery record otherwise altered
        Emitters,         // the section's placed effects (0x35020) differ, or were added
        Other,            // a difference the editor does not know how to separate
    };

    Kind        kind = Kind::Other;
    std::string label;          // what a person reads: a model or texture name
    std::wstring detail;

    u32 hash = 0;               // object or texture
    u32 order = 0;              // which chunk of its kind, in file order
    u32 sceneryNumber = 0;
    u32 index = 0;              // record index within its scenery chunk

    std::vector<u8> chunk;      // object or collision chunk, header included
    u32 mod128 = 0;             // file position mod 128 `chunk` was laid out for
    NewTexture texture;
    nfs::PlacedInstance placement;
    std::vector<u8> record;     // 64-byte scenery record, for shipped ones;
                                // for Emitters, the chunk's payload without its filler

    bool Revertible() const { return kind != Kind::Other; }
};

const wchar_t* EditKindName(SectionEdit::Kind kind);

// Everything that differs between the section as the editor shows it and as
// the game shipped it. Empty when the section is untouched.
bool DiffSection(const nfs::World& world, const nfs::Section& section,
                 std::vector<SectionEdit>& out, std::wstring& error);

// The same comparison for any bytes standing in for the section at `fileOffset`.
bool DiffSectionBytes(const nfs::World& world, const nfs::Section& section, ByteSpan bytes,
                      u64 fileOffset, std::vector<SectionEdit>& out, std::wstring& error);

// Checks, for every section that carries edits, that rebuilding it from the
// shipped bytes with all of them gives the same edits back, and with none of
// them gives the shipped bytes. Logs one line per section.
void SelfTestSectionEdits(const nfs::World& world);

// The shipped section with `edits` applied. Every edit is laid back on the
// file position modulo 128 it was made for, and everything else keeps its
// shipped alignment, so the result is valid wherever a 2 KiB aligned section
// can go.
bool BuildSection(const nfs::World& world, const nfs::Section& section,
                  const std::vector<SectionEdit>& edits, std::vector<u8>& out,
                  std::wstring& error);

// Rebuilds the section with `edits` and plans the spans that put it in the
// game -- or, with no edits left, removes the section's spans so the game
// reads the shipped bytes again.
bool PlanSectionFromEdits(const nfs::World& world, const nfs::Section& section,
                          const std::vector<SectionEdit>& edits,
                          const std::vector<SpanOverlayInfo>& existing,
                          SectionSpanPlan& out, std::wstring& error);

// The payload, alignment filler removed, of the section's first shipped
// top-level chunk `id`. False when the section shipped without one.
bool ShippedChunkData(const nfs::World& world, const nfs::Section& section, u32 id,
                      std::vector<u8>& data);

// The section's collision packs as the game shipped them, in the order the
// loader lists them (LoadedSection::collision).
bool ShippedCollisionPacks(const nfs::World& world, const nfs::Section& section,
                           std::vector<nfs::CollisionPack>& packs);

// A zero-id chunk that brings the next write to `wanted` modulo 128.
void PadToModulo(std::vector<u8>& out, u64 base, u64 wanted);
