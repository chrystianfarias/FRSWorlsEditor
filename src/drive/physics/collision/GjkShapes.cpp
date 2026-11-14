// Port of NFSU2Unity Core/Physics/Collision/GjkShapes.cs.
#include "drive/physics/collision/GjkShapes.h"

#include "drive/numerics/BWareMath.h"

#include <cstring>
#include <deque>

namespace bb {

namespace {

const float Sentinel = X87::Bits(0xFCDC84DCu);   // flt_7A206C -9.16e36 (also the immediate)

int16_t RdS16(const uint8_t* b, int o) { int16_t v; std::memcpy(&v, b + o, 2); return v; }
uint16_t RdU16(const uint8_t* b, int o) { uint16_t v; std::memcpy(&v, b + o, 2); return v; }
int32_t RdS32(const uint8_t* b, int o) { int32_t v; std::memcpy(&v, b + o, 4); return v; }
uint32_t RdU32(const uint8_t* b, int o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; }
float RdF32(const uint8_t* b, int o) { float v; std::memcpy(&v, b + o, 4); return v; }

int Align16(int a) { return (a + 15) & ~15; }

} // namespace

// ---------------------------------------------------------------------------------------------- GjkPolytope

void GjkPolytope::GetPoint(int id, GjkVertex& v)
{
    const GjkVertex& s = Vertices[id];
    v.NeighbourCount = s.NeighbourCount;
    v.NeighbourFirst = s.NeighbourFirst;
    v.Id = s.Id;
    v.Mark = s.Mark;
    v.X = s.X; v.Y = s.Y; v.Z = s.Z;
    if (CogSet) {
        v.X = (F24(v.X) - Cog[0]).Store();
        v.Y = (F24(v.Y) - Cog[1]).Store();
        v.Z = (F24(v.Z) - Cog[2]).Store();
    }
}

F24 GjkPolytope::DotOf(int i, const float* ld) const
{
    const GjkVertex& e = Vertices[i];
    F24 x, y, z;
    if (CogSet) { x = F24(e.X) - Cog[0]; y = F24(e.Y) - Cog[1]; z = F24(e.Z) - Cog[2]; }
    else { x = e.X; y = e.Y; z = e.Z; }
    return F24(ld[2]) * z + F24(ld[1]) * y + F24(ld[0]) * x;
}

void GjkPolytope::Support(const float* dir, GjkVertex& v, const float* m, const float* mt)
{
    float ld[3];
    BWareMath2::MulVector3X87(ld, mt, dir);
    int n = static_cast<int>(Vertices.size());
    std::vector<bool> visited(n);
    std::vector<float> dots(n);
    bool hill = Count > 16;
    std::deque<int> list;
    list.push_back(v.Id);
    int best = -1;
    while (!list.empty()) {
        int e = list.front();
        list.pop_front();
        bool local = true;
        if (!visited[e]) { dots[e] = DotOf(e, ld).Store(); visited[e] = true; }
        float cur = dots[e];
        const GjkVertex& ev = Vertices[e];
        if (ev.NeighbourCount == 0) continue;
        int end = ev.NeighbourFirst + ev.NeighbourCount;
        for (int k = ev.NeighbourFirst; k < end; k++) {
            int nb = Neighbours[k];
            bool wasVisited = visited[nb];
            if (!wasVisited) dots[nb] = DotOf(nb, ld).Store();
            if ((BWareMath2::Fcom(dots[nb], cur) & 0x41) == 0) {        // neighbour strictly better
                local = false;
                if (hill && !wasVisited) list.push_back(nb);
            }
            visited[nb] = true;
        }
        hill = false;                                                   // var_64 = dword_86B2D8 (0)
        if (local) { best = e; break; }
    }
    if (best < 0) {
        float bd = Sentinel;
        for (int i = 0; i < n; i++) {
            if (!visited[i]) { dots[i] = DotOf(i, ld).Store(); visited[i] = true; }
            if ((BWareMath2::Fcom(dots[i], bd) & 0x41) == 0 || !BWareMath2::NeOrNaN(bd, Sentinel)) {
                bd = dots[i];
                best = i;
            }
        }
        if (best < 0) return;
    }
    const GjkVertex& b = Vertices[best];
    float p[3];
    const float bv[3] = { b.X, b.Y, b.Z };
    BWareMath2::MulVector3X87(p, m, bv);
    v.X = p[0]; v.Y = p[1]; v.Z = p[2];
    v.Id = b.Id;
}

void GjkPolytope::GetBoundingBox(float* min, float* max)
{
    GjkVertex v;
    GetPoint(0, v);
    min[0] = max[0] = v.X; min[1] = max[1] = v.Y; min[2] = max[2] = v.Z;
    for (int i = 1; i < Count; i++) {
        GetPoint(i, v);
        Expand(min, max, 0, v.X); Expand(min, max, 1, v.Y); Expand(min, max, 2, v.Z);
    }
}

void GjkPolytope::Expand(float* min, float* max, int k, float x)
{
    if (x < min[k]) min[k] = x;
    if (x > max[k]) max[k] = x;
}

std::vector<std::unique_ptr<GjkPolytope>> GjkPolytope::ReadAll(const std::vector<uint8_t>& file)
{
    std::vector<std::unique_ptr<GjkPolytope>> list;
    if (!file.empty()) ScanContainers(file.data(), 0, static_cast<int>(file.size()), list);
    return list;
}

void GjkPolytope::ScanContainers(const uint8_t* b, int start, int end, std::vector<std::unique_ptr<GjkPolytope>>& list)
{
    int p = start;
    while (p + 8 <= end) {
        uint32_t id = RdU32(b, p);
        int size = RdS32(b, p + 4);
        if (size < 0 || p + 8 + size > end) break;
        if (id == 0x80034020u) ReadPack(b, p + 8, p + 8 + size, list);
        else if ((id & 0x80000000u) != 0) ScanContainers(b, p + 8, p + 8 + size, list);
        p += 8 + size;
    }
}

void GjkPolytope::ReadPack(const uint8_t* b, int p, int end, std::vector<std::unique_ptr<GjkPolytope>>& list)
{
    while (p + 8 <= end) {
        uint32_t id = RdU32(b, p);
        int size = RdS32(b, p + 4);
        if (id != 0x39200u) {
            p += 8 + size;
            continue;
        }
        int hdr = Align16(p + 8);
        int vc = p + 8 + size;                                  // 0x39201
        int vsize = RdS32(b, vc + 4);
        int v0 = Align16(vc + 8);
        int nverts = (vc + 8 + vsize - v0) / 48;
        int nc = vc + 8 + vsize;                                // 0x39202
        int nsize = RdS32(b, nc + 4);
        int n0 = Align16(nc + 8);
        int nnb = (nc + 8 + nsize - n0) / 2;
        auto poly = std::make_unique<GjkPolytope>();
        poly->NameHash = RdU32(b, hdr + 0x18);
        poly->Count = nverts;
        poly->Vertices.resize(nverts);
        poly->Neighbours.resize(nnb);
        int len = 0;
        while (len < 32 && b[hdr + 0x1C + len] != 0) len++;
        poly->Name.assign(reinterpret_cast<const char*>(b + hdr + 0x1C), static_cast<size_t>(len));
        for (int i = 0; i < nverts; i++) {
            int o = v0 + 48 * i;
            GjkVertex& v = poly->Vertices[i];
            v.NeighbourCount = RdS16(b, o + 8);
            v.NeighbourFirst = RdS16(b, o + 0xA);
            v.Id = RdS16(b, o + 0xC);
            v.Mark = RdU16(b, o + 0xE);
            v.X = RdF32(b, o + 0x10);
            v.Y = RdF32(b, o + 0x14);
            v.Z = RdF32(b, o + 0x18);
            v.W = RdF32(b, o + 0x1C);
        }
        for (int i = 0; i < nnb; i++) poly->Neighbours[i] = RdS16(b, n0 + 2 * i);
        list.push_back(std::move(poly));
        p = nc + 8 + nsize;
    }
}

// ---------------------------------------------------------------------------------------------- GjkBox

GjkBox::GjkBox(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
{
    Count = 8;
    Center[0] = ((F24(minX) + maxX) * BWareMath2::K_Half).Store();
    Center[1] = ((F24(minY) + maxY) * BWareMath2::K_Half).Store();
    Center[2] = ((F24(minZ) + maxZ) * BWareMath2::K_Half).Store();
    Half[0] = ((F24(maxX) - minX) * BWareMath2::K_Half).Store();
    Half[1] = ((F24(maxY) - minY) * BWareMath2::K_Half).Store();
    Half[2] = ((F24(maxZ) - minZ) * BWareMath2::K_Half).Store();
}

GjkBox GjkBox::DefaultCar()
{
    return GjkBox(X87::Bits(0xC00DE354u), X87::Bits(0xBF789375u), X87::Bits(0xBE4CCCCDu),
                  X87::Bits(0x400A8F5Cu), X87::Bits(0x3F789375u), X87::Bits(0x3F1FBE77u));
}

void GjkBox::GetPoint(int id, GjkVertex& v)
{
    float x = Half[0], y = Half[1], z = Half[2];
    v.Id = static_cast<int16_t>(id);
    if ((id & 1) != 0) x = -Half[0];
    if ((id & 2) != 0) y = -Half[1];
    if ((id & 4) != 0) z = -Half[2];
    v.X = (F24(Center[0]) + x).Store();
    v.Y = (F24(Center[1]) + y).Store();
    v.Z = (F24(Center[2]) + z).Store();
}

void GjkBox::Support(const float* dir, GjkVertex& v, const float* m, const float* mt)
{
    float ld[3];
    BWareMath2::MulVector3X87(ld, mt, dir);
    F24 hx = Half[0], hy = Half[1], hz = Half[2];
    int id = 0;
    if (!BWareMath2::GeOrNaN(ld[0], CollisionK::Zero)) { id = 1; hx = -hx; }
    if (!BWareMath2::GeOrNaN(ld[1], CollisionK::Zero)) { id |= 2; hy = -hy; }
    if (!BWareMath2::GeOrNaN(ld[2], CollisionK::Zero)) { id |= 4; hz = -hz; }
    const float p[3] = { (hx + Center[0]).Store(), (F24(Center[1]) + hy).Store(), (F24(Center[2]) + hz).Store() };
    float o[3];
    BWareMath2::MulVector3X87(o, m, p);
    v.X = o[0]; v.Y = o[1]; v.Z = o[2]; v.W = 0.0f;
    v.Id = static_cast<int16_t>(id);
}

// ---------------------------------------------------------------------------------------------- GjkPolygon

void GjkPolygon::Build(const TrackPolygonShape& tp, const float* offset, float thickness)
{
    for (int k = 0; k < 3; k++) { FaceCentroid[k] = 0.0f; Centroid[k] = 0.0f; }
    Thickness = thickness;
    bool tri = CollisionK::Equal3(tp.V, 6, tp.V, 9, CollisionK::Eps1e3);
    Count = tri ? 3 : 4;
    for (int k = 0; k < Count; k++) {
        float x = (F24(tp.V[3 * k]) - offset[0]).Store();
        float y = (F24(tp.V[3 * k + 1]) - offset[1]).Store();
        float z = (F24(tp.V[3 * k + 2]) - offset[2]).Store();
        Vertices[k] = GjkVertex{};
        Vertices[k].Id = static_cast<int16_t>(k);
        Vertices[k].X = x; Vertices[k].Y = y; Vertices[k].Z = z; Vertices[k].W = 0.0f;
        FaceCentroid[0] = (F24(x) + FaceCentroid[0]).Store();
        FaceCentroid[1] = (F24(y) + FaceCentroid[1]).Store();
        FaceCentroid[2] = (F24(z) + FaceCentroid[2]).Store();
    }
    Centroid[0] = FaceCentroid[0]; Centroid[1] = FaceCentroid[1]; Centroid[2] = FaceCentroid[2];
    F24 inv = F24(CollisionK::One) / static_cast<float>(Count);
    FaceCentroid[0] = (inv * FaceCentroid[0]).Store();
    FaceCentroid[1] = (inv * FaceCentroid[1]).Store();
    FaceCentroid[2] = (inv * FaceCentroid[2]).Store();
    if ((BWareMath2::Fcom(Thickness, CollisionK::Zero) & 0x41) == 0) {      // thickness > 0
        F24 t = -F24(Thickness);
        float ex = (t * tp.Nx).Store(), ey = (F24(tp.Ny) * t).Store(), ez = (F24(tp.Nz) * t).Store();
        for (int k = 0; k < Count; k++) {
            GjkVertex s = Vertices[k];
            float x = (F24(s.X) + ex).Store(), y = (F24(s.Y) + ey).Store(), z = (F24(ez) + s.Z).Store();
            GjkVertex& d = Vertices[Count + k];
            d = GjkVertex{};
            d.Id = static_cast<int16_t>(Count + k);
            d.X = x; d.Y = y; d.Z = z; d.W = 0.0f;
            Centroid[0] = (F24(x) + Centroid[0]).Store();
            Centroid[1] = (F24(y) + Centroid[1]).Store();
            Centroid[2] = (F24(z) + Centroid[2]).Store();
        }
        Count *= 2;
    }
    F24 inv2 = F24(CollisionK::One) / static_cast<float>(Count);
    Centroid[0] = (inv2 * Centroid[0]).Store();
    Centroid[1] = (inv2 * Centroid[1]).Store();
    Centroid[2] = (inv2 * Centroid[2]).Store();
}

void GjkPolygon::GetPoint(int id, GjkVertex& v)
{
    const GjkVertex& s = Vertices[id];
    v.NeighbourCount = s.NeighbourCount;
    v.NeighbourFirst = s.NeighbourFirst;
    v.Id = s.Id;
    v.Mark = s.Mark;
    v.X = s.X; v.Y = s.Y; v.Z = s.Z;
}

void GjkPolygon::Support(const float* dir, GjkVertex& v, const float* /*m*/, const float* /*mt*/)
{
    int n = Count;
    if ((BWareMath2::Fcom(Thickness, CollisionK::Zero) & 0x41) == 0 && UseExtruded == 0) n /= 2;
    float best = Sentinel;
    int sel = -1;
    int groups = n >= 4 ? ((n - 4) / 4 + 1) : 0;
    int done = 0;
    for (int g = 0; g < groups; g++) {
        for (int s = 0; s < 4; s++) {
            int i = 4 * g + s;
            const GjkVertex& e = Vertices[i];
            F24 d = s == 0 ? F24(e.X) * dir[0] + F24(e.Z) * dir[2] + F24(dir[1]) * e.Y
                           : F24(e.Y) * dir[1] + F24(e.Z) * dir[2] + F24(e.X) * dir[0];
            if ((BWareMath2::Fcom(d, best) & 0x41) == 0 || !BWareMath2::NeOrNaN(best, Sentinel)) { best = d.Store(); sel = i; }
        }
        done += 4;
    }
    for (int i = done; i < n; i++) {
        const GjkVertex& e = Vertices[i];
        F24 d = F24(e.X) * dir[0] + F24(e.Z) * dir[2] + F24(e.Y) * dir[1];
        if ((BWareMath2::Fcom(d, best) & 0x41) == 0 || !BWareMath2::NeOrNaN(best, Sentinel)) { best = d.Store(); sel = i; }
    }
    if (sel < 0) return;
    const GjkVertex& b = Vertices[sel];
    v.X = b.X; v.Y = b.Y; v.Z = b.Z; v.W = 0.0f;
    v.Id = b.Id;
}

} // namespace bb
