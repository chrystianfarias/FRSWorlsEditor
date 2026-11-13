#pragma once

#include "core/Types.h"
#include "io/TextureImport.h"

#include <string>
#include <vector>

// Adds textures to a section's texture pack.
//
// A pack is four parallel tables plus a block of pixels: a hash table the game
// searches, a 124-byte header per texture, a 32-byte format entry beside it, and
// the compressed levels themselves. Nothing records how many textures there are
// -- the count is each table's length divided by its entry size -- so adding one
// means appending to all four consistently and nothing else.
//
// The one ordering constraint is the hash table. It is sorted, which is only
// worth doing if something binary-searches it, so a new entry goes in at its
// sorted position rather than at the end and the indices it stores are fixed up
// around it.
struct TpkTables {
    std::vector<u8> hashTable;   // 0x33310002, eight bytes per texture
    std::vector<u8> headers;     // 0x33310004, 124 bytes per texture
    std::vector<u8> formats;     // 0x33310005, 32 bytes per texture
    std::vector<u8> pixels;      // 0x33320002, every texture's levels
};

// Builds the new tables. `original` is the pack as it stands; `added` is what to
// put in it. A texture whose hash is already in the pack replaces it in place
// rather than being appended, so importing the same folder twice is harmless.
//
// Returns false and fills `error` only when the original tables are malformed --
// a header table whose length is not a whole number of entries, say, which would
// mean the pack was not understood rather than that the new texture is bad.
bool BuildTpkTables(const TpkTables& original, const std::vector<NewTexture>& added,
                    TpkTables& out, std::wstring& error);
