// User-tunable settings. Loaded from the INI profiles and changeable at
// runtime through vrlayer-ctl ("set stereo.separation 0.02").
#pragma once

#include <string>

#include "config.h"

namespace vrl {

enum class ViewMode { Theater, Immersive };

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

    // [input]
    bool head_mouse = true;  // immersive mode only
    float mouse_counts_per_degree = 30.0f;
    bool mouse_invert_y = false;
    bool head_mouse_pitch = true;
    bool gamepad = true;

    static Settings from_config(const Config& cfg);

    // "section.key" -> value. Returns false (and *error) on unknown key/value.
    bool set(const std::string& key, const std::string& value, std::string* error);
    std::string get(const std::string& key) const;
    // All settings as INI text.
    std::string dump() const;
};

}  // namespace vrl
