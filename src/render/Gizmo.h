#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <vector>

// The handles drawn on the selected object: arrows to move it along an axis
// or across the ground, rings to turn it about an axis, and boxes to scale it.
//
// The handles follow the world's axes -- X red, Y green, Z (up) blue -- not
// the object's own, because "along the street" and "up" are what a person
// placing a lamp post means. They keep one size on screen however far away
// the object is, so they can always be grabbed.
enum class GizmoMode { Move, Rotate, Scale };

// Handle ids: an axis 0..2 (X, Y, Z), 3 the ground square (Move) or the
// middle box (Scale), -1 for none.
constexpr int kGizmoNone   = -1;
constexpr int kGizmoCentre = 3;

// What the handles are measured against: the view and projection together,
// the viewport's size in pixels, and where the eye is.
struct GizmoView {
    Mat4  viewProjection;
    float width = 1.0f, height = 1.0f;
    Vec3  eye;
};

struct GizmoVertex {
    float x, y, z;
    u32   colour;
};

// Length of the handles in world units for an object at `origin`.
float GizmoSize(const Vec3& origin, const GizmoView& view);

// Pixel position of a world point; false when it is behind the eye.
bool GizmoProject(const GizmoView& view, const Vec3& point, Vec2& pixel);

// The handle under the pixel (mx, my), or kGizmoNone.
int GizmoHit(GizmoMode mode, const Vec3& origin, const GizmoView& view, float mx, float my);

// The handles as lines and triangles, with `hot` drawn highlighted.
void BuildGizmo(GizmoMode mode, const Vec3& origin, const GizmoView& view, int hot,
                std::vector<GizmoVertex>& lines, std::vector<GizmoVertex>& triangles);

// The world axis a handle stands for.
Vec3 GizmoAxis(int handle);

// A turn of `angle` radians about the unit `axis`, as a row-vector matrix
// (the convention every transform in the editor uses).
Mat4 AxisRotation(const Vec3& axis, float angle);
