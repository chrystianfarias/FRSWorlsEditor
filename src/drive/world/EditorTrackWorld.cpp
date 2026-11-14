// Editor -> bb::TrackWorld bridge and the CityDrive spawn helpers.
#include "drive/world/EditorTrackWorld.h"

#include "nfs/Collision.h"
#include "nfs/SectionTable.h"
#include "nfs/World.h"

#include <windows.h>

#include <exception>
#include <fstream>
#include <iterator>

namespace bb {

namespace {

std::wstring Widen(const char* s)
{
    std::wstring w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

bool FileExists(const std::wstring& path)
{
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ParentFolder(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

} // namespace

bool GatherTrackWorldInputs(nfs::World& world, std::vector<uint8_t>& layout,
                            std::vector<SectionCollisionInput>& sections, std::wstring& error)
{
    layout.clear();
    sections.clear();
    if (!world.IsOpen()) { error = L"No region is open."; return false; }

    // The layout file as the game reads it: the shipped bytes with the editor's layout spans laid over.
    const ByteSpan shipped = world.LayoutBytes();
    layout.assign(shipped.begin(), shipped.end());
    for (const SpanOverlay& span : world.Spans()) {
        if (!span.layoutFile || span.bytes.empty()) continue;
        const size_t end = static_cast<size_t>(span.fileOffset) + span.bytes.size();
        if (end > layout.size()) layout.resize(end, 0);
        std::copy(span.bytes.begin(), span.bytes.end(), layout.begin() + static_cast<std::ptrdiff_t>(span.fileOffset));
    }

    // Every positioned section with collision (TrackWorld.cs skips placeholders, the library and empty entries).
    const std::vector<nfs::Section>& table = world.Table().Sections();
    for (size_t i = 0; i < table.size(); ++i) {
        const nfs::Section& s = table[i];
        const bool placeholder = s.number == 0 && s.name == "--";
        const bool library = s.position.x == 0.0f && s.position.y == 0.0f;
        if (placeholder || library || s.streamSize == 0) continue;
        const nfs::LoadedSection* loaded = world.LoadSection(s);
        if (!loaded || loaded->collision.empty()) continue;
        // TrackWorld.cs takes the section's last pack.
        const nfs::CollisionPack& pack = loaded->collision.back();
        SectionCollisionInput in;
        in.Number = static_cast<int>(s.number);
        in.Index = static_cast<int>(i);
        in.IsAlternate = s.IsAlternate();
        std::wstring encodeError;
        if (!nfs::EncodeCollision(pack, in.Collision, encodeError)) {
            error = L"The collision of section " + Widen(s.name.c_str()) + L" cannot be encoded: " + encodeError;
            return false;
        }
        sections.push_back(std::move(in));
    }
    if (sections.empty()) { error = L"The region has no collision."; return false; }
    return true;
}

std::unique_ptr<TrackWorld> BuildTrackWorld(nfs::World& world, const Speed2Image& exe, std::wstring& error,
                                            const std::wstring& gameFolder)
{
    std::vector<uint8_t> layout;
    std::vector<SectionCollisionInput> sections;
    if (!GatherTrackWorldInputs(world, layout, sections, error)) return nullptr;
    std::wstring folder = gameFolder;
    if (folder.empty() && !world.OverlayFolder().empty()) {
        const std::wstring parent = ParentFolder(world.OverlayFolder());
        if (!parent.empty() && FileExists(parent + L"\\SPEED2.EXE")) folder = parent;
    }
    try {
        return std::make_unique<TrackWorld>(exe, layout, sections, folder);
    } catch (const std::exception& e) {
        error = L"The track topology could not be built: " + Widen(e.what());
        return nullptr;
    }
}

bool SpawnGroundHeight(TrackWorld& track, float x, float y, float zHint, float& ground)
{
    float z = 0.0f;
    if (track.TryFindGround(x, y, zHint, z) || track.TryFindGround(x, y, 10000.0f, z)) {
        ground = z;
        return true;
    }
    return false;
}

bool NearestGround(TrackWorld& track, float x, float y, float& outX, float& outY, float& outZ)
{
    Vec3 g;
    if (!track.TryNearestGround(x, y, g)) return false;
    float z = g.Z;
    float gz;
    if (track.TryFindGround(g.X, g.Y, g.Z + 1.0f, gz)) z = gz;
    outX = g.X;
    outY = g.Y;
    outZ = z;
    return true;
}

bool FindFreeRoamStart(const std::wstring& gameFolder, const std::wstring& region, TrackPositionMarkerU2& marker)
{
    const std::wstring path = gameFolder + L"\\TRACKS\\ROUTES" + region + L"\\TrackPosMarkersFreeRoam.bin";
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto markers = TrackPositionMarkersU2::Read(bytes);
    return TrackPositionMarkersU2::TryFind(markers, TrackPositionMarkersU2::ForwardStartingPosition,
                                           TrackPositionMarkersU2::FreeRoamTrackId, 0, marker);
}

} // namespace bb
