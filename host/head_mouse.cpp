#include "head_mouse.h"

#include <cmath>

namespace vrl {

float angle_diff_deg(float a, float b) {
    float d = std::fmod(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d <= -180.0f) d += 360.0f;
    return d;
}

void HeadMouse::reset(float yaw_deg, float pitch_deg) {
    active_ = true;
    last_yaw_ = yaw_deg;
    last_pitch_ = pitch_deg;
    rem_x_ = rem_y_ = 0;
    cam_yaw_ = cam_pitch_ = 0;
}

void HeadMouse::update(float yaw_deg, float pitch_deg, float cpd, bool invert_y, bool use_pitch, int* dx, int* dy) {
    *dx = *dy = 0;
    if (!active_ || cpd <= 0.0f) {
        last_yaw_ = yaw_deg;
        last_pitch_ = pitch_deg;
        return;
    }
    float dyaw = angle_diff_deg(yaw_deg, last_yaw_);
    float dpitch = pitch_deg - last_pitch_;
    last_yaw_ = yaw_deg;
    last_pitch_ = pitch_deg;

    // Turning left (yaw up) is negative mouse X; looking up is negative mouse Y.
    float fx = -dyaw * cpd + rem_x_;
    float fy = use_pitch ? (invert_y ? dpitch : -dpitch) * cpd + rem_y_ : 0.0f;
    *dx = int(std::trunc(fx));
    *dy = int(std::trunc(fy));
    rem_x_ = fx - float(*dx);
    rem_y_ = fy - float(*dy);

    cam_yaw_ -= float(*dx) / cpd;
    if (use_pitch) cam_pitch_ += (invert_y ? float(*dy) : -float(*dy)) / cpd;
}

}  // namespace vrl
