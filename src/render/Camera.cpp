#include "render/Camera.h"

namespace {

// Just under 90 degrees: at exactly 90 the forward vector becomes parallel to
// the world up axis and the view matrix loses a degree of freedom.
constexpr float kMaxPitch = 1.5533f;  // 89 degrees

} // namespace

void Camera::SetYawPitch(float yaw, float pitch) {
    yaw_   = yaw;
    pitch_ = Clamp(pitch, -kMaxPitch, kMaxPitch);
}

Vec3 Camera::Forward() const {
    const float cosPitch = std::cos(pitch_);
    return { std::cos(yaw_) * cosPitch,
             std::sin(yaw_) * cosPitch,
             std::sin(pitch_) };
}

Vec3 Camera::Right() const {
    // Perpendicular to forward in the ground plane, so strafing never drifts
    // vertically even when looking steeply up or down.
    return { std::sin(yaw_), -std::cos(yaw_), 0.0f };
}

Vec3 Camera::Up() const {
    return Cross(Right(), Forward());
}

void Camera::Rotate(float deltaX, float deltaY) {
    yaw_  -= deltaX * sensitivity_;
    pitch_ = Clamp(pitch_ - deltaY * sensitivity_, -kMaxPitch, kMaxPitch);

    // Keep yaw in a sane range so it does not lose precision over a long session.
    if (yaw_ >  kPi) yaw_ -= 2.0f * kPi;
    if (yaw_ < -kPi) yaw_ += 2.0f * kPi;
}

void Camera::Move(const Vec3& local) {
    position_ += Right()   * local.x;
    position_ += Forward() * local.y;
    position_ += Vec3{ 0.0f, 0.0f, 1.0f } * local.z;  // rise along world up
}

void Camera::LookAt(const Vec3& eye, const Vec3& target) {
    position_ = eye;

    const Vec3 direction = target - eye;
    const float ground = std::sqrt(direction.x * direction.x + direction.y * direction.y);

    yaw_   = std::atan2(direction.y, direction.x);
    pitch_ = Clamp(std::atan2(direction.z, ground), -kMaxPitch, kMaxPitch);
}

void Camera::FrameBounds(const Aabb& bounds) {
    if (!bounds.Valid()) return;

    const Vec3 centre = bounds.Center();
    const float radius = std::max(bounds.Radius(), 1.0f);

    // Back off far enough that the bounding sphere fits the vertical field of
    // view, then approach from above and behind so the ground stays readable.
    const float distance = radius / std::tan(fovY_ * 0.5f);
    const Vec3 offset{ -distance * 0.7f, -distance * 0.7f, distance * 0.55f };

    LookAt(centre + offset, centre);

    // Scale the clip planes to the world being viewed. The movement speed is
    // left alone: it is the one the person set, not a property of the view.
    nearZ_ = std::max(0.5f, radius * 0.001f);
    farZ_  = std::max(radius * 8.0f, 2000.0f);
}

Mat4 Camera::ViewMatrix() const {
    if (explicitView_) return explicitViewMatrix_;
    return LookAtRH(position_, position_ + Forward(), Vec3{ 0.0f, 0.0f, 1.0f });
}

Mat4 Camera::ProjectionMatrix(float aspect) const {
    if (explicitView_) return explicitProjection_;
    return PerspectiveRH(fovY_, aspect > 0.0f ? aspect : 1.0f, nearZ_, farZ_);
}

void Camera::SetExplicitView(const Mat4& view, const Mat4& projection) {
    explicitViewMatrix_ = view;
    explicitProjection_ = projection;
    explicitView_ = true;

    // A negative determinant means the transform turns the world inside out,
    // so front and back faces swap over.
    const float determinant =
        view.m[0][0] * (view.m[1][1] * view.m[2][2] - view.m[1][2] * view.m[2][1]) -
        view.m[0][1] * (view.m[1][0] * view.m[2][2] - view.m[1][2] * view.m[2][0]) +
        view.m[0][2] * (view.m[1][0] * view.m[2][1] - view.m[1][1] * view.m[2][0]);
    mirrored_ = determinant < 0.0f;

    // Keep the fly camera roughly in step, so leaving the link does not jump.
    position_ = { -(view.m[3][0] * view.m[0][0] + view.m[3][1] * view.m[1][0] +
                    view.m[3][2] * view.m[2][0]),
                  -(view.m[3][0] * view.m[0][1] + view.m[3][1] * view.m[1][1] +
                    view.m[3][2] * view.m[2][1]),
                  -(view.m[3][0] * view.m[0][2] + view.m[3][1] * view.m[1][2] +
                    view.m[3][2] * view.m[2][2]) };
}
