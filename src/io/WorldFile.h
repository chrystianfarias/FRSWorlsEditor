#pragma once

#include "core/Types.h"

#include <map>
#include <string>
#include <vector>

// A world as one file (.frsworld): every edit of it, as its source.
//
// <game>\models is where the game reads a world from -- the export. The world
// file holds what that folder is made from, item by item, so a world can be
// kept anywhere, passed on, and opened again:
//   models/<path>   each file of the world in <game>\models: the section
//                   overlays (every mesh, object, texture and placement edit,
//                   which the Edits tab lists one by one), the collision log
//                   (one entry per collision change), the road network and
//                   terrain sources, markers, replaced game files...
//   meta            who wrote it and when.
// Opening a world writes its files back into <game>\models; saving one reads
// them from there. An extension can keep entries of its own under any other
// name with Put / Get.
//
// Layout: "FRSW", u32 version (1), u32 entry count, then per entry u32 name
// length, the UTF-8 name, u64 size and the bytes.
namespace io {

class WorldFile {
public:
    bool Load(const std::wstring& path, std::wstring& error);
    // Written beside the target and renamed over it, so a failed save never
    // leaves half a world.
    bool Save(const std::wstring& path, std::wstring& error) const;

    void Put(const std::string& name, std::vector<u8> bytes) { entries_[name] = std::move(bytes); }
    bool Get(const std::string& name, std::vector<u8>& bytes) const;
    bool Has(const std::string& name) const { return entries_.count(name) != 0; }
    void Remove(const std::string& name) { entries_.erase(name); }
    std::vector<std::string> Names() const;
    void Clear() { entries_.clear(); }

private:
    std::map<std::string, std::vector<u8>> entries_;
};

// Every file of the open world in `modelsFolder` (all but the shared ones,
// see WorldProfiles.h) into `file` as models/<path>. Replaces the models/
// entries the file had.
bool PackWorld(const std::wstring& modelsFolder, WorldFile& file, size_t& files, std::wstring& error);

// Moves the open world's files out of `modelsFolder` into `backupFolder`
// (nothing is deleted), then writes the file's models/ entries in their place.
// An empty `file` leaves the folder with no edits: the game as shipped.
bool UnpackWorld(const WorldFile& file, const std::wstring& modelsFolder, const std::wstring& backupFolder,
                 size_t& files, std::wstring& error);

// A cheap fingerprint of the world's files in `modelsFolder` -- names, sizes
// and times -- to tell whether they changed since the world file was written.
std::string WorldSignature(const std::wstring& modelsFolder);

// Whether the world in `modelsFolder` has any file at all.
bool WorldHasFiles(const std::wstring& modelsFolder);

} // namespace io
