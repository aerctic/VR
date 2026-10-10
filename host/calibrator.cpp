#include "calibrator.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace vrl {

namespace {
constexpr float kStillDeg = 0.05f;      // per-frame change considered "not moving"
constexpr double kStillTime = 0.35;     // seconds the camera must be still
constexpr double kPhaseTimeout = 3.0;   // seconds to wait for a reaction
constexpr float kMinTurnDeg = 0.5f;     // smaller turns mean "no mouse look"
constexpr int kMaxAttempts = 3;
}  // namespace

float rotation_delta_deg(const float a[9], const float b[9]) {
    float tr = 0;
    for (int i = 0; i < 9; ++i) tr += a[i] * b[i];  // trace(A^T B)
    float c = (tr - 1.0f) * 0.5f;
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return std::acos(c) * 57.29577951f;
}

void MouseCalibrator::start(double now, int counts) {
    state_ = State::WaitStill;
    counts_ = counts;
    attempts_ = 0;
    phase_start_ = now;
    still_since_ = now;
    have_prev_ = false;
    cpd_ = 0;
    message_ = "hold still: measuring mouse look";
}

void MouseCalibrator::fail(const std::string& why) {
    if (++attempts_ < kMaxAttempts && state_ != State::Idle) {
        message_ = why + " - retrying";
        state_ = State::WaitStill;
        have_prev_ = false;
        return;
    }
    state_ = State::Failed;
    message_ = why;
}

bool MouseCalibrator::stable(double now, const float rot[9]) {
    if (!have_prev_ || rotation_delta_deg(prev_, rot) > kStillDeg) still_since_ = now;
    memcpy(prev_, rot, sizeof(prev_));
    have_prev_ = true;
    return now - still_since_ >= kStillTime;
}

int MouseCalibrator::update(double now, bool valid, const float rot[9]) {
    if (!busy()) return 0;
    if (!valid) {
        if (now - phase_start_ > kPhaseTimeout * 3) {
            state_ = State::Failed;
            message_ = "game camera not visible (get into gameplay)";
        }
        return 0;
    }
    const bool still = stable(now, rot);
    switch (state_) {
        case State::WaitStill:
            if (still) {
                memcpy(ref_, rot, sizeof(ref_));
                state_ = State::Turning;
                phase_start_ = now;
                still_since_ = now;
                return counts_;
            }
            if (now - phase_start_ > kPhaseTimeout * 3) {
                state_ = State::Failed;
                message_ = "camera never stopped moving (stand still in game)";
            }
            return 0;
        case State::Turning: {
            float a = rotation_delta_deg(ref_, rot);
            if (still && a >= kMinTurnDeg) {
                first_angle_ = a;
                memcpy(turned_, rot, sizeof(turned_));
                state_ = State::Returning;
                phase_start_ = now;
                still_since_ = now;
                return -counts_;
            }
            if (now - phase_start_ > kPhaseTimeout) {
                phase_start_ = now;
                fail(a < kMinTurnDeg ? "camera did not follow the mouse (not a mouse-look view?)"
                                     : "camera kept moving after the mouse turn");
                return -counts_;  // undo the test movement
            }
            return 0;
        }
        case State::Returning: {
            float back = rotation_delta_deg(turned_, rot);
            if (still && back >= kMinTurnDeg) {
                float asym = std::fabs(back - first_angle_) / first_angle_;
                if (asym > 0.2f) {
                    phase_start_ = now;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "turns not symmetric (%.1f vs %.1f deg): disable mouse acceleration",
                             first_angle_, back);
                    fail(buf);
                    return 0;
                }
                cpd_ = 2.0f * float(counts_) / (first_angle_ + back);
                state_ = State::Done;
                char buf[96];
                snprintf(buf, sizeof(buf), "%.2f mouse counts per degree", cpd_);
                message_ = buf;
                return 0;
            }
            if (now - phase_start_ > kPhaseTimeout) {
                phase_start_ = now;
                fail("camera did not turn back");
            }
            return 0;
        }
        default: return 0;
    }
}

}  // namespace vrl
