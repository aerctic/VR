#include "vrmath.h"

#include <cmath>

namespace vrl {

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

Quat mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat normalize(Quat q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0.0f) return Quat{};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

Vec3 rotate(Quat q, Vec3 v) {
    // v' = q * v * q^-1, expanded.
    Vec3 u{q.x, q.y, q.z};
    float s = q.w;
    float dot_uv = u.x * v.x + u.y * v.y + u.z * v.z;
    float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return add(add(scale(u, 2.0f * dot_uv), scale(v, s * s - dot_uu)), scale(cross, 2.0f * s));
}

Quat from_axis_angle(Vec3 a, float angle) {
    float h = angle * 0.5f, s = std::sin(h);
    return {a.x * s, a.y * s, a.z * s, std::cos(h)};
}

Quat from_yaw_pitch(float yaw, float pitch) {
    return normalize(mul(from_axis_angle({0, 1, 0}, yaw), from_axis_angle({1, 0, 0}, pitch)));
}

void yaw_pitch(Quat q, float* yaw, float* pitch) {
    Vec3 f = rotate(q, {0, 0, -1});
    *yaw = std::atan2(-f.x, -f.z);
    float y = f.y > 1.0f ? 1.0f : (f.y < -1.0f ? -1.0f : f.y);
    *pitch = std::asin(y);
}

}  // namespace vrl
