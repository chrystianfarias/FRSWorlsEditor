#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

// Textures that come in beside an edited model.
//
// The convention is a folder named after the OBJ with the images inside, so
// `airport.obj` is accompanied by `airport\`. A file called `MY_WALL.png` there
// becomes a texture named MY_WALL, and a material in the OBJ that says
// `usemtl MY_WALL` finds it. That keeps the naming in one place -- the file
// system -- rather than in a manifest someone has to keep in step by hand.
//
// The name is what matters, because the game finds a texture by a hash of its
// name and nothing else. The hash is the one the game itself uses: start at
// 0xFFFFFFFF and fold each byte in with h = h * 33 + c. Confirmed against 1,451
// of the 1,611 textures in L4RA; the rest differ only because their stored names
// are cut at 23 characters, so there is no full name left to hash.
struct NewTexture {
    std::string     name;
    u32             hash   = 0;
    u32             width  = 0;
    u32             height = 0;
    u32             fourcc = 0;    // 'DXT1' or 'DXT5'
    bool            hasAlpha = false;

    // Every mip level, tightly packed, largest first -- the layout the pack
    // stores and the game uploads straight from.
    //
    // The chain is not optional. A shipped 256x256 track texture carries six
    // levels, and a surface handed to the game with one would be sampled at
    // full size from any distance: shimmering on every roof and road sign, and
    // the whole texture resident in memory for a building at the horizon.
    std::vector<u8> pixels;
    u32             mipCount = 1;
    u32             baseLevelSize = 0;   // bytes of the largest level alone

    // Read from a TGA, the format the editor exports the game's own textures
    // in -- so possibly one of them coming back rather than a new one.
    bool            fromTga = false;
};

// The hash the game looks a texture up by.
u32 TextureNameHash(const std::string& name);

// Reads every image in the folder beside `objPath`. Returns false only when the
// folder is there and something in it could not be read; a missing folder is
// simply no new textures.
bool LoadTextureFolder(const std::wstring& objPath, std::vector<NewTexture>& out,
                       std::wstring& error);

// Reads one image as a texture named after the file, upper case, without its
// extension (SHOP_ICON_GAS.png -> SHOP_ICON_GAS), block-compressed with mips.
bool LoadTextureFile(const std::wstring& path, NewTexture& out, std::wstring& error);

// A texture from pixels already in memory (BGRA, top row first), with its
// mips. Both sides must be multiples of four.
void TextureFromPixels(const std::string& name, const std::vector<u32>& bgra, u32 width,
                       u32 height, NewTexture& out);
