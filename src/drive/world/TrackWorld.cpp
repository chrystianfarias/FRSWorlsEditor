// Port of NFSU2Unity Core/World/TrackWorld.cs (editor variant: built once from in-memory sections).
#include "drive/world/TrackWorld.h"

#include "drive/numerics/BWareMath.h"

#include <cctype>
#include <cfloat>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace bb {

namespace {

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& bytes)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

TrackWorld::TrackWorld(const Speed2Image& exe, const std::vector<uint8_t>& layoutFile,
                       const std::vector<SectionCollisionInput>& sections, const std::wstring& gameDirectory)
    : gameDir(gameDirectory)
{
    TopologyK::Verify(exe);
    for (int t = 0; t < TerrainCount; t++) {
        noiseFrequency[t] = exe.F32(VA_TerrainTypeInfoTable + static_cast<uint32_t>(TerrainStride * t) + 0x18);
        noiseAmplitude[t] = exe.F32(VA_TerrainTypeInfoTable + static_cast<uint32_t>(TerrainStride * t) + 0x1C);
    }
    for (int i = 0; i < 16; i++) noiseLattice[i] = exe.F32(VA_RoadNoiseTable + static_cast<uint32_t>(4 * i));

    CollisionK::Load(exe);
    volumes = GjkPolytope::ReadAll(layoutFile);
    Topology = std::make_unique<TopologyManagerU2>(VisibleSectionsU2::Read(layoutFile), TopologyGroupsU2::Read(layoutFile));
    defaultCoordinate = Topology->CreateCoordinate();
    spawnCoordinate = Topology->CreateCoordinate();

    // The packs (the editor's "loaded sections"), then the candidates: main set first, then the alternates.
    for (const SectionCollisionInput& s : sections) {
        if (s.Collision.empty()) continue;
        packs[s.Index] = std::make_unique<TrackCollisionU2>(TrackCollisionU2::Read(s.Collision.data(), s.Collision.size()));
    }
    for (const SectionCollisionInput& s : sections) if (!s.IsAlternate) Candidate(s.Number, s.Index);
    for (const SectionCollisionInput& s : sections) if (s.IsAlternate) Candidate(s.Number, s.Index);
    Sync();
}

TrackWorld::~TrackWorld()
{
    // the trees go before the manager that points to them
    if (Topology) {
        for (auto& kv : slotTree) Topology->UnloadTree(kv.second.get());
    }
    slotTree.clear();
}

void TrackWorld::Candidate(int number, int index)
{
    int slot = Topology->Slot(static_cast<int16_t>(number));
    auto it = slotIndex.find(slot);
    if (it == slotIndex.end()) {
        slotIndex[slot] = slotSections.size();
        slotSections.push_back({ slot, {} });
        it = slotIndex.find(slot);
    }
    slotSections[it->second].second.push_back(index);
}

void TrackWorld::Sync()
{
    if (synced) return;
    synced = true;
    for (auto& kv : slotSections) {
        int wantIndex = -1;
        const TrackCollisionU2* pack = nullptr;
        for (int idx : kv.second) {
            auto p = packs.find(idx);
            if (p == packs.end() || !p->second) continue;
            wantIndex = idx;
            pack = p->second.get();
            break;
        }
        if (pack == nullptr) continue;
        auto tree = std::make_unique<TopologyTree>(wantIndex, pack);
        TopologyTree* t = tree.get();
        slotTree[kv.first] = std::move(tree);
        Topology->LoadTree(t);
    }
}

// ------------------------------------------------------------------ ITrackTopology

void* TrackWorld::CreateTopologyCoordinate() { return Topology->CreateCoordinate(); }

void TrackWorld::SetSectionPosition(void* coordinate, float x, float y, float slack)
{
    Sync();
    Topology->SetSectionPosition(Coordinate(coordinate), x, y, slack);
}

bool TrackWorld::IsConnected(void* coordinate) { return Coordinate(coordinate)->Tree != nullptr; }

TopologyCoordinate* TrackWorld::Coordinate(void* o) const
{
    return o != nullptr ? static_cast<TopologyCoordinate*>(o) : defaultCoordinate;
}

int TrackWorld::ConnectedSection(void* coordinate)
{
    TopologyCoordinate* c = Coordinate(coordinate);
    return c->Tree != nullptr ? c->Tree->Number : 0;
}

// ------------------------------------------------------------------ IWorldCollision

bool TrackWorld::UpdateInterest(void* coordinate, float minX, float minY, float maxX, float maxY, const float* velocity)
{
    Sync();
    return Topology->UpdateInterest(Coordinate(coordinate), minX, minY, maxX, maxY, velocity);
}

int TrackWorld::InterestCount(void* coordinate) { return Coordinate(coordinate)->InterestCount; }

TrackPolygonShape* TrackWorld::Interest(void* coordinate, int i) { return Coordinate(coordinate)->Interest[i]; }

GjkPolytope* TrackWorld::FindCollisionVolume(const std::string& carName, uint32_t nameHash)
{
    if (!carName.empty()) {
        std::string key = carName;
        for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (carsLoaded.insert(key).second && !gameDir.empty()) {
            std::wstring car(carName.begin(), carName.end());
            std::vector<uint8_t> bytes;
            if (ReadFile(gameDir + L"\\CARS\\" + car + L"\\GEOMETRY.BIN", bytes)) {
                auto more = GjkPolytope::ReadAll(bytes);
                for (auto& v : more) volumes.push_back(std::move(v));
            }
        }
    }
    for (auto& v : volumes) if (v->NameHash == nameHash) return v.get();
    return nullptr;
}

// ------------------------------------------------------------------ IWheelWorld

float TrackWorld::GetElevation(void* topology, const float* point, int& terrain, float* normal, bool& valid)
{
    Sync();
    int t = 0;
    F24 h = Topology->NewGetElevation(Coordinate(topology), point[0], point[1], point[2], t, normal, valid);
    terrain = t;
    return h.Store();
}

float TrackWorld::GetRoadNoise(const float* pointXY, int terrain)
{
    if (terrain < 0 || terrain >= TerrainCount) return 0.0f;   // (the C# throws: no such TerrainTypeInfoTable row)
    float freq = noiseFrequency[terrain], amp = noiseAmplitude[terrain];
    F24 x = F24(freq) * pointXY[0];
    F24 y = F24(pointXY[1]) * freq;
    int ix0 = X87::FtoL2Lo(x.V);
    float fx = static_cast<float>(ix0);                                     // fild; fst dword
    if ((BWareMath2::Fcom(static_cast<double>(ix0), x.V) & 0x41) == 0)    // fcomp st(2): exact integer > x
        fx = (F24(fx) - TopologyK::One).Store();
    int iy0 = X87::FtoL2Lo(y.V);
    F24 fy = static_cast<float>(iy0);                                      // fild (kept on the stack)
    if ((BWareMath2::Fcom(static_cast<double>(iy0), y.V) & 0x41) == 0) fy = fy - TopologyK::One;
    int ix = X87::FtoL2Lo(F24(fx).V);
    int iy = X87::FtoL2Lo(fy.V);
    float fracX = (x - fx).Store();
    F24 fracY = y - fy;
    F24 gy = F24(TopologyK::One) - fracY;
    F24 gx = F24(TopologyK::One) - fracX;
    F24 h00 = noiseLattice[Hash(ix, iy)], h10 = noiseLattice[Hash(ix + 1, iy)];
    F24 h01 = noiseLattice[Hash(ix, iy + 1)], h11 = noiseLattice[Hash(ix + 1, iy + 1)];
    F24 s = h00 * gx * gy;
    s = s + h10 * gy * fracX;
    s = s + h01 * gx * fracY;
    s = s + h11 * fracY * fracX;
    return (s * amp).Store();
}

int TrackWorld::Hash(int i, int j)
{
    uint32_t ui = static_cast<uint32_t>(i), uj = static_cast<uint32_t>(j);
    uint32_t v = static_cast<uint32_t>(-5) * ui * uj - 3u * (ui + 1u) * (uj + 1u);
    return static_cast<int>(v & 15u);
}

// ------------------------------------------------------------------ helpers

bool TrackWorld::TryFindGround(float x, float y, float zHint, float& z)
{
    Sync();
    TopologyCoordinate* c = spawnCoordinate;
    ResetCoordinate(c);
    Topology->SetSectionPosition(c, x, y, 0.0f);
    int terrain = 0;
    bool valid = false;
    F24 h = Topology->NewGetElevation(c, x, y, zHint, terrain, nullptr, valid);
    if (!valid) {
        // outside the boundaries (or its section is not loaded): try each loaded section's grid
        c->PosX = x;
        c->PosY = y;
        c->Radius = TopologyK::FltMax;
        for (TopologyTree* tree : Topology->Trees()) {
            Topology->Connect(c, tree);
            h = Topology->NewGetElevation(c, x, y, zHint, terrain, nullptr, valid);
            if (valid) break;
        }
    }
    ResetCoordinate(c);
    z = valid ? h.Store() : zHint;
    return valid;
}

void TrackWorld::ResetCoordinate(TopologyCoordinate* c)
{
    Topology->Disconnect(c);
    c->Boundary = nullptr;
    c->PosX = c->PosY = 0.0f;
    c->Radius = TopologyK::InitialRadius;
}

bool TrackWorld::TryNearestGround(float x, float y, Vec3& point) const
{
    point = Vec3(x, y, 0.0f);
    float best = FLT_MAX;
    for (const auto& kv : slotSections) {
        auto t = slotTree.find(kv.first);
        if (t == slotTree.end()) continue;
        Vec3 p;
        if (!t->second->Pack->TryNearestGround(x, y, p)) continue;
        float d = (p.X - x) * (p.X - x) + (p.Y - y) * (p.Y - y);
        if (d < best) {
            best = d;
            point = p;
        }
    }
    return best < FLT_MAX;
}

bool TrackWorld::TryGroundHeight(float x, float y, float maxZ, float& z) const
{
    bool found = false;
    z = 0.0f;
    for (const auto& kv : slotTree) {
        float h;
        if (kv.second->Pack->TryGetGroundHeight(x, y, maxZ, h) && (!found || h > z)) {
            z = h;
            found = true;
        }
    }
    return found;
}

} // namespace bb
