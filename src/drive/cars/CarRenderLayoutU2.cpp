#include "drive/cars/CarRenderLayoutU2.h"

#include "drive/cars/CarBin.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bb {

namespace {

const uint32_t MarkerBrake = carbin::Hash("MARKER_BRAKE");
const uint32_t MarkerSpoiler = carbin::Hash("SPOILER");
const uint32_t MarkerRoofScoop = carbin::Hash("ROOF_SCOOP");
const uint32_t MarkerMirrorLeft = carbin::Hash("MIRROR_BODY_LEFT");
const uint32_t MarkerMirrorRight = carbin::Hash("MIRROR_BODY_RIGHT");
const uint32_t AttrSpinnerOffset = carbin::Hash("SPINNEROFFSET");

// Exhaust tip markers (+0x598): EXHAUST, EXHAUST_LEFT, EXHAUST_RIGHT and three unnamed hashes.
const uint32_t exhaustMarkers[6] = { 0xA5B28001, 0xA2799C2B, 0xF21BE91E, 0xBCF8A18B, 0xBD7CF15E, 0xDF066A41 };
// Rear plate markers (+0x620): REAR_LICENSE_PLATE, LICENSE_PLATE_REAR.
const uint32_t rearPlateMarkers[2] = { 0xC805F7E0, 0x494ED280 };

// HasSunroof (0x60C850): car types 240SX, CIVIC, RSX, G35.
bool HasSunroof(int carType) { return carType == 3 || carType == 5 || carType == 18 || carType == 23; }

// ShowTrunkUnderInFE (0x60C8F0).
bool ShowTrunkUnderInFE(int t) {
    return (t >= 0 && t <= 3) || (t >= 6 && t <= 11) || t == 13 || (t >= 15 && t <= 20) || t == 22 || t == 24 ||
           t == 29;
}

Mat4 Translation(float x, float y, float z) { return Mat4::Translation({ x, y, z }); }
Mat4 Scale(float x, float y, float z) { return Mat4::Scale({ x, y, z }); }

// eCreateRotationZ (0x5BAC90): RZ = [[c,s,0],[-s,c,0],[0,0,1]]; 65536 = 360 degrees.
Mat4 RotationZ(int angle) {
    const float a = angle * (float)(3.14159265358979323846 * 2.0 / 65536.0);
    const float c = std::cos(a), s = std::sin(a);
    Mat4 m = Mat4::Identity();
    m.m[0][0] = c;
    m.m[0][1] = s;
    m.m[1][0] = -s;
    m.m[1][1] = c;
    return m;
}

Mat4 RotationY(int angle) {
    const float a = angle * (float)(3.14159265358979323846 * 2.0 / 65536.0);
    const float c = std::cos(a), s = std::sin(a);
    Mat4 m = Mat4::Identity();
    m.m[0][0] = c;
    m.m[0][2] = -s;
    m.m[2][0] = s;
    m.m[2][2] = c;
    return m;
}

bool FindMarker(const Solid* s, uint32_t hash, Mat4& out) {
    if (!s) return false;
    for (const PositionMarker& m : s->Markers)
        if (m.NameHash == hash) {
            out = m.ToMat4();
            return true;
        }
    return false;
}

} // namespace

CarRenderLayoutU2::CarRenderLayoutU2(const RideInfoU2* ride, std::function<const Solid*(uint32_t)> findSolid)
    : Ride(ride), findSolid_(std::move(findSolid)) {
    // CarRenderInfo::UpdateCarParts (0x634800): missing solids reuse the previous LOD.
    const CarPartDatabaseU2& db = *Ride->Db;
    for (int slot = 0; slot < NumModelSlots; slot++) {
        const int part = Ride->Parts[slot];
        if (part < 0 || (slot >= 35 && slot <= 46)) continue;
        for (int row = 0; row < 2; row++)
            for (int lod = 0; lod < 4; lod++) {
                const uint32_t h = db.ModelNameHash(part, row, lod);
                if (h == 0) continue;
                const Solid* s = findSolid_(h);
                models_[slot][row][lod] = s ? s : (lod > 0 ? models_[slot][row][lod - 1] : nullptr);
            }
    }
}

int CarRenderLayoutU2::Level(int slot) const {
    return Ride->Parts[slot] >= 0 ? Ride->Db->Parts[Ride->Parts[slot]].UpgradeLevel() : 0;
}

float CarRenderLayoutU2::RestBodyZ() const {
    float sum = 0;
    for (const CarWheelInfo& w : Ride->CarType->Wheels) sum += w.Radius - w.RideHeight;
    return sum * 0.25f;
}

float CarRenderLayoutU2::WheelWidth(int i) const {
    const CarPartDatabaseU2& db = *Ride->Db;
    const int fw = Ride->Parts[29], rw = Ride->Parts[30];
    const int lf = fw >= 0 ? db.Parts[fw].UpgradeLevel() : 0, lr = rw >= 0 ? db.Parts[rw].UpgradeLevel() : 0;
    const CarWheelInfo& w = Ride->CarType->Wheels[i];
    float width = ((i < 2) ? lf : lr) != 0 ? w.Width : WheelStandardWidth;
    if (width == 0) width = WheelStandardWidth;
    return width;
}

Vec3 CarRenderLayoutU2::WheelCentre(int i) const {
    const CarWheelInfo& w = Ride->CarType->Wheels[i];
    const float y = Level(6) > 0 ? w.WideBodyY : w.Y;
    const float sgn = y > 0 ? 1.0f : -1.0f;
    return { w.X, y - sgn * WheelWidth(i) * WheelPivotTranslationAmount, w.RideHeight };
}

std::vector<CarDrawItem> CarRenderLayoutU2::Build(int carLod, int tireLod, bool atRest) const {
    std::vector<CarDrawItem> items;
    const Mat4 body = atRest ? Translation(0, 0, RestBodyZ() + BodyZBias) : Mat4::Identity();
    const AddFn add = [&items](const Solid* s, const Mat4& m, int slot, int row, const char* label, bool leftBrake) {
        if (!s) return;
        CarDrawItem it;
        it.Solid_ = s;
        it.Matrix = m;
        it.Slot = slot;
        it.Row = row;
        it.Label = label;
        it.LeftBrake = leftBrake;
        items.push_back(std::move(it));
    };
    BodyParts(carLod, body, add);
    MarkerParts(carLod, body, add);
    Wheels(carLod, tireLod, body, items);
    return items;
}

// Pass 1 (0x623040..0x6244CF).
void CarRenderLayoutU2::BodyParts(int carLod, const Mat4& body, const AddFn& add) const {
    const int carType = Ride->CarType->Index;
    const bool wideBody = Level(6) > 0;
    for (int slot = 0; slot < NumModelSlots; slot++)
        for (int row = 0; row < 2; row++) {
            const Solid* m = models_[slot][row][carLod];
            if (!m) continue;
            if (slot == 3 || slot == 4 || slot == 7 || slot == 12 || slot == 13 || slot == 16 || (slot >= 27 && slot <= 46))
                continue;
            bool show = Ride->Enabled[slot];
            switch (slot) {
                case 8: // TOP: row 1 (FULLROOF) only on sunroof cars with a roof scoop
                    if (HasSunroof(carType) && Level(7) > 0) {
                        if (row == 0) show = false;
                    } else if (row == 1)
                        show = false;
                    break;
                case 1:
                case 2:
                case 11:
                    if (carLod >= 3) continue;
                    break;
                case 9: // HOOD: ignores the enabled flag; row 1 only at LOD A
                    show = !(row == 1 && carLod > 0);
                    if (carLod >= 3) continue;
                    break;
                case 10: // TRUNK (DrawTrunkAudio not modelled: always false)
                    if (carLod >= 3) continue;
                    break;
                case 17:
                case 18:
                    if (wideBody && carLod > 2) continue;
                    break;
                case 19:
                case 21:
                    show = LeftDoorOpen || FrontEnd;
                    if (carLod > 0) continue;
                    break;
                case 20:
                case 22:
                    show = RightDoorOpen || FrontEnd;
                    if (carLod > 0) continue;
                    break;
                case 25:
                    show = HoodOpen;
                    break;
                case 26:
                    show = FrontEnd ? TrunkOpen || ShowTrunkUnderInFE(carType) : TrunkOpen;
                    if (carLod > 0) continue;
                    break;
                default:
                    break;
            }
            if (show) add(m, body, slot, row, CarSlotsU2::SlotNames[slot], false);
        }
}

// Emitter lists (0x6395A0): matching markers on every enabled part's Models[slot][0][0].
std::vector<Mat4> CarRenderLayoutU2::CollectMarkers(const uint32_t* hashes, int n) const {
    std::vector<Mat4> list;
    for (int slot = 0; slot < NumModelSlots; slot++) {
        const Solid* s = models_[slot][0][0];
        if (!s || !Ride->Enabled[slot]) continue;
        for (const PositionMarker& mk : s->Markers)
            if (std::find(hashes, hashes + n, mk.NameHash) != hashes + n) list.push_back(mk.ToMat4());
    }
    return list;
}

// Passes 2-4: exhaust tips, rear plate, spoiler, roof scoop, mirrors.
void CarRenderLayoutU2::MarkerParts(int carLod, const Mat4& body, const AddFn& add) const {
    const Mat4 ry90 = RotationY(0x4000);
    if (carLod <= 1)
        for (const Mat4& marker : CollectMarkers(exhaustMarkers, 6)) {
            const Mat4 m = ry90 * marker * body;
            for (int row = 0; row < 2; row++) add(models_[16][row][0], m, 16, row, "EXHAUST", false);
        }
    if (carLod <= 2)
        for (const Mat4& marker : CollectMarkers(rearPlateMarkers, 2)) {
            const Mat4 m = ry90 * marker * body;
            for (int row = 0; row < 2; row++) add(models_[33][row][0], m, 33, row, "LICENSE_PLATE", false);
        }
    if (carLod >= 3) return;

    const Solid* baseA = models_[0][0][0];
    Mat4 spoiler;
    const bool hasSpoiler = FindMarker(baseA, MarkerSpoiler, spoiler);
    for (int row = 0; row < 2; row++)
        add(models_[12][row][carLod], hasSpoiler ? spoiler * body : body, 12, row, "SPOILER", false);
    Mat4 scoop;
    const bool hasScoop = FindMarker(baseA, MarkerRoofScoop, scoop);
    for (int row = 0; row < 2; row++) add(models_[7][row][carLod], hasScoop ? scoop * body : body, 7, row, "ROOF", false);

    if (Level(32) > 0) {
        Mat4 left = Mat4::Identity(), right = Mat4::Identity();
        FindMarker(baseA, MarkerMirrorLeft, left);
        FindMarker(baseA, MarkerMirrorRight, right);
        right.m[1][1] = -right.m[1][1]; // 0x626A3C: undo the marker's mirror
        add(models_[32][0][carLod], left * body, 32, 0, "WING_MIRROR", false);
        add(models_[32][1][carLod], right * body, 32, 1, "WING_MIRROR", false);
    } else {
        for (int row = 0; row < 2; row++) add(models_[32][row][carLod], body, 32, row, "WING_MIRROR", false);
    }
}

// Passes 5-6: wheels, spinners and brakes (section 5.2-5.4).
void CarRenderLayoutU2::Wheels(int carLod, int tireLod, const Mat4& body, std::vector<CarDrawItem>& items) const {
    const CarPartDatabaseU2& db = *Ride->Db;
    const CarTypeInfoU2& ct = *Ride->CarType;
    const bool wideBody = Level(6) > 0;
    const int fw = Ride->Parts[29];
    float spin = 0;
    if (fw >= 0 && db.HasAttribute(fw, AttrSpinnerOffset)) {
        const uint32_t bits = db.GetAttribute(fw, AttrSpinnerOffset);
        std::memcpy(&spin, &bits, 4);
    }
    Mat4 mk;
    const float brakeYFront = FindMarker(models_[29][0][0], MarkerBrake, mk) ? mk.m[3][1] : 0.0f;
    const float brakeYRear = FindMarker(models_[30][0][0], MarkerBrake, mk) ? mk.m[3][1] : 0.0f;
    const int tLod = std::min(tireLod, 2);
    const Mat4 rz180 = RotationZ(0x8000);
    const Mat4 steer = RotationZ((int)(SteerDegrees * 65536.0f / 360.0f));
    Mat4 tireMats[4];
    Mat4 c[4];
    float brakeLocalY[4] = {};

    auto push = [&items](const Solid* s, const Mat4& m, int slot, int row, const char* label, bool leftBrake, int wheel,
                         const Mat4& local, bool brake) {
        if (!s) return;
        CarDrawItem it;
        it.Solid_ = s;
        it.Matrix = m;
        it.Slot = slot;
        it.Row = row;
        it.Label = label;
        it.LeftBrake = leftBrake;
        it.Wheel = wheel;
        it.Local = local;
        it.Brake = brake;
        items.push_back(std::move(it));
    };

    for (int i = 0; i < 4; i++) {
        const CarWheelInfo& w = ct.Wheels[i];
        const float y = wideBody ? w.WideBodyY : w.Y;
        const float width = WheelWidth(i);
        const float s = width / WheelStandardWidth, p = WheelPivotTranslationAmount;
        const float sgn = y > 0 ? 1.0f : -1.0f;
        const float f = i < 2 ? brakeYFront : brakeYRear;

        Mat4 a = Scale(1, s, 1);
        a.m[3][1] = -width * p;
        const Mat4 b = Translation(0, y - sgn * width * p, 0);
        const Mat4 d = Translation(0, y - sgn * width * p + sgn * spin, 0);
        const float t = f != 0 ? s * f : width * p;
        c[i] = Translation(0, y - sgn * t, 0);
        brakeLocalY[i] = (y - sgn * t) - (y - sgn * width * p);

        // FE pose (0x4A6670): steer on the fronts, translation (X, Y, RideHeight) with Y zeroed by Render.
        Mat4 tm = i < 2 ? steer : Mat4::Identity();
        tm.m[3][0] = w.X;
        tm.m[3][1] = 0;
        tm.m[3][2] = w.RideHeight;
        tireMats[i] = tm;

        if (!Ride->Enabled[29] || carLod > 2) continue;
        const Mat4 side = i == 0 || i == 3 ? a * rz180 : a;
        const int slot = i < 2 ? 29 : 30;
        push(models_[slot][0][tLod], side * tm * b * body, slot, 0, "WHEEL", false, i, side, false);
        if (models_[29][1][0])
            push(models_[slot][1][tLod], side * tm * d * body, slot, 1, "SPINNER", false, i,
                 side * Translation(0, sgn * spin, 0), false);
    }

    if (carLod > 1 || !models_[27][0][0]) return;
    const int bl = FrontEnd ? 0 : 1;
    const Solid* front = models_[27][0][bl];
    const Solid* rear = models_[27][Level(27) == 0 ? 1 : 0][bl];
    const Mat4 mirror = Scale(1, -1, 1);
    push(front, mirror * tireMats[0] * c[0] * body, 27, 0, "BRAKE", true, 0, mirror * Translation(0, brakeLocalY[0], 0), true);
    push(front, tireMats[1] * c[1] * body, 27, 0, "BRAKE", false, 1, Translation(0, brakeLocalY[1], 0), true);
    push(rear, tireMats[2] * c[2] * body, 27, 1, "BRAKE", false, 2, Translation(0, brakeLocalY[2], 0), true);
    push(rear, mirror * tireMats[3] * c[3] * body, 27, 1, "BRAKE", true, 3, mirror * Translation(0, brakeLocalY[3], 0), true);
}

} // namespace bb
