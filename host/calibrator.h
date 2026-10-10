// Automatic mouse-look calibration.
//
// Sends a known horizontal mouse movement and watches the game camera (from
// the layer's camera tracking) to measure how many mouse counts turn the
// camera by one degree. Then moves back by the same amount and checks that
// the turn was symmetric (mouse acceleration or smoothing make it fail).
#pragma once

#include <string>

namespace vrl {

class MouseCalibrator {
public:
    enum class State { Idle, WaitStill, Turning, Returning, Done, Failed };

    // counts: size of the test movement.
    void start(double now, int counts = 400);
    void cancel() { state_ = State::Idle; }

    // Feed every game frame (camera->world rotation, row-major 3x3, as sent
    // by the layer). Returns the mouse counts (x) to emit now, usually 0.
    int update(double now, bool camera_valid, const float rot[9]);

    State state() const { return state_; }
    bool busy() const { return state_ == State::WaitStill || state_ == State::Turning || state_ == State::Returning; }
    float counts_per_degree() const { return cpd_; }
    const std::string& message() const { return message_; }

private:
    void fail(const std::string& why);
    bool stable(double now, const float rot[9]);

    State state_ = State::Idle;
    int counts_ = 400;
    int attempts_ = 0;
    double phase_start_ = 0, still_since_ = 0;
    bool have_prev_ = false;
    float prev_[9]{}, ref_[9]{}, turned_[9]{};
    float first_angle_ = 0;
    float cpd_ = 0;
    std::string message_;
};

// Angle in degrees between two row-major 3x3 rotations.
float rotation_delta_deg(const float a[9], const float b[9]);

}  // namespace vrl
