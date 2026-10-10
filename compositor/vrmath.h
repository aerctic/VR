// Minimal vector / quaternion math (OpenXR conventions: +Y up, -Z forward).
#pragma once

namespace vrl {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

constexpr float kPi = 3.14159265358979323846f;
inline float deg(float rad) { return rad * 180.0f / kPi; }
inline float rad(float deg) { return deg * kPi / 180.0f; }

Vec3 add(Vec3 a, Vec3 b);
Vec3 sub(Vec3 a, Vec3 b);
float dot(Vec3 a, Vec3 b);
Quat conjugate(Quat q);  // inverse of a unit quaternion
Vec3 scale(Vec3 v, float s);
Quat mul(Quat a, Quat b);
Quat normalize(Quat q);
Vec3 rotate(Quat q, Vec3 v);
Quat from_axis_angle(Vec3 axis, float angle_rad);

// Orientation looking along yaw (left positive) and pitch (up positive), no roll.
Quat from_yaw_pitch(float yaw_rad, float pitch_rad);
// Yaw/pitch of the forward (-Z) direction of q. Roll is ignored.
void yaw_pitch(Quat q, float* yaw_rad, float* pitch_rad);

}  // namespace vrl
