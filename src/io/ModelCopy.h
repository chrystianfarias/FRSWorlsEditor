#pragma once

#include "core/Types.h"
#include "io/TextureImport.h"

#include <string>
#include <vector>

namespace nfs { class World; struct Texture; }

// Copying a model from one section into another, so it can be placed there.
//
// No shipped scenery places a model that lives in a different positioned
// section: the game only has a section's models while that section is
// streamed in. A model is taken somewhere new by giving the destination its
// own copy -- the solid object chunk byte for byte, and any texture it uses
// that is not already somewhere the destination can see (the region's
// library, or the destination's own pack).

// Appends `object` (a whole 0x80134010 chunk that sits at `objectOffset` in
// its own file) to the solid list pack `pack` (a whole 0x80134000 chunk that
// will land at `packOffset`), and lists it in the pack's header: the object
// count at 0x0C of 0x00134002 and a (hash, 0) pair at its sorted place in
// 0x00134003. Everything inside an object is aligned by file position, to at
// most 128 bytes, so the copy is put on the same position modulo 128 as the
// original and the shipped objects keep theirs; zero-id padding chunks, which
// the format already uses between objects, make up the difference.
bool AppendSolidObject(ByteSpan pack, u64 packOffset, ByteSpan object, u64 objectOffset,
                       u32 hash, std::vector<u8>& out, std::wstring& error);

// Finds the bytes of a model's object chunk as the editor has them, and where
// they sit. Searches the section the model is stored in.
bool FindModelChunk(const nfs::World& world, u32 hash, ByteSpan& object, u64& fileOffset,
                    u32* packOrder = nullptr);

// Whether `targetSection` can resolve a texture without help: it is in a
// library pack, which the game keeps loaded everywhere, or in the section's
// own pack.
bool TextureVisibleTo(const nfs::World& world, u32 hash, size_t targetSection);

// The game texture an exported TGA stands for: the export names the file after
// the texture, with anything but letters, digits, '_' and '-' made '_'.
const nfs::Texture* FindGameTextureByFile(const nfs::World& world, const std::string& stem);

// A game texture as it is stored -- the same compressed levels, not a re-encode.
bool CopyGameTexture(const nfs::Texture& source, NewTexture& copy);

// The textures a model uses that `targetSection` could not resolve on its
// own: not in the region's library packs and not in its own. They come back
// ready to be added to the target's pack.
std::vector<NewTexture> TexturesToCopy(const nfs::World& world, u32 hash, size_t targetSection,
                                       std::wstring& problem);
