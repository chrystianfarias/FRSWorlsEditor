#include "drive/cars/CarFxSetupU2.h"

#include "drive/cars/CarHeadlightsU2.h"
#include "drive/cars/CarRenderLayoutU2.h"
#include "drive/cars/RideInfoU2.h"
#include "drive/geometry/SolidList.h"
#include "drive/io/BinHash.h"
#include "drive/numerics/BWareMath.h"

#include <algorithm>
#include <cfloat>

namespace bb {

std::vector<LightFlareU2> CarFxSetupU2::CreateFlares(const CarRenderLayoutU2& layout) {
    std::vector<LightFlareU2> list;
    const Solid* body = layout.Model(5, 0, 0);
    float px = 0, py = 0, pz = 0;
    if (body != nullptr) { px = body->Pivot[12]; py = body->Pivot[13]; pz = body->Pivot[14]; }
    for (int slot = 0; slot < CarRenderLayoutU2::NumModelSlots; slot++) {
        const Solid* s = layout.Model(slot, 0, 0);
        if (s == nullptr) continue;
        for (const PositionMarker& mk : s->Markers) {
            int8_t type;
            switch (mk.NameHash) {
                case CarLightFlaresU2::CentreBrakelight: case CarLightFlaresU2::LeftBrakelight:
                case CarLightFlaresU2::RightBrakelight: type = 1; break;
                case CarLightFlaresU2::CentreHeadlight: case CarLightFlaresU2::LeftHeadlight:
                case CarLightFlaresU2::RightHeadlight: type = 0; break;
                default: continue;
            }
            const float* m = mk.Matrix;
            LightFlareU2 f;
            f.NameHash = mk.NameHash;
            f.Type = type;
            f.Flags = 4;
            f.X = BWareMath::S(m[12], px); f.Y = BWareMath::S(m[13], py); f.Z = BWareMath::S(m[14], pz);
            f.ReflectPosZ = 0.0f;
            f.DirX = m[8]; f.DirY = m[9]; f.DirZ = m[10];
            list.push_back(f);
        }
    }
    return list;
}

void CarFxSetupU2::PartsBoundingBox(const CarRenderLayoutU2& layout, float* min, float* max) {
    min[0] = min[1] = min[2] = FLT_MAX;
    max[0] = max[1] = max[2] = -FLT_MAX;
    bool any = false;
    for (int slot = 0; slot < CarRenderLayoutU2::NumModelSlots; slot++) {
        const bool listed = (slot >= 0 && slot <= 7) || (slot >= 9 && slot <= 12) || (slot >= 47 && slot <= 51);
        if (!listed) continue;
        for (int row = 0; row < 2; row++) {
            const Solid* s = layout.Model(slot, row, 0);
            if (s == nullptr) continue;
            any = true;
            min[0] = std::min(min[0], s->AabbMin.x); min[1] = std::min(min[1], s->AabbMin.y); min[2] = std::min(min[2], s->AabbMin.z);
            max[0] = std::max(max[0], s->AabbMax.x); max[1] = std::max(max[1], s->AabbMax.y); max[2] = std::max(max[2], s->AabbMax.z);
        }
    }
    if (!any) { for (int i = 0; i < 3; i++) { min[i] = 0; max[i] = 0; } }
    for (int i = 0; i < 3; i++) { min[i] = BWareMath::S(min[i], 0.1f); max[i] = BWareMath::A(max[i], 0.1f); }
}

uint32_t CarFxSetupU2::HeadlightColour(const RideInfoU2& ride) {
    const int part = ride.Parts[CarHeadlightsU2::BulbSlot];
    if (part < 0) return CarHeadlightsU2::DefaultColour;
    const CarPartDatabaseU2* db = ride.Db;
    const uint32_t r = db->GetAttribute(part, BinHash::Hash("RED"));
    const uint32_t g = db->GetAttribute(part, BinHash::Hash("GREEN"));
    const uint32_t b = db->GetAttribute(part, BinHash::Hash("BLUE"));
    return ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
}

bool CarFxSetupU2::HasCustomSpoiler(const RideInfoU2& ride) {
    const int p = ride.Parts[12];
    return p >= 0 && ride.Db->Parts[(size_t)p].UpgradeLevel() > 0;
}

std::vector<std::vector<float>> CarFxSetupU2::HullPoints(const RideInfoU2& ride,
                                                         const std::vector<std::unique_ptr<SolidList>>& packs) {
    uint32_t cv = 0;
    const int body = ride.Parts[5];
    if (body >= 0) cv = ride.Db->GetAttribute(body, BinHash::Hash("CV"));
    const uint32_t fallback = BinHash::Hash(ride.CarType->CollectionName + "_CV");
    const CollisionHull* found = nullptr;
    for (const auto& p : packs) {
        if (!p) continue;
        for (const CollisionHull& h : p->Collision) {
            if (cv != 0 && h.NameHash == cv) { found = &h; break; }
            if (found == nullptr && h.NameHash == fallback) found = &h;
        }
        if (found != nullptr && found->NameHash == cv) break;
    }
    std::vector<std::vector<float>> pts;
    if (found == nullptr) return pts;
    pts.reserve(found->Points.size());
    for (const Vec3& q : found->Points) pts.push_back({ q.x, q.y, q.z });
    return pts;
}

} // namespace bb
