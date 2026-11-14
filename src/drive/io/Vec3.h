#pragma once

// Port of NFSU2Unity Core/IO/Vec3.cs.

namespace bb {

/// Game-space vector: meters, X forward, Y left, Z up (right-handed).
struct Vec3 {
    float X = 0, Y = 0, Z = 0;

    Vec3() = default;
    Vec3(float x, float y, float z) : X(x), Y(y), Z(z) {}
};

} // namespace bb
