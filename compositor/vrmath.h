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

// Where a point (local space) appears on the game's screen, for a game camera
// at (cam_pos, cam_rot) with the given full FOVs. u, v in 0..1 (v down).
// False if the point is behind the camera.
bool project_to_screen(Vec3 point, Vec3 cam_pos, Quat cam_rot, float hfov_rad, float vfov_rad, float* u, float* v);

// Pitch (radians, up positive) of a game camera given its camera->world
// rotation (row-major 3x3) and camera-space axes (up sign, forward sign).
// The world's up axis is taken as the world axis closest to the camera's up,
// which works for Y-up and Z-up engines as long as the camera is not rolled
// or pitched beyond 45 degrees past vertical.
float camera_pitch(const float rot[9], float basis_up, float basis_fwd);

// Relative mouse motion that walks an estimated cursor to a target pixel.
struct RelativeCursor {
    float x = -1, y = -1;  // estimated cursor position in pixels (-1: unknown)
    // Returns whole mouse counts to send; keeps the remainder.
    void step(float target_x, float target_y, float counts_per_pixel, int* dx, int* dy);
    void reset(float px, float py) { x = px, y = py; }
};

}  // namespace vrl
