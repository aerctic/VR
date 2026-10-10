#include "loading.h"

#include <cmath>
#include <cstdio>

namespace vrl {

namespace {

constexpr double kCountdown = 5.0;

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[160];
    snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

// Button rectangles in panel pixels.
struct Rect {
    int x, y, w, h;
};
const Rect kButtons[3] = {{48, 600, 432, 72}, {504, 600, 224, 72}, {752, 600, 224, 72}};

}  // namespace

const char* mode_name(ViewMode m) {
    switch (m) {
        case ViewMode::Vr: return "VR (6DOF head tracking)";
        case ViewMode::Immersive: return "Immersive (head aims the camera)";
        default: return "Theater (big screen)";
    }
}

LoadingView build_loading_view(const LoadingInputs& in, const Settings& s, double ready_since) {
    using C = LoadingView::Check;
    LoadingView v;
    v.title = "VRLAYER";
    if (!in.game) {
        v.subtitle = "Waiting for a game...";
        v.items.push_back({"Game", "start it with: vrlayer-run %command%", C::Active});
        v.hint = "Steam: Properties > Launch Options > vrlayer-run %command%";
        return v;
    }
    v.subtitle = in.game_name;
    const AnalysisMsg& a = in.analysis;
    const bool proj = in.has_analysis && (a.flags & kAnalysisProjection);
    const bool view = proj && (a.flags & kAnalysisView);

    // 1. Frames.
    v.items.push_back({"Game video", fmt("%.0fx%.0f, %.0f fps", in.width, in.height, in.fps) +
                                         (in.gpu_transport ? ", zero-copy" : ", cpu copy"),
                       in.fps > 0 ? C::Ok : C::Active});

    // 2. Depth buffer.
    v.items.push_back({"Depth buffer", in.depth ? "found: stereo 3D available" : "waiting for 3D rendering",
                       in.depth ? C::Ok : C::Active});

    // 3. Camera projection.
    LoadingView::Item p{"Camera lens", "", C::Active};
    if (proj) {
        p.state = C::Ok;
        p.detail = fmt("%.0f x %.0f deg FOV, near %.3g", a.hfov_deg, a.vfov_deg, a.near_d) +
                   (a.far_d == 0 ? ", infinite far" : fmt(", far %.4g", a.far_d)) + (a.reversed ? ", reversed-Z" : "");
    } else if (in.has_analysis) {
        p.detail = fmt("searching game memory (%.0f MB, %.0f%%)", a.scanned_mb, a.progress);
    } else {
        p.detail = "waiting for analysis";
    }
    v.items.push_back(p);

    // 4. Camera pose.
    LoadingView::Item c{"Camera position", "", C::Pending};
    if (view) {
        c.state = C::Ok;
        c.detail = std::string("found") + ((a.flags & kAnalysisViewProj) ? " (+ view-projection)" : "") +
                   ((a.flags & kAnalysisPushConstants) ? " in push constants" : "") + ": 6DOF possible";
    } else if (proj) {
        c.state = C::Warn;
        c.detail = "not found next to the lens: no 6DOF for this game";
    } else {
        c.detail = "needs the lens first";
    }
    v.items.push_back(c);

    // 5. Gameplay check: the camera must follow player input. A main menu's
    //    3D background (or a cutscene) has a camera too, but it ignores you.
    LoadingView::Item g{"Gameplay", "", C::Pending};
    const bool mouse_ok = in.calibration == MouseCalibrator::State::Done || in.calibration_from_profile;
    if (!in.probe_possible) {
        g.state = C::Warn;
        g.detail = !in.uinput ? "can't check (no /dev/uinput): press START once you are in the game"
                              : "check disabled: press START once you are in the game";
    } else if (in.gameplay_confirmed) {
        g.state = C::Ok;
        if (in.calibration == MouseCalibrator::State::Done) g.detail = "camera follows your mouse: " + in.calibration_message;
        else if (in.calibration_from_profile) g.detail = fmt("camera follows your controls (%.2f counts/deg saved)", s.mouse_counts_per_degree);
        else g.detail = in.calibration_message.empty() ? "camera follows your controls" : in.calibration_message;
    } else if (!view) {
        g.detail = "needs the camera position";
    } else if (in.calibration == MouseCalibrator::State::Failed) {
        g.state = C::Active;
        g.detail = "this camera ignores your controls: main menu or cutscene? (retrying)";
    } else {
        g.state = C::Active;
        g.detail = in.calibration_message.empty() ? "checking that the camera follows your controls"
                                                  : in.calibration_message;
    }
    v.items.push_back(g);

    // 6. Hands & controllers.
    std::string hands;
    if (in.controllers) hands = "controllers tracked";
    if (in.hands_visible) hands += hands.empty() ? "hands tracked" : " + hands";
    if (hands.empty()) hands = in.hand_tracking ? "pick up controllers or show your hands" : "pick up your controllers";
    hands += s.scheme == "keyboard_mouse" ? " -> keyboard + mouse" : " -> Xbox controller";
    v.items.push_back({"Hands & controls", hands, (in.controllers || in.hands_visible) ? C::Ok : C::Active});

    // Recommendation: what START does (you vouch for being in gameplay).
    if (view) {
        v.recommended = ViewMode::Vr;
        v.recommended_text = std::string("VR: your head moves the game camera") + (mouse_ok ? ", aim with your head" : "");
    } else if (proj && mouse_ok) {
        v.recommended = ViewMode::Immersive;
        v.recommended_text = "Immersive: game fills your view, head turns the camera";
    } else {
        v.recommended = ViewMode::Theater;
        v.recommended_text = in.depth ? "Theater: 3D screen" : "Theater: big screen";
    }

    // Progress and readiness. Only a camera that follows the player starts
    // VR / immersive by itself; after the timeout an unconfirmed camera
    // (still the main menu?) gets the theater screen, never VR.
    int done = 0;
    for (auto& it : v.items) done += (it.state == C::Ok || it.state == C::Warn) ? 1 : 0;
    v.progress = float(done) / float(v.items.size());
    const bool timed_out = in.elapsed >= double(s.loading_timeout);
    const bool confirmed = in.gameplay_confirmed && proj;
    v.ready = confirmed || timed_out;
    v.auto_mode = confirmed ? v.recommended : ViewMode::Theater;
    if (!confirmed && timed_out) v.recommended_text += " - auto start: theater (gameplay not confirmed)";

    const bool probing = in.calibration != MouseCalibrator::State::Idle && in.calibration != MouseCalibrator::State::Failed &&
                         !in.gameplay_confirmed;
    if (!proj && !timed_out)
        v.hint = "Load a save / enter gameplay - the 3D camera can only be found while the game renders it.";
    else if (proj && in.probe_possible && !in.gameplay_confirmed && probing)
        v.hint = "Stand still in game and leave the mouse alone for a moment.";
    else if (proj && in.probe_possible && !in.gameplay_confirmed)
        v.hint = "In a menu? Load a save - VRLayer switches by itself. RESCAN if stuck.";
    else if (proj && !in.probe_possible && !in.gameplay_confirmed)
        v.hint = "Press START once you are in the game. RESCAN if it found a menu.";
    else
        v.hint = "START / pinch to begin   THEATER: big screen   RESCAN: search again";

    if (v.ready && s.auto_continue && ready_since >= 0) v.countdown = std::fmax(0.0, kCountdown - (in.elapsed - ready_since));
    return v;
}

int loading_button_at(float u, float v) {
    const float x = u * kPanelWidth, y = v * kPanelHeight;
    for (int i = 0; i < 3; ++i) {
        const Rect& r = kButtons[i];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
    }
    return -1;
}

void draw_loading_panel(Canvas& c, const LoadingView& v, int hovered, int pressed) {
    using C = LoadingView::Check;
    const Rgba bg = 0x0E131CF0, fg = 0xE8EEF7FF, dim = 0x8A97ABFF, accent = 0x5AB4FFFF;
    const Rgba ok = 0x58D68DFF, warn = 0xF2B84BFF, fail = 0xF06A6AFF;
    c.clear(bg);
    c.frame_rect(0, 0, kPanelWidth, kPanelHeight, 3, 0x2A3A52FF);

    c.text(48, 36, v.title, 5, accent);
    c.text_fit(48 + Canvas::text_width(v.title, 5) + 30, 50, v.subtitle, 3, fg, kPanelWidth - 96 - 230);
    c.progress(48, 92, kPanelWidth - 96, 8, v.progress, accent, 0x223047FF);

    int y = 130;
    for (const auto& it : v.items) {
        Rgba col = it.state == C::Ok ? ok : it.state == C::Warn ? warn : it.state == C::Fail ? fail
                 : it.state == C::Active ? accent : dim;
        const char* mark = it.state == C::Ok ? "OK" : it.state == C::Warn ? "!!" : it.state == C::Fail ? "XX"
                         : it.state == C::Active ? ".." : "--";
        c.fill_rect(48, y, 56, 40, (col & 0xFFFFFF00) | 0x30);
        c.text(48 + 28 - Canvas::text_width(mark, 3) / 2, y + 9, mark, 3, col);
        c.text(124, y + 2, it.label, 3, fg);
        c.text_fit(124, y + 28, it.detail, 2, dim, kPanelWidth - 124 - 48);
        y += 66;
    }

    c.text_fit(48, 536, v.recommended_text, 2, fg, kPanelWidth - 96);
    c.text_fit(48, 562, v.hint, 2, dim, kPanelWidth - 96);

    const std::string countdown =
        v.countdown >= 0 ? " (" + std::to_string(int(std::ceil(v.countdown))) + ")" : std::string();
    for (int i = 0; i < 3; ++i) {
        const Rect& r = kButtons[i];
        const bool primary = i == kButtonStart;
        Rgba fill = primary ? (v.ready && v.auto_mode == v.recommended ? 0x2F7FD0FF : 0x2A3A52FF) : 0x223047FF;
        if (hovered == i) fill = primary ? 0x4A9BEBFF : 0x33465FFF;
        if (pressed == i) fill = 0x6FB6FFFF;
        c.fill_rect(r.x, r.y, r.w, r.h, fill);
        std::string label;
        if (primary) {
            label = std::string("START ") +
                    (v.recommended == ViewMode::Vr ? "VR" : v.recommended == ViewMode::Immersive ? "IMMERSIVE" : "THEATER");
            if (v.auto_mode == v.recommended) label += countdown;
        } else if (i == kButtonTheater) {
            label = "THEATER";
            if (v.auto_mode != v.recommended) label += countdown;
        } else {
            label = "RESCAN";
        }
        c.text(r.x + (r.w - Canvas::text_width(label, 3)) / 2, r.y + (r.h - 21) / 2, label, 3, fg);
    }
}

}  // namespace vrl
