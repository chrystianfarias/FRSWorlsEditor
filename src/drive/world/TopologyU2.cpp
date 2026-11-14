// Port of NFSU2Unity Core/World/TopologyU2.cs.
#include "drive/world/TopologyU2.h"

#include "drive/numerics/BWareMath.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace bb {

namespace {

/// fcom + fnstsw: condition bits (AH) for st0 = a against b (TopologyFpu.Fc).
int Fc(F24 a, F24 b) { return BWareMath2::Fcom(a, b); }

template <class T> T Rd(const uint8_t* b, size_t o) { T v; std::memcpy(&v, b + o, sizeof(T)); return v; }

struct RawChunk {
    uint32_t Id = 0;
    size_t DataOffset = 0, Size = 0;
    size_t End() const { return DataOffset + Size; }
};

/// The chunks of [offset, end) (Chunk.Parse, one level; an overrunning chunk ends the walk).
std::vector<RawChunk> Children(const std::vector<uint8_t>& buf, size_t offset, size_t end)
{
    std::vector<RawChunk> list;
    while (offset + 8 <= end) {
        uint32_t id = Rd<uint32_t>(buf.data(), offset);
        int32_t size = Rd<int32_t>(buf.data(), offset + 4);
        if (size < 0 || offset + 8 + static_cast<size_t>(size) > end) break;
        list.push_back({ id, offset + 8, static_cast<size_t>(size) });
        offset += 8 + static_cast<size_t>(size);
    }
    return list;
}

} // namespace

// ---------------------------------------------------------------------------------------------- TopologyK

void TopologyK::Verify(const Speed2Image& exe)
{
    auto C = [&](uint32_t va, float v) {
        if (exe.U32(va) != X87::Bits(v)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SPEED2.EXE constant at 0x%08X is 0x%08X, expected 0x%08X",
                          va, exe.U32(va), X87::Bits(v));
            throw std::runtime_error(buf);
        }
    };
    C(0x784250, One); C(0x7842A0, MinusOne); C(0x7D7D3C, Zero); C(0x784260, Half); C(0x7844F8, Four);
    C(0x7881F4, Eight); C(0x7A52D0, MinusEighth); C(0x7A5180, InvCell); C(0x7875B4, Cell);
    C(0x788164, InvSubCell); C(0x784238, Fixed); C(0x784278, InvFixed); C(0x784358, PointOne);
    C(0x784554, VelocityScale); C(0x787ADC, VelocityMax); C(0x78BAFC, VelocityMin);
}

// ---------------------------------------------------------------------------------------------- layout chunks

VisibleSectionsU2 VisibleSectionsU2::Read(const std::vector<uint8_t>& layoutFile)
{
    VisibleSectionsU2 v;
    const uint8_t* b = layoutFile.data();
    for (const RawChunk& c : Children(layoutFile, 0, layoutFile.size())) {
        if (c.Id != WorldChunks::VisibleSections) continue;
        for (const RawChunk& sub : Children(layoutFile, c.DataOffset, c.End())) {
            if (sub.Id == ChunkHeader && sub.Size >= 4)
                v.DrivableLimit = Rd<int32_t>(b, sub.DataOffset);
            else if (sub.Id == ChunkBoundaries) {
                // sub_5E9C10: the records are walked from the chunk data with no alignment.
                size_t p = sub.DataOffset, end = sub.End();
                while (p < end) {
                    if (p + 0x24 > end) break;
                    int16_t n = Rd<int16_t>(b, p + 0x0A);
                    int count = std::max(0, static_cast<int>(n));
                    if (p + 0x24 + static_cast<size_t>(count) * 8 > layoutFile.size()) break;
                    SectionBoundaryU2 r;
                    r.Section = Rd<int16_t>(b, p + 0x08);
                    r.MinX = Rd<float>(b, p + 0x0C); r.MinY = Rd<float>(b, p + 0x10);
                    r.MaxX = Rd<float>(b, p + 0x14); r.MaxY = Rd<float>(b, p + 0x18);
                    r.X.resize(count);
                    r.Y.resize(count);
                    for (int i = 0; i < count; i++) {
                        r.X[i] = Rd<float>(b, p + 0x24 + 8 * static_cast<size_t>(i));
                        r.Y[i] = Rd<float>(b, p + 0x28 + 8 * static_cast<size_t>(i));
                    }
                    v.Boundaries.push_back(std::move(r));
                    p += 0x24 + static_cast<size_t>(static_cast<int>(n) * 8);
                }
            }
        }
    }
    return v;
}

TopologyGroupsU2 TopologyGroupsU2::Read(const std::vector<uint8_t>& layoutFile)
{
    TopologyGroupsU2 g;
    const uint8_t* b = layoutFile.data();
    for (const RawChunk& c : Children(layoutFile, 0, layoutFile.size())) {
        if (c.Id != ChunkId) continue;
        int n = static_cast<int>(c.Size >> 5);   // sub_5E78A0: count = size >> 5, data at the chunk data offset
        g.Hash.resize(n);
        g.Name.resize(n);
        for (int i = 0; i < n; i++) {
            size_t o = c.DataOffset + 32 * static_cast<size_t>(i);
            g.Hash[i] = Rd<uint32_t>(b, o);
            int len = 0;
            while (len < 23 && b[o + 9 + len] != 0) len++;
            g.Name[i].assign(reinterpret_cast<const char*>(b + o + 9), static_cast<size_t>(len));
        }
        break;
    }
    return g;
}

// ---------------------------------------------------------------------------------------------- TopologyPolygon

F24 TopologyPolygon::EdgeTest(float px, float py) const
{
    F24 m = TopologyK::MinusOne;
    float e0 = ((F24(Y(1)) - Y(0)) * (F24(px) - X(0)) - (F24(py) - Y(0)) * (F24(X(1)) - X(0))).Store();
    if (!((Fc(TopologyK::MinusOne, e0) & 0x41) == 0)) {      // fcomp e0; test ah,41h; jz (-1 > e0 keeps -1)
        m = e0;
        if ((Fc(e0, TopologyK::Zero) & 0x41) == 0) return e0;  // e0 > 0: outside
    }
    float e1 = ((F24(Y(2)) - Y(1)) * (F24(px) - X(1)) - (F24(py) - Y(1)) * (F24(X(2)) - X(1))).Store();
    if (!((Fc(m, e1) & 0x41) == 0)) m = e1;
    if ((Fc(m, TopologyK::Zero) & 0x41) == 0) return m;
    float e2 = ((F24(Y(0)) - Y(3)) * (F24(px) - X(3)) - (F24(py) - Y(3)) * (F24(X(0)) - X(3))).Store();
    if (!((Fc(m, e2) & 0x41) == 0)) m = e2;
    if ((Fc(m, TopologyK::Zero) & 0x41) == 0) return m;
    if (Count != 4) return m;
    float e3 = ((F24(Y(3)) - Y(2)) * (F24(px) - X(2)) - (F24(py) - Y(2)) * (F24(X(3)) - X(2))).Store();
    if (!((Fc(m, e3) & 0x41) == 0)) m = e3;
    return m;
}

F24 TopologyPolygon::PlaneHeight(float px, float py) const
{
    F24 s = F24(Nx) * px + F24(Ny) * py - D;      // the cache path adds n.y*y + n.x*x: same (IEEE addition commutes)
    return s * (F24(TopologyK::MinusOne) / Nz);
}

// ---------------------------------------------------------------------------------------------- TopologyTree

TopologyTree::TopologyTree(int sectionIndex, const TrackCollisionU2* pack)
    : SectionIndex(sectionIndex), Pack(pack),
      Number(static_cast<int16_t>(pack->SectionNumber)),
      Width(static_cast<int16_t>(pack->GridWidth)),
      Height(static_cast<int16_t>(pack->GridHeight)),
      OriginX(pack->OriginX), OriginY(pack->OriginY),
      Raw(pack->RawPolygons),
      PolygonCount(static_cast<int>(pack->RawPolygons.size() >> 5))
{
    expanded.resize(PolygonCount);
    Listed.assign(PolygonCount, 0);
}

void TopologyTree::MarkDead()
{
    for (auto& p : expanded) if (p) p->Alive = false;
}

TopologyPolygon* TopologyTree::Expand(int index)
{
    TopologyPolygon* p = expanded[index].get();
    if (p != nullptr) return p;
    auto owned = std::make_unique<TopologyPolygon>();
    owned->Tree = this;
    owned->Index = index;
    owned->GroupMember = Attr0(index) != 0;
    owned->SortKey = (static_cast<int64_t>(SectionIndex) << 20) | static_cast<int64_t>(static_cast<uint32_t>(index));
    Build(*owned, index);
    p = owned.get();
    expanded[index] = std::move(owned);
    return p;
}

std::unique_ptr<TopologyPolygon> TopologyTree::ExpandRecord(const uint8_t* record32)
{
    TrackCollisionU2 pack;
    pack.RawPolygons.assign(record32, record32 + 32);
    pack.CellFirst.assign(1, 0);
    pack.CellCount.assign(1, 0);
    TopologyTree t(0, &pack);
    t.Expand(0);
    std::unique_ptr<TopologyPolygon> p = std::move(t.expanded[0]);
    p->Tree = nullptr;
    return p;
}

void TopologyTree::Build(TopologyPolygon& p, int i) const
{
    p.Terrain = static_cast<int8_t>(TerrainByte(i));
    p.Flags = Flags(i);
    int code = CornerCode(i);
    p.MinX = FixedToFloat(Shl13(Xs(i, code & 3)));
    p.MinY = FixedToFloat(Shl13(Ys(i, (code >> 2) & 3)));
    p.MaxX = FixedToFloat(Shl13(Xs(i, (code >> 4) & 3)));
    p.MaxY = FixedToFloat(Shl13(Ys(i, (code >> 6) & 3)));
    // sub_5D2CC0: x = (X << 13) / 65536, y likewise, z = (((base << 8) + Z) << 8) / 65536
    int b = Base(i);
    for (int k = 0; k < 4; k++) {
        p.V[3 * k] = FixedToFloat(Shl13(Xs(i, k)));
        p.V[3 * k + 1] = FixedToFloat(Shl13(Ys(i, k)));
        uint32_t z = (static_cast<uint32_t>(b) << 8) + static_cast<uint32_t>(static_cast<int>(Zs(i, k)));
        p.V[3 * k + 2] = FixedToFloat(static_cast<int>(z << 8));
    }
    p.Count = (p.Flags & 0x10) != 0 ? 4 : 3;
    if ((p.Flags & 0x04) != 0) {
        p.V[2] = TopologyK::BarrierLow;
        p.V[5] = TopologyK::BarrierHigh;
        p.V[8] = TopologyK::BarrierHigh;
        p.V[11] = TopologyK::BarrierLow;
    }
    const float e1[3] = { (F24(p.V[3]) - p.V[0]).Store(), (F24(p.V[4]) - p.V[1]).Store(), (F24(p.V[5]) - p.V[2]).Store() };
    const float e2[3] = { (F24(p.V[6]) - p.V[0]).Store(), (F24(p.V[7]) - p.V[1]).Store(), (F24(p.V[8]) - p.V[2]).Store() };
    float n[3];
    BWareMath::CrossX87(n, e1, e2);
    BWareMath2::NormalizeX87(n, n);
    p.Nx = n[0];
    p.Ny = n[1];
    p.Nz = n[2];
    p.D = (F24(p.V[2]) * p.Nz + F24(p.V[1]) * p.Ny + F24(p.V[0]) * p.Nx).Store();
}

// ---------------------------------------------------------------------------------------------- TopologyManagerU2

TopologyManagerU2::TopologyManagerU2(VisibleSectionsU2 sections, TopologyGroupsU2 groups)
    : Sections(std::move(sections)), Groups(std::move(groups)), DrivableLimit(Sections.DrivableLimit)
{
    // sub_5E9C10: letter = number / 100 + '@' in 'A'..'T' and 1 <= number % 100 < dword_800DFC -> drivable list
    for (SectionBoundaryU2& b : Sections.Boundaries) {
        int letter = static_cast<int8_t>(static_cast<uint8_t>(b.Section / 100 + 0x40));
        int rem = b.Section % 100;
        if (letter >= 0x41 && letter <= 0x54 && rem >= 1 && rem < DrivableLimit) Drivable.push_back(&b);
    }
    GroupEnabled.resize(Groups.Hash.size());
    for (size_t i = 0; i < GroupEnabled.size(); i++) GroupEnabled[i] = static_cast<int8_t>(i == 0 ? 1 : 0);   // TopologyManager::Init 0x5D8350
}

std::vector<TopologyTree*> TopologyManagerU2::Trees() const
{
    std::vector<TopologyTree*> list;
    list.reserve(slotOrder.size());
    for (int s : slotOrder) {
        auto it = slots.find(s);
        if (it != slots.end()) list.push_back(it->second);
    }
    return list;
}

int TopologyManagerU2::Slot(int16_t number) const
{
    if (number == 0) return 0;
    int n = number;
    int rem = n % 100;
    if (rem >= DrivableLimit && rem < DrivableLimit + DrivableLimit) n = static_cast<int16_t>(n - DrivableLimit);
    int s = static_cast<int16_t>(n);
    int letter = static_cast<int8_t>(static_cast<uint8_t>(s / 100 + 0x40));
    return (s % 100) + (letter - 0x41) * 20;
}

TopologyTree* TopologyManagerU2::GetIndex(int16_t number) const
{
    auto it = slots.find(Slot(number));
    return it != slots.end() ? it->second : nullptr;
}

TopologyCoordinate* TopologyManagerU2::CreateCoordinate()
{
    auto c = std::make_unique<TopologyCoordinate>();
    c->Registered = true;
    TopologyCoordinate* r = c.get();
    coordinates.push_back(std::move(c));
    return r;
}

void TopologyManagerU2::LoadTree(TopologyTree* tree)
{
    int slot = Slot(tree->Number);
    auto it = slots.find(slot);
    if (it != slots.end() && it->second != tree) UnloadTree(it->second);
    if (slots.find(slot) == slots.end()) slotOrder.push_back(slot);
    slots[slot] = tree;
    int boundary = tree->Number - DrivableLimit;
    for (auto& cp : coordinates) {
        TopologyCoordinate* c = cp.get();
        if (c->Tree != nullptr) continue;
        int b = c->Boundary != nullptr ? c->Boundary->Section : 0;
        if (b == boundary) Connect(c, tree);
    }
}

void TopologyManagerU2::UnloadTree(TopologyTree* tree)
{
    std::vector<TopologyCoordinate*> clients = tree->Coordinates;
    for (TopologyCoordinate* c : clients) Disconnect(c);
    tree->MarkDead();
    int slot = Slot(tree->Number);
    auto it = slots.find(slot);
    if (it != slots.end() && it->second == tree) {
        slots.erase(it);
        slotOrder.erase(std::remove(slotOrder.begin(), slotOrder.end(), slot), slotOrder.end());
    }
}

void TopologyManagerU2::Disconnect(TopologyCoordinate* c)
{
    if (c->Tree == nullptr) return;
    c->CacheCount = 0;
    for (auto& p : c->Cache) p = nullptr;
    ClearInterest(c);
    auto& list = c->Tree->Coordinates;
    auto it = std::find(list.begin(), list.end(), c);
    if (it != list.end()) list.erase(it);
    c->Tree = nullptr;
}

void TopologyManagerU2::Connect(TopologyCoordinate* c, TopologyTree* tree)
{
    Disconnect(c);
    if (tree == nullptr) return;
    c->Tree = tree;
    tree->Coordinates.push_back(c);
    RefreshInterest(c);
}

bool TopologyManagerU2::UpdateInterest(TopologyCoordinate* c, float minX, float minY, float maxX, float maxY, const float* vel)
{
    float cx = ((F24(minX) + maxX) * TopologyK::Half).Store();
    float cy = ((F24(minY) + maxY) * TopologyK::Half).Store();
    SetSectionPosition(c, cx, cy, 0.0f);
    c->BoxMinX = minX; c->BoxMinY = minY; c->BoxMaxX = maxX; c->BoxMaxY = maxY;
    // sub_43D470(interest min, interest max, box min, box max): the box lies inside the interest box
    if ((Fc(minX, c->InterestMinX) & 0x01) == 0 && !X87::ParityEven(Fc(maxX, c->InterestMaxX) & 0x41)
        && (Fc(minY, c->InterestMinY) & 0x01) == 0 && !X87::ParityEven(Fc(maxY, c->InterestMaxY) & 0x41))
        return false;
    float x0 = (F24(minX) - TopologyK::PointOne).Store();
    float y0 = (F24(minY) - TopologyK::PointOne).Store();
    float x1 = (F24(maxX) + TopologyK::PointOne).Store();
    float y1 = (F24(maxY) + TopologyK::PointOne).Store();
    c->InterestMinX = x0; c->InterestMinY = y0; c->InterestMaxX = x1; c->InterestMaxY = y1;
    if (vel != nullptr) {
        float vx = (F24(vel[0]) * TopologyK::VelocityScale).Store();
        float vy = (F24(vel[1]) * TopologyK::VelocityScale).Store();
        float dx;
        if ((Fc(vx, TopologyK::VelocityMin) & 0x41) != 0) dx = TopologyK::VelocityMin;
        else dx = X87::ParityEven(Fc(TopologyK::VelocityMax, vx) & 0x05) ? vx : TopologyK::VelocityMax;
        float dy;
        if ((Fc(vy, TopologyK::VelocityMin) & 0x41) != 0) dy = TopologyK::VelocityMin;
        else dy = X87::ParityEven(Fc(TopologyK::VelocityMax, vy) & 0x05) ? vy : TopologyK::VelocityMax;
        if ((Fc(dx, TopologyK::Zero) & 0x41) == 0) c->InterestMaxX = (F24(x1) + dx).Store();
        else c->InterestMinX = (F24(x0) + dx).Store();
        if ((Fc(dy, TopologyK::Zero) & 0x41) == 0) c->InterestMaxY = (F24(y1) + dy).Store();
        else c->InterestMinY = (F24(y0) + dy).Store();
    }
    RefreshInterest(c);
    return true;
}

void TopologyManagerU2::ClearInterest(TopologyCoordinate* c)
{
    if (c->InterestCount == 0) return;
    for (int i = 0; i < c->InterestCount; i++) c->Interest[i] = nullptr;
    c->InterestCount = c->InterestGround = c->InterestVertical = c->InterestOther = 0;
}

void TopologyManagerU2::RefreshInterest(TopologyCoordinate* c)
{
    ClearInterest(c);
    if (c->Tree == nullptr) return;
    if ((Fc(c->InterestMaxX, c->InterestMinX) & 0x01) != 0) return;   // fcomp; test ah,1; jnz: max.x < min.x
    for (int g : c->Groups) GroupEnabled[g] = 1;
    FillInterest(c->Tree, c, c->InterestMinX, c->InterestMinY, c->InterestMaxX, c->InterestMaxY);
    for (int g : c->Groups) GroupEnabled[g] = 0;
}

void TopologyManagerU2::FillInterest(TopologyTree* t, TopologyCoordinate* c, float minX, float minY, float maxX, float maxY)
{
    if (t->PolygonCount == 0) return;
    int x0 = Ftol((F24(minX) - t->OriginX) * TopologyK::InvCell);
    int x1 = Ftol((F24(maxX) - t->OriginX) * TopologyK::InvCell);
    int y1 = Ftol((F24(maxY) - t->OriginY) * TopologyK::InvCell);
    int y0 = Ftol((F24(minY) - t->OriginY) * TopologyK::InvCell);
    std::vector<int> list;
    list.reserve(MaxListed);
    int nVertical = 0, nOther = 0;
    for (int cy = y0; cy <= y1; cy++) {
        F24 y32 = F24(static_cast<float>(cy)) * TopologyK::Cell;            // fild; fmul 32 (kept on the stack)
        for (int cx = x0; cx <= x1; cx++) {
            F24 cellX = F24(static_cast<float>(cx)) * TopologyK::Cell + t->OriginX;
            float cellY = (y32 + t->OriginY).Store();
            int hx = Ftol((F24(maxX) - minX + TopologyK::Eight) * TopologyK::MinusEighth);
            uint32_t mx = (1u << ((1 - hx) & 31)) - 1u;
            int lx = Ftol((F24(minX) - cellX) * TopologyK::InvSubCell);
            mx = (mx << ((lx < 0 ? 0 : lx) & 31)) & 0xFu;
            int hy = Ftol((F24(maxY) - minY + TopologyK::Eight) * TopologyK::MinusEighth);
            uint32_t my = (1u << ((1 - hy) & 31)) - 1u;
            int ly = Ftol((F24(minY) - cellY) * TopologyK::InvSubCell);
            my = (my << ((ly < 0 ? 0 : ly) & 31)) & 0xFu;
            uint32_t mask = 0;
            if ((my & 1) != 0) mask = mx;
            if ((my & 2) != 0) mask |= mx << 4;
            if ((my & 4) != 0) mask |= mx << 8;
            if ((my & 8) != 0) mask |= mx << 12;
            int ccx = std::min(cx <= 0 ? 0 : cx, t->Width - 1);
            int ccy = std::min(cy <= 0 ? 0 : cy, t->Height - 1);
            int cell = t->Width * ccy + ccx;
            if (cell < 0 || cell >= static_cast<int>(t->Pack->CellFirst.size())) continue;   // (the C# throws)
            int first = t->CellFirst(cell), end = first + t->CellCount(cell);
            for (int r = first; r < end; r++) {
                if ((static_cast<uint32_t>(t->RefMask(r)) & mask) == 0) continue;
                int i = t->RefPolygon(r);
                if (t->Listed[i]) continue;                                   // record bit 0x20
                int code = t->CornerCode(i);
                if (X87::ParityEven(Fc(TopologyTree::FixedToFloat(TopologyTree::Shl13(t->Xs(i, code & 3))), maxX) & 0x41)) continue;
                if (X87::ParityEven(Fc(TopologyTree::FixedToFloat(TopologyTree::Shl13(t->Ys(i, (code >> 2) & 3))), maxY) & 0x41)) continue;
                if ((Fc(TopologyTree::FixedToFloat(TopologyTree::Shl13(t->Xs(i, (code >> 4) & 3))), minX) & 0x01) != 0) continue;
                if ((Fc(TopologyTree::FixedToFloat(TopologyTree::Shl13(t->Ys(i, (code >> 6) & 3))), minY) & 0x01) != 0) continue;
                int g = t->Attr0(i);
                if (g != 0 && (g >= static_cast<int>(GroupEnabled.size()) || GroupEnabled[g] == 0)) continue;
                if (static_cast<int>(list.size()) >= MaxListed) continue;
                list.push_back(i);
                uint8_t f = t->Flags(i);
                if ((f & 2) != 0) nVertical++;
                else if ((f & 8) != 0) nOther++;
                t->Listed[i] = 1;
            }
        }
    }
    int total = static_cast<int>(list.size());
    int ground = total - nOther - nVertical;
    int gi = 0, vi = ground, oi = total - nOther;
    for (int i : list) {
        t->Listed[i] = 0;
        TopologyPolygon* p = t->Expand(i);
        uint8_t f = t->Flags(i);
        if ((f & 2) != 0) c->Interest[vi++] = p;
        else if ((f & 8) != 0) c->Interest[oi++] = p;
        else c->Interest[gi++] = p;
    }
    c->InterestCount = total;
    c->InterestGround = ground;
    c->InterestVertical = nVertical;
    c->InterestOther = nOther;
}

void TopologyManagerU2::SetSectionPosition(TopologyCoordinate* c, float px, float py, float slack)
{
    F24 man = F24::Abs(F24(py) - c->PosY) + F24::Abs(F24(px) - c->PosX);
    F24 lim = F24(slack) + c->Radius;
    if ((Fc(lim, man) & 0x01) == 0) return;          // fcompp; test ah,1; jz
    if (!FullUpdate(c, px, py, slack)) return;
    int b = c->Boundary != nullptr ? c->Boundary->Section : 0;
    if (c->Tree == nullptr) {
        if (b == 0) return;
        TopologyTree* t = GetIndex(static_cast<int16_t>(b));
        if (t == nullptr) return;
        Connect(c, t);
        return;
    }
    if (c->Tree->Number - DrivableLimit == b) return;
    Connect(c, GetIndex(static_cast<int16_t>(b)));
}

bool TopologyManagerU2::FullUpdate(TopologyCoordinate* c, float px, float py, float slack)
{
    F24 dx = F24(px) - c->PosX, dy = F24(py) - c->PosY;
    F24 d = F24::Sqrt(dy * dy + dx * dx);
    F24 lim = F24(slack) + c->Radius;
    if ((Fc(lim, d) & 0x01) == 0) return false;
    c->PosX = px;
    c->PosY = py;
    if (c->Boundary != nullptr && Contains(*c->Boundary, px, py)) {       // sub_5D7090
        c->Radius = InsideDistance(*c->Boundary, px, py).Store();
        return false;
    }
    SectionBoundaryU2* found = FindBoundary(px, py);                       // sub_5DB850
    c->Boundary = found;
    if (found != nullptr) {
        c->Radius = InsideDistance(*found, px, py).Store();
        return true;
    }
    Nearest(px, py, c->Radius);                                           // sub_5DB770 (the boundary stays null)
    return true;
}

SectionBoundaryU2* TopologyManagerU2::FindBoundary(float px, float py)
{
    for (int i = 0; i < static_cast<int>(Drivable.size()); i++) {
        SectionBoundaryU2* b = Drivable[i];
        if (InBox(*b, px, py, 0.0f) && PointInPolygon(*b, px, py)) {
            MoveToFront(i);
            return b;
        }
    }
    float dist;
    SectionBoundaryU2* n = Nearest(px, py, dist);
    if (BWareMath2::GeOrNaN(dist, TopologyK::PointOne)) return nullptr;   // fcomp 0.1; test ah,5; jp
    return n;
}

SectionBoundaryU2* TopologyManagerU2::Nearest(float px, float py, float& distance)
{
    SectionBoundaryU2* best = nullptr;
    float bestD = TopologyK::FarDistance;
    for (int i = 0; i < static_cast<int>(Drivable.size()); i++) {
        SectionBoundaryU2* b = Drivable[i];
        if (InBox(*b, px, py, 0.0f) && PointInPolygon(*b, px, py)) {
            MoveToFront(i);
            distance = 0.0f;
            return b;
        }
        F24 d = OutsideDistance(*b, px, py, bestD);                       // sub_5D70D0
        bool take;
        if (best == nullptr) take = true;
        else if ((Fc(d, bestD) & 0x05) == 0x01) take = true;              // test ah,5; jnp: d < best
        else if (BWareMath2::NeOrNaN(d, bestD)) take = false;             // test ah,44h; jp
        else take = b->Section < best->Section;                           // cmp dx, [edi+8]; jge
        if (take) {
            bestD = d.Store();
            best = b;
        }
    }
    distance = bestD;
    return best;
}

void TopologyManagerU2::MoveToFront(int i)
{
    if (i == 0) return;
    SectionBoundaryU2* b = Drivable[i];
    Drivable.erase(Drivable.begin() + i);
    Drivable.insert(Drivable.begin(), b);
}

bool TopologyManagerU2::InBox(const SectionBoundaryU2& b, float px, float py, float eps)
{
    if ((Fc(F24(b.MinX) - eps, px) & 0x41) == 0) return false;        // min.x - eps > x
    if ((Fc(F24(eps) + b.MaxX, px) & 0x05) == 0x01) return false;     // eps + max.x < x
    if ((Fc(F24(b.MinY) - eps, py) & 0x41) == 0) return false;
    if ((Fc(F24(eps) + b.MaxY, py) & 0x05) == 0x01) return false;
    return true;
}

bool TopologyManagerU2::Contains(const SectionBoundaryU2& b, float px, float py)
{
    return InBox(b, px, py, 0.0f) && PointInPolygon(b, px, py);
}

bool TopologyManagerU2::PointInPolygon(const SectionBoundaryU2& b, float px, float py)
{
    bool inside = false;
    int n = b.Count();
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float yi = b.Y[i], yj = b.Y[j];
        if (!Crosses(yi, yj, py)) continue;
        F24 x = (F24(py) - yi) * (F24(b.X[j]) - b.X[i]) / (F24(yj) - yi) + b.X[i];
        if ((Fc(x, px) & 0x41) != 0) continue;          // test ah,41h; jnz: x <= px
        inside = !inside;
    }
    return inside;
}

bool TopologyManagerU2::Crosses(float yi, float yj, float py)
{
    // fld yi; fcomp py; test ah,41h; jp alt  (alt when yi > py or unordered)
    if (!X87::ParityEven(Fc(yi, py) & 0x41)) {
        // yi <= py: fld py; fcomp yj; test ah,5; jnp compute  (py < yj)
        if ((Fc(py, yj) & 0x05) == 0x01) return true;
    }
    // alt: fld yj; fcomp py; test ah,41h; jp skip (yj > py or unordered)
    if (X87::ParityEven(Fc(yj, py) & 0x41)) return false;
    // fld py; fcomp yi; test ah,5; jp skip (py >= yi or unordered)
    if (X87::ParityEven(Fc(py, yi) & 0x05)) return false;
    return true;
}

F24 TopologyManagerU2::SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = (F24(px) - ax).Store(), dy = (F24(py) - ay).Store();
    F24 eyS = F24(by) - ay;
    float ex = (F24(bx) - ax).Store();
    float ey = eyS.Store();
    float len = F24::Sqrt(eyS * eyS + F24(ex) * ex).Store();
    // bNormalize(bVector2 *, const bVector2 *) 0x43CE50 in place
    float nx, ny;
    F24 l = F24::Sqrt(F24(ey) * ey + F24(ex) * ex);
    float ls = l.Store();
    if (!BWareMath2::NeOrNaN(ls, TopologyK::Zero)) { nx = 1.0f; ny = 0.0f; }
    else {
        F24 inv = F24(TopologyK::One) / ls;
        nx = (inv * ex).Store();
        ny = (inv * ey).Store();
    }
    float along = (F24(dy) * ny + F24(dx) * nx).Store();
    F24 perp = -F24(nx) * dy + F24(dx) * ny;
    if ((Fc(along, TopologyK::Zero) & 0x05) == 0x01)        // test ah,5; jp not taken: along < 0
        return F24::Sqrt(perp * perp + F24(along) * along);
    if ((Fc(along, len) & 0x41) != 0) return F24::Abs(perp); // along <= len
    F24 t = F24(along) - len;
    return F24::Sqrt(t * t + perp * perp);
}

F24 TopologyManagerU2::InsideDistance(const SectionBoundaryU2& b, float px, float py)
{
    if (!(InBox(b, px, py, 0.0f) && PointInPolygon(b, px, py))) return TopologyK::Zero;
    int n = b.Count();
    F24 dx0 = F24(px) - b.X[0], dy0 = F24(py) - b.Y[0];
    float best = F24::Sqrt(dy0 * dy0 + dx0 * dx0).Store();
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        F24 d = SegmentDistance(px, py, b.X[i], b.Y[i], b.X[j], b.Y[j]);
        if (!BWareMath2::GeOrNaN(d, best)) best = d.Store();
    }
    return best;
}

F24 TopologyManagerU2::OutsideDistance(const SectionBoundaryU2& b, float px, float py, float max)
{
    if (!InBox(b, px, py, max)) return max;
    if (InBox(b, px, py, 0.0f) && PointInPolygon(b, px, py)) return TopologyK::Zero;
    float best = max;
    int n = b.Count();
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        F24 d = SegmentDistance(px, py, b.X[i], b.Y[i], b.X[j], b.Y[j]);
        if (!BWareMath2::GeOrNaN(d, best)) best = d.Store();
    }
    return best;
}

// ------------------------------------------------------------------ NewGetElevation

F24 TopologyManagerU2::NewGetElevation(TopologyCoordinate* c, float px, float py, float pz, int& terrain, float* normal, bool& valid)
{
    SetSectionPosition(c, px, py, TopologyK::SectionSlack);
    TopologyTree* t = c->Tree;
    if (t == nullptr) {
        valid = false;
        terrain = 1;
        if (normal != nullptr) { normal[0] = 0.0f; normal[1] = 0.0f; normal[2] = -1.0f; }
        return pz;
    }
    float limit = (F24(pz) + TopologyK::One).Store();
    // 1. cache
    for (int i = 0; i < c->CacheCount; i++) {
        TopologyPolygon* p = c->Cache[i];
        F24 r = p->EdgeTest(px, py);
        if (BWareMath2::GeOrNaN(r, TopologyK::Zero)) continue;              // fcomp 0; test ah,5; jp
        F24 h = p->PlaneHeight(px, py);
        bool hit = (p->Flags & 1) == 0;
        if (!hit) {
            if (X87::ParityEven(Fc(h, limit) & 0x41)) continue;  // h > limit or unordered
            hit = (Fc(F24(limit) - h, TopologyK::Four) & 0x05) == 0x01;   // limit - h < 4
        }
        if (!hit) continue;
        for (int k = i; k > 0; k--) c->Cache[k] = c->Cache[k - 1];
        c->Cache[0] = p;
        valid = true;
        terrain = p->Terrain;
        if (normal != nullptr) { normal[0] = p->Nx; normal[1] = p->Ny; normal[2] = p->Nz; }
        return h;
    }
    for (int g : c->Groups) GroupEnabled[g] = 1;
    // 2. grid cell and sub-cell
    int cx = Ftol((F24(px) - t->OriginX) * TopologyK::InvCell);
    int cy = Ftol((F24(py) - t->OriginY) * TopologyK::InvCell);
    F24 cellX = F24(static_cast<float>(cx)) * TopologyK::Cell + t->OriginX;
    float cellY = (F24(static_cast<float>(cy)) * TopologyK::Cell + t->OriginY).Store();
    int sx = Ftol((F24(px) - cellX) * TopologyK::InvSubCell);
    int sy = Ftol((F24(py) - cellY) * TopologyK::InvSubCell);
    uint32_t mask = 1u << (sx & 31);
    mask <<= (static_cast<uint32_t>(sy) << 2) & 31u;
    int ccx = std::min(cx <= 0 ? 0 : cx, t->Width - 1);
    int ccy = std::min(cy <= 0 ? 0 : cy, t->Height - 1);
    int cell = t->Width * ccy + ccx;
    if (cell < 0 || cell >= static_cast<int>(t->Pack->CellFirst.size())) cell = -1;   // no grid (the C# throws)
    TopologyPolygon* best = nullptr;
    float bestH = TopologyK::NoBestElevation;
    for (int pass = 0; pass < 2 && best == nullptr && cell >= 0; pass++) {
        if (pass == 1) limit = TopologyK::FltMax;
        int qx = Ftol(F24(px) * TopologyK::Fixed);
        int qy = Ftol(F24(py) * TopologyK::Fixed);
        int first = t->CellFirst(cell), end = first + t->CellCount(cell);
        for (int r = first; r < end; r++) {
            if ((static_cast<uint32_t>(t->RefMask(r)) & mask) == 0) continue;
            int i = t->RefPolygon(r);
            if ((t->Flags(i) & 0x0A) != 0) continue;                          // vertical faces and 0x08
            int code = t->CornerCode(i);
            if (qx < TopologyTree::Shl13(t->Xs(i, code & 3))) continue;
            if (qy < TopologyTree::Shl13(t->Ys(i, (code >> 2) & 3))) continue;
            if (qx > TopologyTree::Shl13(t->Xs(i, (code >> 4) & 3))) continue;
            if (qy > TopologyTree::Shl13(t->Ys(i, (code >> 6) & 3))) continue;
            int g = t->Attr0(i);
            if (g != 0 && (g >= static_cast<int>(GroupEnabled.size()) || GroupEnabled[g] == 0)) continue;
            TopologyPolygon* p = t->Expand(i);
            F24 e = p->EdgeTest(px, py);
            if (X87::ParityEven(Fc(e, TopologyK::Zero) & 0x41)) continue;   // outside (or unordered)
            float h = p->PlaneHeight(px, py).Store();
            if ((p->Flags & 1) == 0 && !BWareMath2::GeOrNaN(e, TopologyK::Zero)) {
                // strictly inside a single-level polygon: taken at once
                bestH = h;
                best = p;
                break;
            }
            if ((Fc(h, bestH) & 0x41) != 0) continue;                  // h <= best
            if (X87::ParityEven(Fc(h, limit) & 0x41)) continue;         // h > limit or unordered
            bestH = h;
            best = p;
            if ((p->Flags & 0x40) != 0) continue;
            if ((Fc(F24(limit) - h, TopologyK::Four) & 0x05) == 0x01) break;   // within 4 below the limit
        }
    }
    for (int g : c->Groups) GroupEnabled[g] = 0;
    if (best == nullptr) {
        valid = false;
        terrain = 1;
        if (normal != nullptr) { normal[0] = 0.0f; normal[1] = 0.0f; normal[2] = -1.0f; }
        return pz;
    }
    if ((best->Flags & 0x40) == 0) {
        c->CacheCount = std::min(c->CacheCount + 1, static_cast<int>(TopologyCoordinate::CacheSize));
        for (int k = c->CacheCount - 1; k > 0; k--) c->Cache[k] = c->Cache[k - 1];
        c->Cache[0] = best;
    }
    valid = true;
    terrain = best->Terrain;
    if (normal != nullptr) { normal[0] = best->Nx; normal[1] = best->Ny; normal[2] = best->Nz; }
    return bestH;
}

bool TopologyManagerU2::HasTopology(TopologyCoordinate* c, float px, float py)
{
    int terrain;
    bool valid;
    NewGetElevation(c, px, py, TopologyK::HasTopologyZ, terrain, nullptr, valid);
    return valid;
}

} // namespace bb
