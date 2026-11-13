#pragma once

#include "core/Types.h"
#include "core/Math.h"

#include <string>
#include <unordered_map>
#include <vector>

// An edited mesh, kept beside the game rather than inside it.
//
// The game's own files are never written. Each edited mesh goes into its own
// file under <game>\models, and two readers apply them: the editor, so the
// viewport shows what the game will show, and an ASI that patches the bytes as
// the game reads them off disk. The original .BUN stays byte for byte as it
// shipped, which makes an edit something you can undo by deleting a file.
//
// The payload is positions only, in the space the mesh is stored in, one for
// one with the vertices already in the file. That is what lets the patch be a
// straight overwrite of the bytes the game just read: nothing changes size, so
// no chunk moves and the section table stays true. A mesh whose vertex count
// changed cannot be carried this way.
#pragma pack(push, 1)
struct MeshOverlayHeader {
    char magic[8];        // "FRSMESH1"
    char region[8];       // "L4RA", zero padded
    u32  hash;            // the solid object's hash
    u32  stride;          // bytes between vertices in the stream
    u32  vertexCount;
    u32  reserved;
    u64  fileOffset;      // where the vertices sit in STREAM<region>.BUN
    char name[64];        // the model's name, for a person reading the folder
};
#pragma pack(pop)

static_assert(sizeof(MeshOverlayHeader) == 104, "the overlay header is a fixed 104 bytes");

struct MeshOverlay {
    MeshOverlayHeader header{};
    std::vector<Vec3> positions;
};

// Writes one overlay into `modelsFolder`, named after the model.
bool WriteMeshOverlay(const std::wstring& modelsFolder, const MeshOverlay& overlay);

// A span of the game's files that reads differently.
//
// This is what a mesh whose vertex count changed turns into: the section
// around it, rebuilt, as one run of bytes. The same shape also carries the
// correction to the region's table when a section had to move.
#pragma pack(push, 1)
struct SpanOverlayHeader {
    char magic[8];        // "FRSSECT1"
    char region[8];
    u32  byteCount;
    u32  target;          // 0 = the stream, 1 = the layout file beside it
    u64  fileOffset;
    char name[64];
};
#pragma pack(pop)

static_assert(sizeof(SpanOverlayHeader) == 96, "the span header is a fixed 96 bytes");

// Writes one span into `modelsFolder`. `key` only names the file.
bool WriteSpanOverlay(const std::wstring& modelsFolder, const std::wstring& region,
                      const std::string& name, u64 fileOffset, bool layoutFile,
                      const std::vector<u8>& bytes, const std::wstring& key);

// One overlay as it sits on disk, for showing and removing.
struct MeshOverlayInfo {
    std::wstring path;
    std::string  model;
    std::string  region;
    u32          hash        = 0;
    u32          vertexCount = 0;
};

// Lists every overlay in `modelsFolder`, whatever region it belongs to, so a
// stale one from another map can be seen and removed rather than sitting there
// silently doing nothing.
size_t ListMeshOverlays(const std::wstring& modelsFolder,
                        std::vector<MeshOverlayInfo>& out);

// Reads every overlay in `modelsFolder` that belongs to `region`, keyed by hash.
size_t ReadMeshOverlays(const std::wstring& modelsFolder, const std::wstring& region,
                        std::unordered_map<u32, std::vector<Vec3>>& out);

// One span read back off disk, for the editor to show what the game will see.
struct SpanOverlay {
    u64             fileOffset = 0;
    bool            layoutFile = false;
    std::vector<u8> bytes;
};

// One span file as it sits on disk, for replacing or removing it.
struct SpanOverlayInfo {
    std::wstring path;
    std::string  name;
    u64          fileOffset = 0;
    u32          byteCount  = 0;
    bool         layoutFile = false;
};

// Lists the span files in `modelsFolder` that belong to `region`.
size_t ListSpanOverlays(const std::wstring& modelsFolder, const std::wstring& region,
                        std::vector<SpanOverlayInfo>& out);

// Reads every span in `modelsFolder` belonging to `region`.
size_t ReadSpanOverlays(const std::wstring& modelsFolder, const std::wstring& region,
                        std::vector<SpanOverlay>& out);
