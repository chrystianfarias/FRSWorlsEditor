#include "drive/cars/CarSkinCompositorU2.h"

#include "drive/cars/CarBin.h"

#include <algorithm>

namespace bb {

namespace {

constexpr uint32_t AttrRed = 0x0000D99A, AttrGreen = 0x02DDC8F0, AttrBlue = 0x00136707, AttrGloss = 0x02DAAB07;

// Key colours marking the PAINT_ENGINE/SPOILER/BRAKE/EXHAUST/AUDIO/ROOF/MIRROR blocks.
const uint32_t keys[7] = { 0xBFFF0000, 0xBF00FF00, 0xBF0000FF, 0xBFFF00FF, 0xBF00FFFF, 0xBFFFFF00, 0xBFFFFFFF };
const int paintSlots[7] = { 68, 69, 70, 71, 72, 75, 76 };

struct Layer {
    std::vector<uint32_t> Image, Mask;
    uint32_t Remap = 0;
    uint32_t Colour[4] = {};
};

// attr RED | GREEN<<8 | BLUE<<16 | GLOSS<<24 (missing = 0).
uint32_t Rgba(const CarPartDatabaseU2& db, int part) {
    return (db.GetAttribute(part, AttrRed) & 0xFF) | (db.GetAttribute(part, AttrGreen) & 0xFF) << 8 |
           (db.GetAttribute(part, AttrBlue) & 0xFF) << 16 | (db.GetAttribute(part, AttrGloss) & 0xFF) << 24;
}

// 0x5BE040: swaps bytes 0 and 2 (RGBA-packed to D3D ARGB).
uint32_t ToArgb(uint32_t v) { return (v & 0xFF00FF00) | (v & 0xFF) << 16 | ((v >> 16) & 0xFF); }

// 0x611B00: weighted sum per channel with truncation, saturated at 255.
uint32_t Mix(const uint32_t* cols, const float* w, int n, bool maxAlpha) {
    int r = 0, g = 0, b = 0, a = 0;
    for (int i = 0; i < n; i++) {
        if (!(w[i] > 1.0f / 255.0f)) continue;
        const uint32_t c = cols[i];
        r += (int)(((c >> 16) & 0xFF) * w[i]);
        g += (int)(((c >> 8) & 0xFF) * w[i]);
        b += (int)((c & 0xFF) * w[i]);
        const int ai = (int)(((c >> 24) & 0xFF) * w[i]);
        if (maxAlpha) a = std::max(a, (int)(uint8_t)ai);
        else a += ai;
    }
    return (uint32_t)std::min(a, 255) << 24 | (uint32_t)std::min(r, 255) << 16 | (uint32_t)std::min(g, 255) << 8 |
           (uint32_t)std::min(b, 255);
}

std::vector<uint32_t> Argb(const Texture& t) {
    std::vector<uint32_t> o;
    int w = 0, h = 0;
    if (!t.ToArgb32(0, o, w, h)) o.clear();
    return o;
}

std::vector<uint32_t> Composite32(std::vector<uint32_t> d, uint32_t baseColour, uint32_t* paint,
                                  std::vector<Layer>& layers) {
    std::vector<int> keyPixels[7];
    for (int px = 0; px < (int)d.size(); px++)
        for (int c = 0; c < 7; c++)
            if (d[px] == keys[c]) {
                keyPixels[c].push_back(px);
                break;
            }
    for (int c = 0; c < 7; c++) paint[c] = ToArgb(paint[c]);
    const uint32_t fill = ToArgb(baseColour);
    std::fill(d.begin(), d.end(), fill);

    float wgt[4];
    uint32_t pair[2];
    float pairW[2];
    for (Layer& l : layers) {
        if (l.Remap != 0)
            for (int k = 0; k < 4; k++) l.Colour[k] = ToArgb(l.Colour[k]);
        const size_t n = std::min(d.size(), std::min(l.Image.size(), l.Mask.size()));
        for (size_t px = 0; px < n; px++) {
            uint32_t s = l.Image[px];
            const uint32_t m = (l.Mask[px] >> 8) & 0xFF;
            if (l.Remap != 0 && m > 0) {
                wgt[0] = ((s >> 16) & 0xFF) * (1.0f / 255.0f);
                wgt[1] = ((s >> 8) & 0xFF) * (1.0f / 255.0f);
                wgt[2] = (s & 0xFF) * (1.0f / 255.0f);
                wgt[3] = 0;
                s = Mix(l.Colour, wgt, 3, true);
            }
            if (m == 0) continue;
            if (m > 0x7F) {
                d[px] = s;
                continue;
            }
            const float t = m * 0.0078125f;
            pair[0] = s;
            pair[1] = d[px];
            pairW[0] = t;
            pairW[1] = 1.0f - t;
            d[px] = Mix(pair, pairW, 2, false);
        }
    }
    for (int c = 0; c < 7; c++)
        for (int px : keyPixels[c]) d[(size_t)px] = paint[c];
    return d;
}

} // namespace

uint32_t CarSkinCompositorU2::DoorLineHash(const RideInfoU2& ride) {
    const CarPartDatabaseU2& db = *ride.Db;
    std::string name = ride.CarType->BaseModelName;
    auto lvl = [&](int slot) { return ride.Parts[slot] >= 0 && db.Parts[ride.Parts[slot]].UpgradeLevel() > 0; };
    if (lvl(6)) name += "_DOORLINE_WIDEBODY";
    else if (lvl(1) || lvl(2) || lvl(11)) name += "_DOORLINE_KIT";
    else name += "_DOORLINE";
    return carbin::Hash(name);
}

std::vector<uint32_t> CarSkinCompositorU2::Composite(const RideInfoU2& ride, const Texture* dst,
                                                     const std::function<const Texture*(uint32_t)>& findTexture) {
    const CarPartDatabaseU2& db = *ride.Db;
    const uint32_t AttrNumColours = carbin::Hash("NUMCOLOURS"), AttrRemap = carbin::Hash("REMAP"),
                   AttrTexture = carbin::Hash("TEXTURE");
    const int basePart = ride.Parts[63];
    if (!dst || basePart < 0) return {};
    const uint32_t baseColour = Rgba(db, basePart);
    uint32_t paint[7];
    for (int i = 0; i < 7; i++) {
        const int p = ride.Parts[paintSlots[i]];
        if (p < 0) paint[i] = baseColour;
        else if (paintSlots[i] == 72 && !(ride.Parts[34] >= 0 && db.Parts[ride.Parts[34]].UpgradeLevel() >= 3))
            paint[i] = baseColour;
        else paint[i] = Rgba(db, p);
    }

    std::vector<Layer> layers;
    for (int i = 1; i <= 5; i++) {
        uint32_t hash;
        int vinyl = -1;
        if (i == 5) hash = DoorLineHash(ride);
        else {
            vinyl = ride.Parts[63 + i];
            for (int j = 1; j < i; j++)
                if (ride.Parts[63 + j] == vinyl) vinyl = -1;
            if (vinyl < 0) continue;
            // GetVinylLayerHash (0x61D620)
            hash = carbin::Hash(ride.CarType->BaseModelName + "_" + db.String((int)db.GetAttribute(vinyl, AttrTexture)));
            if (db.GetAttribute(vinyl, AttrNumColours) == 0) return {}; // aborts the composite
        }
        const Texture* tex = findTexture(hash);
        const Texture* mask = findTexture(carbin::Hash("_MASK", hash));
        if (!tex || !mask) continue;
        Layer l;
        l.Image = Argb(*tex);
        l.Mask = Argb(*mask);
        if (vinyl >= 0 && db.HasAttribute(vinyl, AttrRemap) && (l.Remap = db.GetAttribute(vinyl, AttrRemap)) != 0)
            for (int k = 0; k < 4; k++) {
                const int c = ride.Parts[77 + 4 * (i - 1) + k];
                l.Colour[k] = c >= 0 ? Rgba(db, c) : 0xFFu << (8 * k);
            }
        layers.push_back(std::move(l));
    }
    std::vector<uint32_t> d = Argb(*dst);
    if (d.empty()) return {};
    return Composite32(std::move(d), baseColour, paint, layers);
}

} // namespace bb
