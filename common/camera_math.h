// Recognising a game's camera matrices in raw GPU constant data, and the
// math to re-aim that camera with the VR head pose.
//
// Mat4 is a math matrix acting on column vectors (clip = P * V * world),
// stored column-major in Mat4::m. Game data can be laid out either way in
// memory (HLSL row-vector conventions end up as the transpose); MatOrder
// says how to read it.
#pragma once

#include <cstdint>

namespace vrl {

struct Mat4 {
    float m[16];  // m[col * 4 + row]
    float at(int r, int c) const { return m[c * 4 + r]; }
    float& at(int r, int c) { return m[c * 4 + r]; }
};

enum class MatOrder : uint32_t { ColMajor = 0, RowMajor = 1 };

Mat4 mat_identity();
Mat4 mat_mul(const Mat4& a, const Mat4& b);
Mat4 load_matrix(const float* p, MatOrder order);
void store_matrix(float* p, const Mat4& m, MatOrder order);
// Inverse of a rotation+translation matrix (rotation part may be a reflection).
Mat4 rigid_inverse(const Mat4& m);
bool mat_near(const Mat4& a, const Mat4& b, float rel_tol);
// Angle in degrees of the rotation taking a's rotation part to b's.
float rotation_angle_deg(const Mat4& a, const Mat4& b);

// A perspective projection recovered from 16 floats.
struct ProjectionInfo {
    MatOrder order = MatOrder::ColMajor;
    float sx = 0, sy = 0;      // focal scales (sy < 0 for Vulkan-style y flip)
    float cx = 0, cy = 0;      // off-centre / TAA jitter terms
    float fwd = -1;            // camera looks along (0,0,fwd) in view space
    float hfov_deg = 0, vfov_deg = 0, aspect = 0;
    float near_d = 0, far_d = 0;  // far_d == 0: infinite far plane
    bool reversed = false;        // reversed-Z (near = 1)
    // Depth buffer value as a function of view distance d:
    //   depth = depth_a + depth_b / d     =>     d = depth_b / (depth - depth_a)
    float depth_a = 0, depth_b = 0;
};

// Recognises a perspective projection in either storage order. Rejects
// orthographic, degenerate and absurd-FOV matrices.
bool detect_projection(const float* p, ProjectionInfo* out);
// Cheap pre-filter on the raw bits (exact 0 / +-1 entries) before detect_projection.
bool maybe_projection(const float* p);

// Recognises a rigid transform (rotation + translation, last row 0 0 0 1)
// stored in the given order. Identity rotations are rejected because object
// matrices of static geometry often are identity.
bool detect_rigid(const float* p, MatOrder order, Mat4* out);

// Mapping from the VR head frame (OpenXR: +x right, +y up, -z forward) to the
// game's camera space.
struct CameraBasis {
    float up = 1;    // +1 if game view-space +y is up on screen
    float fwd = -1;  // game camera looks along (0,0,fwd)
};
// up = +1 when view +y ends up at the top of the screen: depends on the sign
// of sy and whether the game flips the viewport (negative height).
CameraBasis camera_basis(const ProjectionInfo& p, bool viewport_flipped);

// Head pose delta (rotation quaternion xyzw + translation in metres, both in
// the head's rest frame) -> transform D in game camera space, translation
// scaled to game units.
Mat4 camera_delta(const CameraBasis& b, const float q[4], const float t[3], float units_per_metre);

// Re-aims a world->camera view matrix by D (expressed in camera space):
// the camera moves to C*D, so the view becomes D^-1 * V.
inline Mat4 inject_view(const Mat4& view, const Mat4& d) { return mat_mul(rigid_inverse(d), view); }
// Same for a camera->world matrix.
inline Mat4 inject_camera(const Mat4& cam, const Mat4& d) { return mat_mul(cam, d); }

}  // namespace vrl
