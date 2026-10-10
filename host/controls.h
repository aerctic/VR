// "keyboard_mouse" control scheme: turns the VR controller state (as a
// gamepad) into keyboard keys, mouse buttons and mouse look for games that
// have no (good) controller support.
//
// Default layout (override in the profile's [bindings] section, e.g.
// `a = space`, `rt = mouse_left`, `move_forward = up`):
//   left stick      W A S D           right stick  mouse look / snap turn
//   A space   B c   X r   Y e         LB q   RB f
//   LT right mouse  RT left mouse     L3 left shift   R3 v
//   Start esc       Back tab          dpad 1 2 3 4
#pragma once

#include <map>
#include <set>
#include <string>

#include "uinput.h"

namespace vrl {

// Linux input code (KEY_* / BTN_LEFT...) for a key name such as "space",
// "e", "f5", "left_shift", "mouse_left"; -1 if unknown.
int key_code(const std::string& name);
inline bool is_mouse_button(int code) { return code >= 0x110 && code <= 0x117; }

class KeyboardMouseMapper {
public:
    KeyboardMouseMapper();

    // button: a b x y lb rb lt rt back start lstick rstick dpad_up dpad_down
    // dpad_left dpad_right move_forward move_back move_left move_right.
    bool bind(const std::string& button, const std::string& key, std::string* error);

    struct Output {
        std::set<int> pressed;     // KEY_* and BTN_* codes currently held
        float look_yaw_deg = 0;    // turn right (+) this frame
        float look_pitch_deg = 0;  // look up (+) this frame
    };
    // snap_deg > 0: right stick X flicks turn by snap_deg; 0: smooth turning at
    // turn_speed deg/s.
    Output update(const GamepadState& s, float dt, float turn_speed, float snap_deg);

private:
    std::map<std::string, int> bindings_;
    bool snap_armed_ = true;
};

}  // namespace vrl
