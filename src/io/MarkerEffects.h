#pragma once

#include "io/SectionSplice.h"
#include "nfs/Markers.h"

#include <string>
#include <vector>

namespace nfs { class World; }

// The icon, the ring and the smoke a shop marker shows.
//
// None of it comes from the shop: they are four particle emitters placed in
// the streaming section around the shop, in its 0x35020 chunk --
// XEE_Event_<KIND>_IconA, _RingA, _SmokeA and _SmokeB, 208 bytes each, defined
// in the region's emitter library (0x35021 in L4Rx.BUN, which names the icon
// texture: SHOP_ICON_BODY, SHOP_ICON_CAR_LOT, ...). A placed shop gets a copy
// of the set a shipped shop of its kind carries, turned and moved to the
// marker, in the section the marker stands in.
//
// The sections are rebuilt through the edit list (Kind::Emitters), so the
// effects sit beside every other edit in them and can be reverted on their own.
struct MarkerEffectsPlan {
    std::vector<SectionSpan>  spans;
    std::vector<std::wstring> superseded;
    size_t sections = 0;        // sections rebuilt
    size_t effects  = 0;        // emitters placed
    std::wstring report;
};

// Where a marker's emitters will stand, exactly as PlanMarkerEffects places
// them, for drawing in the editor: the name says which (_IconA the icon over
// it, _RingA on the ground, _SmokeA/B the column). Empty for a marker with no
// set (a race start). The shipped emitters are read once per region.
struct MarkerEffectPreview {
    Vec3 position;
    std::string name;
};
std::vector<MarkerEffectPreview> PreviewMarkerEffects(const nfs::World& world, const nfs::MarkerSet& markers,
                                                      const nfs::Marker& marker);

bool PlanMarkerEffects(const nfs::World& world, const std::wstring& modelsFolder,
                       const nfs::MarkerSet& markers, MarkerEffectsPlan& plan,
                       std::wstring& error);
