#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

// Publishing a world: a .zip that anyone can unpack into their game folder and
// play, with no editor and no world file.
//
// <game>\models holds two kinds of file: what the game reads (section and mesh
// overlays, whole replaced files under models\files) and what only the editor
// reads (the collision log, road and terrain sources, hidden items...). A
// published world carries the first kind only, so it is the result of the
// work and not its source: it cannot be opened in the editor again.
//
// The zip unpacks over the game folder:
//   models\...                       the world's game files
//   models\frs-world.txt             name, author, and the fingerprint of every
//                                    track file the world edits (see
//                                    core/FileFingerprint.h): the loader leaves
//                                    a region's edits out on a game whose file
//                                    differs, rather than crash it
//   scripts\FRSWorldLoader.asi/.ini  what puts it into the game
//   Uninstall <world>.bat            removes exactly the world's files
//   <world> - README.txt             what it is and how to install it
//   ASI loader (only if needed)\     dinput8.dll, for a game without a loader
namespace io {

// A pattern of files in <game>\models (relative, '\' between folders; '*'
// stays within a folder, '**' crosses them) that a published world takes, for
// what an extension writes there for a plugin of its own to read. `note` is
// written into the README when a file matches -- say, the mod it needs.
// The editor's own: *.frsmesh, *.frssection, files\**.
void AddPublishedWorldFiles(const std::wstring& pattern, const std::wstring& note = L"");

struct PublishedFile {
    std::wstring relative;   // below <game>\models
    std::wstring path;
    u64 size = 0;
};

// What a published world made from `modelsFolder` would carry.
std::vector<PublishedFile> CollectPublishedFiles(const std::wstring& modelsFolder);

struct PublishRequest {
    std::wstring gameFolder;
    std::wstring name;            // also names the README and the uninstaller
    std::wstring author;
    std::wstring description;
    std::wstring editorVersion;
    // Copies of FRSWorldLoader.asi (and its .ini beside each) to choose from;
    // the newest by version goes in, the earlier on a tie.
    std::vector<std::wstring> loaderCandidates;
    std::wstring asiLoader;       // dinput8.dll, or empty
    std::wstring zipPath;
};

struct PublishResult {
    size_t files = 0;             // world files
    u64 bytes = 0;                // of the zip
    std::vector<std::wstring> regions;
    std::wstring loaderUsed;
    std::wstring loaderVersion;
};

bool PublishWorld(const PublishRequest& request, PublishResult& result, std::wstring& error);

// "a.b.c.d" from a file's version resource, or empty.
std::wstring FileVersionText(const std::wstring& path);

} // namespace io
