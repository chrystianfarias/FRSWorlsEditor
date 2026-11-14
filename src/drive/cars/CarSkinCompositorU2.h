#pragma once

// Port of NFSU2Unity Core/Cars/CarSkinCompositorU2.cs: builds the DUMMY_SKINn texture of a racer,
// CompositeSkin (0x61D790) + Composite32 (0x611F10). Output is ARGB32 texels (0xAARRGGBB),
// row 0 = top, alpha = GLOSS. See docs/re/car-textures.md section 6.

#include "drive/cars/RideInfoU2.h"
#include "drive/textures/TexturePack.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace bb {

struct CarSkinCompositorU2 {
    // GetDoorLineHash (0x612B70).
    static uint32_t DoorLineHash(const RideInfoU2& ride);

    // Composites the skin. dst is the pristine DUMMY_SKINn. Returns an empty vector when the game
    // would not composite (no BASE_PAINT, missing NUMCOLOURS...).
    static std::vector<uint32_t> Composite(const RideInfoU2& ride, const Texture* dst,
                                           const std::function<const Texture*(uint32_t)>& findTexture);
};

} // namespace bb
