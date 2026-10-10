// Converts head rotation into relative mouse motion so that mouse-look games
// turn their camera with the player's head ("immersive" mode).
#pragma once

namespace vrl {

class HeadMouse {
public:
    // Start tracking from this orientation (degrees). The game camera is
    // assumed to be looking where the head looks now.
    void reset(float yaw_deg, float pitch_deg);

    // Feed the current head orientation. Returns the whole mouse counts to
    // emit; fractional counts are carried over to the next call so slow head
    // motion is not lost. yaw grows to the left, pitch grows upwards.
    void update(float yaw_deg, float pitch_deg, float counts_per_degree, bool invert_y, bool use_pitch, int* dx,
                int* dy);

    // Where the game camera should be looking now, relative to reset(), in
    // degrees (accumulated from the counts actually emitted).
    float camera_yaw() const { return cam_yaw_; }
    float camera_pitch() const { return cam_pitch_; }
    bool active() const { return active_; }
    void stop() { active_ = false; }

private:
    bool active_ = false;
    float last_yaw_ = 0, last_pitch_ = 0;
    float rem_x_ = 0, rem_y_ = 0;
    float cam_yaw_ = 0, cam_pitch_ = 0;
};

// Shortest signed angle difference a - b in degrees, in (-180, 180].
float angle_diff_deg(float a, float b);

}  // namespace vrl
