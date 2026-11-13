#pragma once

#include "core/Types.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace nfs { struct SolidObject; }
class WorldRenderer;

// Pictures of models for the catalogue, drawn once and kept in a pack.
//
// A region has thousands of models and a picture takes a millisecond or two,
// so each is drawn the first time the catalogue wants it and kept. They are
// kept in one file per region, %LOCALAPPDATA%\FRSWorldEditor\thumbs\
// <region>.frsthumbs, rather than one file each: L4RA's 4,200 pictures made a
// folder of 55 MB on disk for 13 MB of PNG, and took as long to copy as to
// draw again. A pack of the same name beside the editor, when there is one,
// is read as well, so a set drawn once can travel with it.
//
// The pack is a header and then records appended as pictures are drawn:
//
//   header  char[8] "FRSTHUMB", u32 version (how pictures are drawn),
//           u32 picture size in pixels
//   record  u32 'FRST', u32 model hash, u64 mesh key, u32 PNG length, PNG
//
// The mesh key is made from the mesh itself -- its vertices and its textures
// -- so a model edited in Blender gets a new picture rather than its old one;
// the newest record of a model wins. A record cut short by a crash ends the
// pack there. When superseded pictures come to outweigh the live ones, the
// pack is written again without them.
class ModelThumbnails {
public:
    static constexpr u32 kSize = 128;

    ~ModelThumbnails();

    void SetRegion(const std::wstring& region);

    // `kSize` x `kSize` BGRA pixels, top row first. From a pack when a
    // picture of this very mesh is in one, drawn by `renderer` and kept
    // otherwise.
    bool Get(WorldRenderer& renderer, const nfs::SolidObject& model, std::vector<u32>& bgra);

    // True when a picture of this very mesh is already kept.
    bool Cached(const nfs::SolidObject& model) const;

private:
    struct Entry {
        u64  key = 0;
        u64  offset = 0;       // of the PNG in its pack
        u32  length = 0;
        bool shipped = false;  // in the pack beside the editor, not the user's
    };

    void Close();
    void Load(const std::wstring& path, bool shipped);
    void ImportFolder(const std::wstring& folder);
    void Compact();
    bool Append(u32 hash, u64 key, const std::vector<u8>& png);
    bool ReadPng(const Entry& entry, std::vector<u8>& png);

    std::wstring region_;
    std::wstring packPath_;       // the user's pack, written to
    std::wstring shippedPath_;    // the one beside the editor, read only
    std::unordered_map<u32, Entry> index_;
    FILE* pack_ = nullptr;        // the user's, open for reading
    FILE* shipped_ = nullptr;
    u64   liveBytes_ = 0, totalBytes_ = 0;
};
