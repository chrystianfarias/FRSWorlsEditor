#pragma once

#include "core/Types.h"
#include "nfs/TexturePack.h"

#include <vector>

namespace nfs {

// Decodes a texture to 32-bit BGRA on the CPU.
//
// The renderer hands the compressed data straight to Direct3D, but a preview
// needs real pixels, and so does anything that exports a texture. Keeping the
// decoder here also means the formats can be checked without a GPU.
//
// `level` selects a mip level; 0 is the full-size image. Returns false when the
// format is unsupported or the data is short.
bool DecodeTexture(const Texture& texture, u32 level,
                   std::vector<u32>& outPixels, u32& outWidth, u32& outHeight);

// Bytes one mip level occupies in the packed source data.
u32 TextureLevelSize(const Texture& texture, u32 level);

} // namespace nfs
