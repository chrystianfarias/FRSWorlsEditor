#pragma once

#include <cmath>
#include <algorithm>

// The NFSU2 world is Z-up and right-handed: X/Y form the ground plane and Z is
// altitude. Matrices are stored row-major and vectors are treated as rows, so a
// point is transformed as "v * M" and translation lives in row 3. That matches
// the layout the game stores in its solid object headers, which lets us upload
// them to Direct3D without transposing.

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s)       const { return { x * s, y * s, z * s }; }
    Vec3 operator-()              const { return { -x, -y, -z }; }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Normalize(const Vec3& v) {
    const float len = Length(v);
    return len > 1e-8f ? v * (1.0f / len) : Vec3{};
}

inline Vec3 Min(const Vec3& a, const Vec3& b) {
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
}

inline Vec3 Max(const Vec3& a, const Vec3& b) {
    return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
}

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// Axis-aligned bounding box. A default-constructed box is empty, so growing it
// with Add() yields the tight bounds of whatever was inserted.
struct Aabb {
    Vec3 min{  1e30f,  1e30f,  1e30f };
    Vec3 max{ -1e30f, -1e30f, -1e30f };

    bool Valid() const { return min.x <= max.x; }

    void Add(const Vec3& p) { min = Min(min, p); max = Max(max, p); }

    void Add(const Aabb& b) {
        if (!b.Valid()) return;
        min = Min(min, b.min);
        max = Max(max, b.max);
    }

    Vec3 Center() const { return (min + max) * 0.5f; }
    Vec3 Extent() const { return (max - min) * 0.5f; }

    float Radius() const { return Valid() ? Length(Extent()) : 0.0f; }
};

struct Mat4 {
    // m[row][column]
    float m[4][4] = {};

    static Mat4 Identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r = Identity();
        r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
        return r;
    }

    static Mat4 Scale(const Vec3& s) {
        Mat4 r = Identity();
        r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
        return r;
    }

    Vec3 TransformPoint(const Vec3& v) const {
        return { v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0],
                 v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1],
                 v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2] };
    }

    Vec3 TransformDirection(const Vec3& v) const {
        return { v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0],
                 v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
                 v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] };
    }
};

// Signed volume scale of a transform's rotation part. Negative means the
// transform mirrors, which reverses the winding of every triangle it moves.
inline float Determinant3x3(const Mat4& m) {
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
           m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
           m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
}

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Grows a box outwards by the same amount on every axis.
inline Aabb Pad(const Aabb& box, float amount) {
    if (!box.Valid()) return box;
    const Vec3 margin{ amount, amount, amount };
    Aabb out;
    out.min = box.min - margin;
    out.max = box.max + margin;
    return out;
}

// Transforms every corner of the box and returns the bounds of the result.
inline Aabb Transform(const Aabb& box, const Mat4& matrix) {
    if (!box.Valid()) return {};
    Aabb out;
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{ (i & 1) ? box.max.x : box.min.x,
                           (i & 2) ? box.max.y : box.min.y,
                           (i & 4) ? box.max.z : box.min.z };
        out.Add(matrix.TransformPoint(corner));
    }
    return out;
}

// Inverse of a matrix that only rotates, scales and translates. Picking needs
// it to carry a ray into the local space a mesh is stored in.
inline Mat4 InverseAffine(const Mat4& m) {
    const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
    const float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
    const float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];

    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(determinant) < 1e-12f) return Mat4::Identity();
    const float inv = 1.0f / determinant;

    Mat4 r = Mat4::Identity();
    r.m[0][0] = (e * i - f * h) * inv;
    r.m[0][1] = (c * h - b * i) * inv;
    r.m[0][2] = (b * f - c * e) * inv;
    r.m[1][0] = (f * g - d * i) * inv;
    r.m[1][1] = (a * i - c * g) * inv;
    r.m[1][2] = (c * d - a * f) * inv;
    r.m[2][0] = (d * h - e * g) * inv;
    r.m[2][1] = (b * g - a * h) * inv;
    r.m[2][2] = (a * e - b * d) * inv;

    const Vec3 t{ m.m[3][0], m.m[3][1], m.m[3][2] };
    r.m[3][0] = -(t.x * r.m[0][0] + t.y * r.m[1][0] + t.z * r.m[2][0]);
    r.m[3][1] = -(t.x * r.m[0][1] + t.y * r.m[1][1] + t.z * r.m[2][1]);
    r.m[3][2] = -(t.x * r.m[0][2] + t.y * r.m[1][2] + t.z * r.m[2][2]);
    return r;
}

// A ray for picking, in whatever space the caller is working in.
struct Ray {
    Vec3 origin;
    Vec3 direction;   // expected to be unit length
};

// Slab test. Returns the entry distance, or a negative value when the ray
// misses. A ray starting inside the box returns 0.
inline float IntersectRayAabb(const Ray& ray, const Aabb& box) {
    if (!box.Valid()) return -1.0f;

    float enter = 0.0f;
    float exit  = 1e30f;

    const float origin[3]    = { ray.origin.x, ray.origin.y, ray.origin.z };
    const float direction[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
    const float low[3]       = { box.min.x, box.min.y, box.min.z };
    const float high[3]      = { box.max.x, box.max.y, box.max.z };

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(direction[axis]) < 1e-9f) {
            if (origin[axis] < low[axis] || origin[axis] > high[axis]) return -1.0f;
            continue;
        }
        const float inv = 1.0f / direction[axis];
        // Not "near" and "far": <windows.h> defines both as macros.
        float closer  = (low[axis] - origin[axis]) * inv;
        float farther = (high[axis] - origin[axis]) * inv;
        if (closer > farther) std::swap(closer, farther);
        enter = std::max(enter, closer);
        exit  = std::min(exit, farther);
        if (enter > exit) return -1.0f;
    }
    return enter;
}

// Moller-Trumbore. Returns the distance along the ray, or a negative value when
// the triangle is missed. Back faces count: a map editor has to be able to pick
// a wall from behind.
inline float IntersectRayTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 pvec  = Cross(ray.direction, edge2);
    const float determinant = Dot(edge1, pvec);

    if (std::fabs(determinant) < 1e-12f) return -1.0f;
    const float inv = 1.0f / determinant;

    const Vec3 tvec = ray.origin - a;
    const float u = Dot(tvec, pvec) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;

    const Vec3 qvec = Cross(tvec, edge1);
    const float v = Dot(ray.direction, qvec) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;

    const float distance = Dot(edge2, qvec) * inv;
    return distance > 0.0f ? distance : -1.0f;
}

// Right-handed look-at for a Z-up world.
inline Mat4 LookAtRH(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 forward = Normalize(target - eye);      // camera looks along +forward
    const Vec3 right   = Normalize(Cross(forward, up));
    const Vec3 trueUp  = Cross(right, forward);
    const Vec3 back    = -forward;                     // view space looks down -Z

    Mat4 r = Mat4::Identity();
    r.m[0][0] = right.x;  r.m[0][1] = trueUp.x;  r.m[0][2] = back.x;
    r.m[1][0] = right.y;  r.m[1][1] = trueUp.y;  r.m[1][2] = back.y;
    r.m[2][0] = right.z;  r.m[2][1] = trueUp.z;  r.m[2][2] = back.z;
    r.m[3][0] = -Dot(right,  eye);
    r.m[3][1] = -Dot(trueUp, eye);
    r.m[3][2] = -Dot(back,   eye);
    return r;
}

// Right-handed perspective mapping z to the [0,1] depth range Direct3D expects.
inline Mat4 PerspectiveRH(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float h = 1.0f / std::tan(fovYRadians * 0.5f);
    const float w = h / aspect;

    Mat4 r;
    r.m[0][0] = w;
    r.m[1][1] = h;
    r.m[2][2] = farZ / (nearZ - farZ);
    r.m[2][3] = -1.0f;
    r.m[3][2] = nearZ * farZ / (nearZ - farZ);
    return r;
}

// One plane of the view frustum, stored as the coefficients of ax+by+cz+d = 0
// with the normal pointing into the volume.
struct Plane {
    Vec3  normal;
    float d = 0.0f;

    float Distance(const Vec3& p) const { return Dot(normal, p) + d; }
};

// Extracts the six clipping planes from a combined view-projection matrix
// (Gribb-Hartmann). Used to cull world sections before issuing draw calls.
struct Frustum {
    Plane planes[6];

    void Build(const Mat4& viewProjection) {
        const auto& m = viewProjection.m;
        auto set = [&](int index, float a, float b, float c, float d) {
            const float len = std::sqrt(a * a + b * b + c * c);
            const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
            planes[index].normal = { a * inv, b * inv, c * inv };
            planes[index].d      = d * inv;
        };
        set(0, m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]); // left
        set(1, m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]); // right
        set(2, m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]); // bottom
        set(3, m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]); // top
        set(4,          m[0][2],          m[1][2],          m[2][2],          m[3][2]);     // near
        set(5, m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]); // far
    }

    bool Intersects(const Aabb& box) const {
        if (!box.Valid()) return false;
        for (const Plane& plane : planes) {
            // Pick the corner furthest along the plane normal; if even that one
            // is behind the plane the whole box is outside.
            const Vec3 corner{ plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                               plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                               plane.normal.z >= 0.0f ? box.max.z : box.min.z };
            if (plane.Distance(corner) < 0.0f) return false;
        }
        return true;
    }
};

constexpr float kPi = 3.14159265358979323846f;

inline float Radians(float degrees) { return degrees * (kPi / 180.0f); }
inline float Degrees(float radians) { return radians * (180.0f / kPi); }

template <class T>
T Clamp(T value, T low, T high) { return value < low ? low : (value > high ? high : value); }
