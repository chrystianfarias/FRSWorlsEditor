#pragma once

#include "core/Math.h"

// Free-flying viewport camera for a Z-up world.
//
// Orientation is kept as yaw and pitch rather than a matrix so the horizon never
// rolls, which is what makes a fly-through feel predictable. Yaw is measured
// around +Z from the +X axis; pitch is clamped just short of straight up and
// straight down to avoid a degenerate up vector.
class Camera {
public:
    void SetPosition(const Vec3& position) { position_ = position; }
    const Vec3& Position() const { return position_; }

    void SetYawPitch(float yaw, float pitch);
    float Yaw()   const { return yaw_; }
    float Pitch() const { return pitch_; }

    // Points the camera at a box from far enough away to see all of it.
    void FrameBounds(const Aabb& bounds);

    // Looks from `eye` towards `target`, deriving yaw and pitch.
    void LookAt(const Vec3& eye, const Vec3& target);

    Vec3 Forward() const;
    Vec3 Right()   const;
    Vec3 Up()      const;

    // Adds a mouse delta in pixels. Sensitivity is radians per pixel.
    void Rotate(float deltaX, float deltaY);

    // Moves along the camera axes. `local` is (right, forward, up) in metres.
    void Move(const Vec3& local);

    Mat4 ViewMatrix() const;
    Mat4 ProjectionMatrix(float aspect) const;

    // Uses the given matrices verbatim instead of deriving them from position
    // and angles. Set while the viewport mirrors the running game: taking the
    // game's own matrices removes every question about handedness, field of
    // view and axis order from the comparison.
    void SetExplicitView(const Mat4& view, const Mat4& projection);
    void ClearExplicitView() { explicitView_ = false; }
    bool HasExplicitView() const { return explicitView_; }

    // True when the explicit view mirrors the world rather than just rotating
    // it, which decides which winding counts as a back face.
    bool ViewIsMirrored() const { return mirrored_; }

    void  SetFieldOfView(float radians) { fovY_ = Clamp(radians, Radians(10.0f), Radians(150.0f)); }
    float FieldOfView() const { return fovY_; }

    void SetClipPlanes(float nearZ, float farZ) { nearZ_ = nearZ; farZ_ = farZ; }
    float NearPlane() const { return nearZ_; }
    float FarPlane()  const { return farZ_; }

    void  SetSpeed(float unitsPerSecond) { speed_ = std::max(0.01f, unitsPerSecond); }
    float Speed() const { return speed_; }

    float Sensitivity() const { return sensitivity_; }
    void  SetSensitivity(float radiansPerPixel) { sensitivity_ = radiansPerPixel; }

private:
    Vec3  position_{ 0.0f, 0.0f, 50.0f };
    float yaw_   = 0.0f;
    float pitch_ = 0.0f;

    float fovY_  = Radians(60.0f);
    float nearZ_ = 0.5f;
    float farZ_  = 20000.0f;

    bool  explicitView_ = false;
    bool  mirrored_     = false;
    Mat4  explicitViewMatrix_;
    Mat4  explicitProjection_;

    float speed_       = 30.0f;
    float sensitivity_ = 0.0035f;
};
