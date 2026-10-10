#include "camera_math.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace vrl {

static constexpr float kRadToDeg = 57.29577951308232f;

Mat4 mat_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(rr, k) * b.at(k, c);
            r.at(rr, c) = s;
        }
    return r;
}

Mat4 load_matrix(const float* p, MatOrder order) {
    Mat4 r;
    if (order == MatOrder::ColMajor) {
        memcpy(r.m, p, sizeof(r.m));
    } else {
        for (int rr = 0; rr < 4; ++rr)
            for (int c = 0; c < 4; ++c) r.at(rr, c) = p[rr * 4 + c];
    }
    return r;
}

void store_matrix(float* p, const Mat4& m, MatOrder order) {
    if (order == MatOrder::ColMajor) {
        memcpy(p, m.m, sizeof(m.m));
    } else {
        for (int rr = 0; rr < 4; ++rr)
            for (int c = 0; c < 4; ++c) p[rr * 4 + c] = m.at(rr, c);
    }
}

Mat4 rigid_inverse(const Mat4& m) {
    // [R t]^-1 = [R^T  -R^T t] for orthonormal R (also holds for reflections).
    Mat4 r = mat_identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.at(i, j) = m.at(j, i);
    for (int i = 0; i < 3; ++i) {
        float s = 0;
        for (int k = 0; k < 3; ++k) s += r.at(i, k) * m.at(k, 3);
        r.at(i, 3) = -s;
    }
    return r;
}

bool mat_near(const Mat4& a, const Mat4& b, float rel_tol) {
    float scale = 0;
    for (int i = 0; i < 16; ++i) scale = std::fmax(scale, std::fabs(b.m[i]));
    float tol = rel_tol * (scale > 1.0f ? scale : 1.0f);
    for (int i = 0; i < 16; ++i)
        if (!(std::fabs(a.m[i] - b.m[i]) <= tol)) return false;
    return true;
}

float rotation_angle_deg(const Mat4& a, const Mat4& b) {
    // trace(A^T B) over the 3x3 parts = 1 + 2 cos(theta)
    float tr = 0;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) tr += a.at(k, i) * b.at(k, i);
    float c = (tr - 1.0f) * 0.5f;
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * kRadToDeg;
}

static bool exactly_zero(float v) { return v == 0.0f; }  // also accepts -0.0
static bool unit(float v) { return v == 1.0f || v == -1.0f; }

bool maybe_projection(const float* p) {
    // Column-major math layout: w row term at [11], [15] == 0.
    // Row-major: w at [14], [15] == 0.
    return exactly_zero(p[15]) && (unit(p[11]) || unit(p[14])) && exactly_zero(p[1]) && exactly_zero(p[4]);
}

static bool finite_all(const float* p) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

bool detect_projection(const float* p, ProjectionInfo* out) {
    if (!maybe_projection(p) || !finite_all(p)) return false;
    for (MatOrder order : {MatOrder::ColMajor, MatOrder::RowMajor}) {
        Mat4 m = load_matrix(p, order);
        // Expected form:
        //   [sx  0  cx  0]
        //   [ 0 sy  cy  0]
        //   [ 0  0   A  B]
        //   [ 0  0   w  0]
        const float zeros[] = {m.at(0, 1), m.at(0, 3), m.at(1, 0), m.at(1, 3), m.at(2, 0),
                               m.at(2, 1), m.at(3, 0), m.at(3, 1), m.at(3, 3)};
        bool ok = true;
        for (float z : zeros) ok = ok && exactly_zero(z);
        float w = m.at(3, 2);
        if (!ok || !unit(w)) continue;
        ProjectionInfo info;
        info.order = order;
        info.sx = m.at(0, 0);
        info.sy = m.at(1, 1);
        info.cx = m.at(0, 2);
        info.cy = m.at(1, 2);
        info.fwd = w;
        float A = m.at(2, 2), B = m.at(2, 3);
        if (!(info.sx > 0.05f && info.sx < 20.0f) || !(std::fabs(info.sy) > 0.05f && std::fabs(info.sy) < 20.0f))
            continue;
        if (std::fabs(info.cx) > 0.25f || std::fabs(info.cy) > 0.25f || B == 0.0f) continue;

        // View distance d = w * z_view > 0. clip.z = A*z + B = (A*w)*d + B,
        // clip.w = d, so depth(d) = A*w + B/d.
        float a = A * w, b = B;
        float near_d, far_d;
        bool reversed = b > 0;  // depth decreases with distance
        if (!reversed) {
            // Vulkan clip depth is [0, 1] (DXVK, VKD3D-Proton and native Vulkan
            // games all use it), so depth(near) = 0 and depth(far) = 1.
            near_d = -b / a;
            far_d = std::fabs(1.0f - a) < 1e-7f ? 0.0f : b / (1.0f - a);
        } else {
            near_d = b / (1.0f - a);             // depth 1
            far_d = a == 0.0f ? 0.0f : -b / a;   // depth 0
        }
        if (!(near_d > 0.0f) || !std::isfinite(near_d)) continue;
        if (far_d != 0.0f && !(far_d > near_d)) continue;

        info.reversed = reversed;
        info.near_d = near_d;
        info.far_d = far_d;
        info.depth_a = a;
        info.depth_b = b;
        info.hfov_deg = 2.0f * std::atan(1.0f / info.sx) * kRadToDeg;
        info.vfov_deg = 2.0f * std::atan(1.0f / std::fabs(info.sy)) * kRadToDeg;
        info.aspect = std::fabs(info.sy) / info.sx;
        if (info.hfov_deg < 15.0f || info.hfov_deg > 170.0f) continue;
        *out = info;
        return true;
    }
    return false;
}

bool detect_rigid(const float* p, MatOrder order, Mat4* out) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(p[i])) return false;
    Mat4 m = load_matrix(p, order);
    if (!exactly_zero(m.at(3, 0)) || !exactly_zero(m.at(3, 1)) || !exactly_zero(m.at(3, 2)) || m.at(3, 3) != 1.0f)
        return false;
    // Orthonormal columns.
    for (int i = 0; i < 3; ++i)
        for (int j = i; j < 3; ++j) {
            float d = 0;
            for (int k = 0; k < 3; ++k) d += m.at(k, i) * m.at(k, j);
            if (std::fabs(d - (i == j ? 1.0f : 0.0f)) > 2e-3f) return false;
        }
    bool identity = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::fabs(m.at(i, j) - (i == j ? 1.0f : 0.0f)) > 1e-6f) identity = false;
    if (identity) return false;
    *out = m;
    return true;
}

CameraBasis camera_basis(const ProjectionInfo& p, bool viewport_flipped) {
    CameraBasis b;
    b.fwd = p.fwd;
    // ndc.y = sy * y / d. Without a viewport flip, Vulkan ndc +y is the bottom
    // of the screen; a negative-height viewport makes +y the top.
    bool ndc_up_is_positive = viewport_flipped;
    bool view_up_maps_to_positive_ndc = p.sy > 0;
    b.up = (ndc_up_is_positive == view_up_maps_to_positive_ndc) ? 1.0f : -1.0f;
    return b;
}

Mat4 camera_delta(const CameraBasis& b, const float q[4], const float t[3], float units_per_metre) {
    // Rotation matrix of q in the head frame.
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0) {
        x /= n;
        y /= n;
        z /= n;
        w /= n;
    }
    Mat4 r = mat_identity();
    r.at(0, 0) = 1 - 2 * (y * y + z * z);
    r.at(0, 1) = 2 * (x * y - z * w);
    r.at(0, 2) = 2 * (x * z + y * w);
    r.at(1, 0) = 2 * (x * y + z * w);
    r.at(1, 1) = 1 - 2 * (x * x + z * z);
    r.at(1, 2) = 2 * (y * z - x * w);
    r.at(2, 0) = 2 * (x * z - y * w);
    r.at(2, 1) = 2 * (y * z + x * w);
    r.at(2, 2) = 1 - 2 * (x * x + y * y);
    // Change of basis S = diag(1, up, -fwd) (its own inverse): D = S R S.
    const float s[3] = {1.0f, b.up, -b.fwd};
    Mat4 d = mat_identity();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) d.at(i, j) = s[i] * r.at(i, j) * s[j];
        d.at(i, 3) = s[i] * t[i] * units_per_metre;
    }
    return d;
}

}  // namespace vrl
