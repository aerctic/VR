// Gameplay probe and automatic mouse-look calibration.
//
// Sends a known horizontal mouse movement and watches the game camera (from
// the layer's camera tracking) to measure how many mouse counts turn the
// camera by one degree. Then moves back by the same amount and checks that
// the turn was symmetric (mouse acceleration or smoothing make it fail).
//
// It also tells gameplay from menus: a main menu's 3D background camera is
// scripted and ignores input, the player's camera follows it. If the mouse
// does nothing, the right stick of the virtual gamepad is tried (controller
// only games). A camera that follows either is the gameplay camera.
#pragma once

#include <string>

namespace vrl {

class MouseCalibrator {
public:
    enum class State {
        Idle,
        WaitStill,
        Turning,         // mouse test movement sent
        Returning,       // and undone
        StickTurning,    // mouse ignored: holding the right stick
        StickReturning,  // turning back the same amount
        Done,            // mouse look calibrated
        DoneStick,       // camera follows the controller (no mouse look)
        Failed,
    };

    // counts: size of the test movement.
    void start(double now, int counts = 400);
    void cancel() {
        state_ = State::Idle;
        stick_x_ = 0;
    }

    // Feed every game frame (camera->world rotation, row-major 3x3, as sent
    // by the layer). Returns the mouse counts (x) to emit now, usually 0.
    int update(double now, bool camera_valid, const float rot[9]);

    State state() const { return state_; }
    bool busy() const {
        return state_ == State::WaitStill || state_ == State::Turning || state_ == State::Returning ||
               state_ == State::StickTurning || state_ == State::StickReturning;
    }
    // Right stick X the probe wants on the virtual gamepad right now.
    float stick_x() const { return stick_x_; }
    // The camera answered player input: it is the gameplay camera.
    bool gameplay_confirmed() const {
        return state_ == State::Done || state_ == State::DoneStick || responded_;
    }
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
    float stick_x_ = 0;
    double stick_held_ = 0;
    bool responded_ = false;
    float cpd_ = 0;
    std::string message_;
};

// Angle in degrees between two row-major 3x3 rotations.
float rotation_delta_deg(const float a[9], const float b[9]);

}  // namespace vrl
