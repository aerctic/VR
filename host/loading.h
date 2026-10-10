// The VR loading screen: what has been learned about the game so far, what
// mode it will start in, and the panel that shows it.
#pragma once

#include <string>
#include <vector>

#include "calibrator.h"
#include "canvas.h"
#include "protocol.h"
#include "settings.h"

namespace vrl {

struct LoadingInputs {
    double elapsed = 0;  // seconds since the loading screen started
    bool game = false;
    std::string game_name;
    uint32_t width = 0, height = 0;
    float fps = 0;
    bool gpu_transport = true;
    bool depth = false;
    bool has_analysis = false;
    AnalysisMsg analysis{};
    MouseCalibrator::State calibration = MouseCalibrator::State::Idle;
    std::string calibration_message;
    bool calibration_from_profile = false;  // mouse counts per degree come from a profile
    bool gameplay_confirmed = false;        // the current camera follows player input
    bool probe_possible = false;            // we can send input to check that (uinput + enabled)
    bool uinput = false;           // virtual mouse / gamepad possible
    bool hand_tracking = false;    // runtime offers XR_EXT_hand_tracking
    bool hands_visible = false;
    bool controllers = false;      // a controller is tracked
};

struct LoadingView {
    enum class Check { Pending, Active, Ok, Warn, Fail };
    struct Item {
        std::string label;
        std::string detail;
        Check state = Check::Pending;
    };
    std::string title;
    std::string subtitle;
    std::vector<Item> items;
    float progress = 0;          // overall, 0..1
    ViewMode recommended = ViewMode::Theater;  // what START does (you vouch for being in gameplay)
    ViewMode auto_mode = ViewMode::Theater;    // what the countdown starts
    std::string recommended_text;
    std::string hint;
    bool ready = false;          // enough is known to start
    double countdown = -1;       // seconds until auto start, -1 = none
};

// `ready_since`: elapsed time when `ready` first became true (or < 0).
LoadingView build_loading_view(const LoadingInputs& in, const Settings& s, double ready_since);

const char* mode_name(ViewMode m);

// Panel drawing. Buttons: 0 = start in the recommended mode, 1 = theater,
// 2 = rescan (forget the camera and search again).
constexpr int kButtonStart = 0, kButtonTheater = 1, kButtonRescan = 2;
constexpr int kPanelWidth = 1024, kPanelHeight = 704;
void draw_loading_panel(Canvas& c, const LoadingView& v, int hovered_button, int pressed_button);
// Button under panel coordinates (u, v in 0..1, v down), or -1.
int loading_button_at(float u, float v);

}  // namespace vrl
