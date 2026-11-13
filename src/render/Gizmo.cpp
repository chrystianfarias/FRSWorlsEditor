#include "render/Gizmo.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr u32 kAxisColour[3] = { 0xFFE24848, 0xFF4CCB4C, 0xFF4A86FF };
constexpr u32 kHotColour     = 0xFFFFE045;
constexpr u32 kCentreColour  = 0xFFE8E8E8;
constexpr int kRingSegments  = 64;
constexpr float kGrabPixels  = 9.0f;

float DistanceToSegment(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab{ b.x - a.x, b.y - a.y };
    const float length2 = ab.x * ab.x + ab.y * ab.y;
    float t = length2 > 1e-6f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / length2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = a.x + ab.x * t - p.x, dy = a.y + ab.y * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Two unit vectors square to `axis` and to each other, for drawing a ring.
void Basis(const Vec3& axis, Vec3& u, Vec3& v) {
    const Vec3 other = std::fabs(axis.z) < 0.9f ? Vec3{ 0.0f, 0.0f, 1.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
    u = Normalize(Cross(axis, other));
    v = Cross(axis, u);
}

Vec3 RingPoint(const Vec3& origin, const Vec3& axis, float radius, int k) {
    Vec3 u, v;
    Basis(axis, u, v);
    const float a = 2.0f * kPi * (float)k / kRingSegments;
    return origin + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
}

void Line(std::vector<GizmoVertex>& out, const Vec3& a, const Vec3& b, u32 colour) {
    out.push_back({ a.x, a.y, a.z, colour });
    out.push_back({ b.x, b.y, b.z, colour });
}

void Triangle(std::vector<GizmoVertex>& out, const Vec3& a, const Vec3& b, const Vec3& c, u32 colour) {
    out.push_back({ a.x, a.y, a.z, colour });
    out.push_back({ b.x, b.y, b.z, colour });
    out.push_back({ c.x, c.y, c.z, colour });
}

// A shaft with some body: a thin strip turned to face the eye, since a
// one-pixel line is hard to see against the city and harder to aim at.
void Shaft(std::vector<GizmoVertex>& out, const Vec3& a, const Vec3& b, const Vec3& eye, float width,
           u32 colour) {
    const Vec3 along = Normalize(b - a);
    Vec3 side = Cross(along, eye - a);
    if (Length(side) < 1e-6f) return;
    side = Normalize(side) * (width * 0.5f);
    Triangle(out, a - side, a + side, b + side, colour);
    Triangle(out, a - side, b + side, b - side, colour);
}

void Cone(std::vector<GizmoVertex>& out, const Vec3& base, const Vec3& axis, float length, float radius,
          u32 colour) {
    Vec3 u, v;
    Basis(axis, u, v);
    const Vec3 tip = base + axis * length;
    constexpr int kSides = 12;
    for (int k = 0; k < kSides; ++k) {
        const float a0 = 2.0f * kPi * k / kSides, a1 = 2.0f * kPi * (k + 1) / kSides;
        const Vec3 p0 = base + u * (std::cos(a0) * radius) + v * (std::sin(a0) * radius);
        const Vec3 p1 = base + u * (std::cos(a1) * radius) + v * (std::sin(a1) * radius);
        Triangle(out, tip, p0, p1, colour);
        Triangle(out, base, p1, p0, colour);
    }
}

void Box(std::vector<GizmoVertex>& out, const Vec3& centre, float half, u32 colour) {
    Vec3 c[8];
    for (int k = 0; k < 8; ++k)
        c[k] = centre + Vec3{ (k & 1) ? half : -half, (k & 2) ? half : -half, (k & 4) ? half : -half };
    const int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 },
                              { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    for (const auto& f : faces) {
        Triangle(out, c[f[0]], c[f[1]], c[f[2]], colour);
        Triangle(out, c[f[0]], c[f[2]], c[f[3]], colour);
    }
}

// The ground square of the move handles: a quarter of the way out along X
// and Y, so it sits between the two arrows.
void SquareCorners(const Vec3& origin, float size, Vec3 corners[4]) {
    const float a = size * 0.22f, b = size * 0.42f;
    corners[0] = origin + Vec3{ a, a, 0.0f };
    corners[1] = origin + Vec3{ b, a, 0.0f };
    corners[2] = origin + Vec3{ b, b, 0.0f };
    corners[3] = origin + Vec3{ a, b, 0.0f };
}

bool InsideTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    const auto side = [](const Vec2& p1, const Vec2& p2, const Vec2& p3) {
        return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
    };
    const float d1 = side(p, a, b), d2 = side(p, b, c), d3 = side(p, c, a);
    const bool negative = d1 < 0 || d2 < 0 || d3 < 0, positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

} // namespace

Vec3 GizmoAxis(int handle) {
    switch (handle) {
        case 0:  return { 1.0f, 0.0f, 0.0f };
        case 1:  return { 0.0f, 1.0f, 0.0f };
        default: return { 0.0f, 0.0f, 1.0f };
    }
}

float GizmoSize(const Vec3& origin, const GizmoView& view) {
    return std::max(0.3f, Length(origin - view.eye) * 0.16f);
}

bool GizmoProject(const GizmoView& view, const Vec3& point, Vec2& pixel) {
    const auto& m = view.viewProjection.m;
    const float x = point.x * m[0][0] + point.y * m[1][0] + point.z * m[2][0] + m[3][0];
    const float y = point.x * m[0][1] + point.y * m[1][1] + point.z * m[2][1] + m[3][1];
    const float w = point.x * m[0][3] + point.y * m[1][3] + point.z * m[2][3] + m[3][3];
    if (w <= 1e-4f) return false;
    pixel = { (x / w * 0.5f + 0.5f) * view.width, (0.5f - y / w * 0.5f) * view.height };
    return true;
}

int GizmoHit(GizmoMode mode, const Vec3& origin, const GizmoView& view, float mx, float my) {
    const float size = GizmoSize(origin, view);
    const Vec2 mouse{ mx, my };
    Vec2 centre;
    if (!GizmoProject(view, origin, centre)) return kGizmoNone;

    int best = kGizmoNone;
    float bestDistance = kGrabPixels;
    const auto consider = [&](int handle, float distance) {
        if (distance < bestDistance) { bestDistance = distance; best = handle; }
    };

    if (mode == GizmoMode::Rotate) {
        for (int axis = 0; axis < 3; ++axis) {
            Vec2 previous;
            bool havePrevious = false;
            for (int k = 0; k <= kRingSegments; ++k) {
                Vec2 now;
                const bool ok = GizmoProject(view, RingPoint(origin, GizmoAxis(axis), size * 0.85f, k), now);
                if (ok && havePrevious) consider(axis, DistanceToSegment(mouse, previous, now));
                previous = now;
                havePrevious = ok;
            }
        }
        return best;
    }

    if (mode == GizmoMode::Move) {
        Vec3 corners[4];
        SquareCorners(origin, size, corners);
        Vec2 p[4];
        bool all = true;
        for (int k = 0; k < 4; ++k) all = all && GizmoProject(view, corners[k], p[k]);
        if (all && (InsideTriangle(mouse, p[0], p[1], p[2]) || InsideTriangle(mouse, p[0], p[2], p[3])))
            return kGizmoCentre;
    } else {
        const float dx = mx - centre.x, dy = my - centre.y;
        if (std::sqrt(dx * dx + dy * dy) < kGrabPixels * 1.4f) return kGizmoCentre;
    }
    for (int axis = 0; axis < 3; ++axis) {
        Vec2 end;
        if (!GizmoProject(view, origin + GizmoAxis(axis) * size, end)) continue;
        consider(axis, DistanceToSegment(mouse, centre, end));
    }
    return best;
}

void BuildGizmo(GizmoMode mode, const Vec3& origin, const GizmoView& view, int hot,
                std::vector<GizmoVertex>& lines, std::vector<GizmoVertex>& triangles) {
    const float size = GizmoSize(origin, view);
    const auto colour = [&](int handle) { return handle == hot ? kHotColour : kAxisColour[handle]; };

    if (mode == GizmoMode::Rotate) {
        for (int axis = 0; axis < 3; ++axis) {
            const u32 c = colour(axis);
            // Three rings a hair apart read as one line of some weight.
            for (float spread : { -0.006f, 0.0f, 0.006f })
                for (int k = 0; k < kRingSegments; ++k)
                    Line(lines, RingPoint(origin, GizmoAxis(axis), size * (0.85f + spread), k),
                         RingPoint(origin, GizmoAxis(axis), size * (0.85f + spread), k + 1), c);
        }
        return;
    }

    const float width = size * 0.035f;
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3 a = GizmoAxis(axis);
        const u32 c = colour(axis);
        if (mode == GizmoMode::Move) {
            Shaft(triangles, origin, origin + a * (size * 0.8f), view.eye, width, c);
            Cone(triangles, origin + a * (size * 0.8f), a, size * 0.2f, size * 0.07f, c);
        } else {
            Shaft(triangles, origin, origin + a * (size * 0.88f), view.eye, width, c);
            Box(triangles, origin + a * size, size * 0.07f, c);
        }
    }
    if (mode == GizmoMode::Move) {
        Vec3 corners[4];
        SquareCorners(origin, size, corners);
        const u32 fill = hot == kGizmoCentre ? 0xC0FFE045 : 0x70FFE045;
        Triangle(triangles, corners[0], corners[1], corners[2], fill);
        Triangle(triangles, corners[0], corners[2], corners[3], fill);
        for (int k = 0; k < 4; ++k) Line(lines, corners[k], corners[(k + 1) % 4], kHotColour);
    } else {
        Box(triangles, origin, size * 0.09f, hot == kGizmoCentre ? kHotColour : kCentreColour);
    }
}

Mat4 AxisRotation(const Vec3& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float x = axis.x, y = axis.y, z = axis.z;
    // The column form of Rodrigues' rotation, transposed for row vectors.
    const float r[3][3] = { { t * x * x + c,     t * x * y - s * z, t * x * z + s * y },
                            { t * x * y + s * z, t * y * y + c,     t * y * z - s * x },
                            { t * x * z - s * y, t * y * z + s * x, t * z * z + c } };
    Mat4 m = Mat4::Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[i][j] = r[j][i];
    return m;
}
