// User-tunable settings. Loaded from the INI profiles and changeable at
// runtime through vrlayer-ctl ("set stereo.separation 0.02").
#pragma once

#include <map>
#include <set>
#include <string>

#include "config.h"

namespace vrl {

enum class ViewMode {
    Theater,    // virtual screen (works with every game)
    Immersive,  // game fills the view at its FOV, head turns the camera via the mouse
    Vr,         // head pose written into the game camera (6DOF), shown as a projection layer
};

struct Settings {
    // [display]
    ViewMode mode = ViewMode::Theater;
    float screen_distance = 3.0f;       // metres (theater)
    float screen_width = 4.0f;          // metres (theater)
    float screen_height = 0.0f;         // vertical offset of the screen centre from eye level
    bool curved = true;                 // use a cylinder layer when the runtime supports it
    float game_fov = 90.0f;             // horizontal FOV configured in the game (immersive)
    float immersive_distance = 2.0f;    // metres; only affects stereo convergence feel
    float immersive_latency_ms = 0.0f;  // age of the game camera vs. head pose when a frame arrives
    bool environment = true;            // VR room around the theater screen

    // [stereo]
    bool stereo = true;
    float separation = 0.015f;
    float convergence = 0.03f;
    float popout = 0.5f;
    std::string depth_reversed = "auto";  // auto | true | false
    float depth_far_plane = 1000.0f;
    bool depth_flip_y = false;
    int search_steps = 24;  // stereo quality vs. cost
    bool show_depth = false;
    // With a detected projection, depth is linearized exactly and the screen
    // plane sits this many near-plane distances away.
    float convergence_near = 30.0f;

    // [vr] (display.mode = vr)
    std::string vr_camera = "head";  // head (mono + stereo from depth) | alternate (true stereo, half rate per eye)
    float units_per_metre = 1.0f;    // game world scale (Unreal: 100)
    float ipd = 0.064f;              // metres, for vr.camera = alternate
    std::string aim = "auto";        // auto | head (mouse follows head) | body (head only moves the view)
    float snap_turn = 30.0f;         // degrees per right-stick flick (keyboard_mouse scheme), 0 = smooth
    float turn_speed = 120.0f;       // degrees per second for smooth turning
    // The game's camera does not follow the mouse or stick (table / puzzle
    // games): trust it without the gameplay check. Saved by the loading room
    // when you press START on such a camera.
    bool fixed_camera = false;
    bool show_room = false;          // VR room around the game's view in VR mode (else black)

    // [loading]
    bool loading = true;              // VR loading screen with game analysis
    bool auto_continue = true;        // start automatically when analysis is done
    float loading_timeout = 120.0f;   // seconds before continuing with whatever was found
    bool calibrate_mouse = true;      // measure mouse counts per degree automatically

    // [input]
    bool head_mouse = true;  // immersive mode only
    float mouse_counts_per_degree = 30.0f;
    bool mouse_invert_y = false;
    bool head_mouse_pitch = true;
    bool gamepad = true;
    std::string scheme = "gamepad";  // gamepad | keyboard_mouse
    bool pointer = true;             // point at the screen to move the mouse, trigger clicks
    bool hand_tracking = true;       // pinch gestures and hand pointer when hands are tracked
    // VR mode: the mouse cursor follows your hand, projected through the game
    // camera, so a mouse-controlled in-game hand / tool is where your hand is.
    std::string hand_mouse = "off";        // off | absolute (cursor position) | relative (mouse motion)
    std::string hand_mouse_hand = "right";  // right | left
    float hand_mouse_offset = 0.12f;       // metres from the controller to the tool tip
    float hand_mouse_counts_per_pixel = 1.0f;  // relative mode: mouse counts per screen pixel

    // [bindings] for the keyboard_mouse scheme: button -> key name.
    std::map<std::string, std::string> bindings;

    // Keys that were set by a profile file (as opposed to built-in defaults).
    std::set<std::string> explicit_keys;
    bool is_explicit(const std::string& key) const { return explicit_keys.count(key) != 0; }

    static Settings from_config(const Config& cfg);

    // "section.key" -> value. Returns false (and *error) on unknown key/value.
    bool set(const std::string& key, const std::string& value, std::string* error);
    std::string get(const std::string& key) const;
    // All settings as INI text.
    std::string dump() const;
};

}  // namespace vrl
