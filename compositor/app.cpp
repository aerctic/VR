#include "app.h"

#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "config.h"
#include "ipc.h"
#include "log.h"

#ifndef VRLAYER_DATADIR
#define VRLAYER_DATADIR "/usr/local/share/vrlayer"
#endif

namespace vrl {

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
static double now_s() { return double(now_ns()) * 1e-9; }
static Quat to_quat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
static Vec3 to_vec(const XrVector3f& v) { return {v.x, v.y, v.z}; }

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

void App::load_profile(uint32_t appid) {
    appid_ = appid;
    Config cfg;
    std::vector<std::string> files;
    const char* env_dir = getenv("VRLAYER_DATADIR");  // e.g. a source checkout
    const std::string data = env_dir && *env_dir ? env_dir : VRLAYER_DATADIR;
    std::vector<std::string> candidates{data + "/profiles/default.ini"};
    if (appid) candidates.push_back(data + "/profiles/games/" + std::to_string(appid) + ".ini");
    for (auto& p : profile_paths(appid)) candidates.push_back(p);
    for (auto& p : candidates) {
        if (access(p.c_str(), R_OK) != 0) continue;
        std::string err;
        if (cfg.load_file(p, &err)) files.push_back(p);
        else VRL_LOG("compositor", "%s", err.c_str());
    }
    settings_ = Settings::from_config(cfg);
    mapper_ = KeyboardMouseMapper();
    for (auto& [button, key] : settings_.bindings) {
        std::string err;
        if (!mapper_.bind(button, key, &err)) VRL_LOG("compositor", "[bindings] %s", err.c_str());
    }
    profile_files_.clear();
    for (auto& f : files) profile_files_ += (profile_files_.empty() ? "" : ", ") + f;
    VRL_LOG("compositor", "profile for appid %u: %s", appid, profile_files_.empty() ? "built-in defaults" : profile_files_.c_str());
    reversed_score_ = 0;
    need_recenter_ = true;
}

// Detection results go to games/<appid>.auto.ini, which loads before the
// user's own games/<appid>.ini (so hand-tuned values always win).
void App::write_auto_profile() {
    const AnalysisMsg* a = analysis();
    if (!appid_ || (!a && calibrator_.state() != MouseCalibrator::State::Done)) return;
    std::string dir = config_dir() + "/games";
    mkdir(config_dir().c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/" + std::to_string(appid_) + ".auto.ini";
    std::ofstream f(path);
    if (!f) return;
    const ClientInfo* c = server_->active();
    f << "; Detected by the VRLayer loading screen" << (c ? " for " + c->exe : std::string()) << ".\n"
      << "; Overwritten on every detection; put your own changes in " << appid_ << ".ini.\n";
    if (a && (a->flags & kAnalysisProjection)) {
        f << "\n[display]\ngame_fov = " << a->hfov_deg << "\n";
        f << "; camera: " << a->hfov_deg << "x" << a->vfov_deg << " deg, near " << a->near_d << ", far "
          << (a->far_d == 0 ? std::string("infinite") : std::to_string(a->far_d)) << (a->reversed ? ", reversed-z" : "")
          << ((a->flags & kAnalysisView) ? ", view matrix found" : "") << "\n";
    }
    if (calibrator_.state() == MouseCalibrator::State::Done)
        f << "\n[input]\nmouse_counts_per_degree = " << calibrator_.counts_per_degree() << "\n";
    VRL_LOG("compositor", "detection results saved to %s", path.c_str());
}

const AnalysisMsg* App::analysis() const {
    const ClientInfo* c = server_ ? server_->active() : nullptr;
    return c && c->has_analysis ? &c->analysis : nullptr;
}

bool App::camera_found() const {
    const AnalysisMsg* a = analysis();
    return a && (a->flags & kAnalysisProjection) && (a->flags & kAnalysisView);
}

uint32_t App::camera_id() const {
    const AnalysisMsg* a = analysis();
    return a && (a->flags & kAnalysisProjection) ? a->camera_id : 0;
}

bool App::probe_possible() const { return uinput_ok_ && settings_.calibrate_mouse; }

bool App::camera_confirmed() const {
    return camera_found() && (confirmed_camera_id_ == camera_id() || !probe_possible());
}

// VR needs the gameplay camera: a main menu's 3D background camera is found
// just as well, but writing the head pose into it is pointless.
bool App::vr_available() const { return camera_confirmed(); }

bool App::cpd_known() const {
    return settings_.is_explicit("input.mouse_counts_per_degree") || cpd_measured_ ||
           calibrator_.state() == MouseCalibrator::State::Done;
}

void App::reset_probe() {
    calibrator_.cancel();
    calibration_started_ = false;
    probe_retry_at_ = -1;
    probed_camera_id_ = 0;
    panel_dirty_ = true;
}

// Runs in both phases. Whenever the tracked camera is not known to follow the
// player, nudge the mouse (then the right stick) and watch it. Failures
// (still in the main menu, a cutscene) are retried every few seconds, and a
// new camera (menu -> level) starts a new check.
void App::update_gameplay_probe(double now) {
    const uint32_t id = camera_id();
    if (id != probed_camera_id_) {
        if (calibrator_.busy() || calibration_started_) VRL_LOG("compositor", "game camera changed, checking it again");
        calibrator_.cancel();
        calibration_started_ = false;
        probe_retry_at_ = -1;
        probed_camera_id_ = id;
        panel_dirty_ = true;
    }
    if (!server_->active() || !camera_found() || !probe_possible()) return;
    if (calibrator_.gameplay_confirmed() && confirmed_camera_id_ != id) {
        confirmed_camera_id_ = id;
        VRL_LOG("compositor", "gameplay confirmed: %s", calibrator_.message().c_str());
        panel_dirty_ = true;
    }
    if (calibrator_.state() == MouseCalibrator::State::Done && !settings_.is_explicit("input.mouse_counts_per_degree")) {
        settings_.mouse_counts_per_degree = calibrator_.counts_per_degree();
        cpd_measured_ = true;
    }
    const bool confirmed = confirmed_camera_id_ == id;
    // In play, a confirmed camera is left alone (no surprise mouse nudges).
    if (confirmed && (phase_ == Phase::Playing || calibration_started_)) return;
    if (calibrator_.state() == MouseCalibrator::State::Failed && !calibrator_.gameplay_confirmed()) {
        if (probe_retry_at_ < 0) probe_retry_at_ = now + 4.0;
        if (now >= probe_retry_at_) calibration_started_ = false;  // try again (did you load a save?)
    }
    if (!calibration_started_ && !calibrator_.busy()) {
        calibrator_.start(now);
        calibration_started_ = true;
        probe_retry_at_ = -1;
        panel_dirty_ = true;
    }
}

bool App::aim_with_head() const {
    if (settings_.aim == "head") return true;
    if (settings_.aim == "body") return false;
    return cpd_known() && uinput_ok_;
}

ViewMode App::effective_mode() const {
    if (settings_.mode == ViewMode::Vr && !vr_available()) return ViewMode::Theater;
    return settings_.mode;
}

// ---------------------------------------------------------------------------
// Status / commands
// ---------------------------------------------------------------------------

std::string App::status() const {
    std::ostringstream o;
    const ClientInfo* c = server_->active();
    o << "runtime: " << xr_.runtime_name() << (xr_.running() ? " (session running)" : " (session idle)") << "\n";
    if (c) {
        o << "game: " << c->exe << " pid " << c->pid << " appid " << c->appid << "\n"
          << "frame: " << c->width << "x" << c->height << " via " << (c->transport == Transport::Gpu ? "gpu" : "cpu")
          << ", depth " << (c->depth ? "yes" : "no") << ", " << int(game_fps_ + 0.5f) << " fps\n";
    } else {
        o << "game: none connected\n";
    }
    for (auto& other : server_->clients())
        if (!c || other.id != c->id) o << "also connected: " << other.exe << " pid " << other.pid << "\n";
    o << "phase: " << (phase_ == Phase::Loading ? "loading" : "playing");
    if (phase_ == Phase::Loading)
        o << " (" << (loading_view_.ready ? "ready" : "analyzing")
          << (loading_view_.countdown >= 0 ? ", starting in " + std::to_string(int(std::ceil(loading_view_.countdown))) + " s" : "")
          << ", recommended " << (loading_view_.recommended == ViewMode::Vr ? "vr" : loading_view_.recommended == ViewMode::Immersive ? "immersive" : "theater")
          << ")";
    o << "\n";
    if (const AnalysisMsg* a = analysis()) {
        o << "analysis: " << a->status << "\n";
        if (a->flags & kAnalysisProjection)
            o << "camera: fov " << a->hfov_deg << "x" << a->vfov_deg << ", near " << a->near_d << ", far "
              << a->far_d << (a->reversed ? ", reversed-z" : "") << ((a->flags & kAnalysisView) ? ", view found" : "")
              << ((a->flags & kAnalysisInjecting) ? ", injecting (" + std::to_string(a->patched) + "/frame)" : "")
              << "\n";
    }
    if (camera_found())
        o << "gameplay: " << (camera_confirmed() ? "confirmed" : "not confirmed (main menu?)") << "\n";
    if (calibrator_.state() != MouseCalibrator::State::Idle) o << "gameplay check: " << calibrator_.message() << "\n";
    const ViewMode m = effective_mode();
    o << "mode: " << (m == ViewMode::Theater ? "theater" : m == ViewMode::Immersive ? "immersive" : "vr")
      << (settings_.mode == ViewMode::Vr && m != ViewMode::Vr ? " (vr unavailable: camera not found)" : "")
      << ", stereo: " << (frame_stereo_ ? "on" : "off");
    if (m == ViewMode::Vr) o << ", camera " << settings_.vr_camera << ", aim " << (aim_with_head() ? "head" : "body");
    o << "\ninput: " << settings_.scheme << (uinput_ok_ ? "" : " (no /dev/uinput access)")
      << ", hands " << (hands_.available() ? (hands_seen_ ? "tracked" : "available") : "unavailable")
      << ", controllers " << (controllers_seen_ ? "tracked" : "not seen") << "\n"
      << "profile: " << (profile_files_.empty() ? "built-in defaults" : profile_files_);
    return o.str();
}

std::string App::handle_command(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, a, b;
    in >> cmd >> a;
    std::getline(in >> std::ws, b);
    if (cmd == "ping") return "pong";
    if (cmd == "status") return status();
    if (cmd == "recenter") {
        need_recenter_ = true;
        return "ok";
    }
    if (cmd == "continue") {
        if (phase_ != Phase::Loading) return "already playing";
        start_playing(a.empty() ? loading_view_.recommended
                                : a == "vr" ? ViewMode::Vr : a == "immersive" ? ViewMode::Immersive : ViewMode::Theater,
                      a != "theater");
        return "playing";
    }
    if (cmd == "loading") {
        enter_loading(a == "reanalyze");
        return "loading screen";
    }
    if (cmd == "analyze" || cmd == "rescan") {
        server_->restart_analysis();
        confirmed_camera_id_ = 0;
        reset_probe();
        return "searching the game camera again";
    }
    if (cmd == "gameplay") {  // "I am in gameplay": trust the current camera
        if (!camera_found()) return "error: no camera found yet";
        confirmed_camera_id_ = camera_id();
        return "camera confirmed as gameplay";
    }
    if (cmd == "calibrate") {
        if (!uinput_ok_) return "error: no access to /dev/uinput";
        if (!camera_found()) return "error: the game camera has not been found yet";
        cpd_measured_ = false;
        calibrator_.start(now_s());
        calibration_started_ = true;
        probed_camera_id_ = camera_id();
        return "calibrating mouse look: stand still in game";
    }
    if (cmd == "mode") {
        if (a == "toggle") {
            ViewMode m = effective_mode();
            a = m == ViewMode::Theater ? "immersive" : (m == ViewMode::Immersive && vr_available()) ? "vr" : "theater";
        }
        std::string err;
        if (!settings_.set("display.mode", a, &err)) return "error: " + err;
        if (settings_.mode == ViewMode::Vr && !vr_available())
            return camera_found() ? "mode vr: waiting for the camera to follow your controls (gameplay check), showing theater"
                                  : "mode vr requested, but the camera was not found: showing theater";
        need_recenter_ = true;
        return "mode " + a;
    }
    if (cmd == "3d" || cmd == "stereo") {
        settings_.stereo = a == "on" ? true : a == "off" ? false : !settings_.stereo;
        return std::string("stereo ") + (settings_.stereo ? "on" : "off");
    }
    if (cmd == "set") {
        std::string err;
        if (!settings_.set(a, b, &err)) return "error: " + err;
        settings_.explicit_keys.insert(a);
        if (a.rfind("display.", 0) == 0) need_recenter_ = true;
        if (a.rfind("bindings.", 0) == 0 && !mapper_.bind(a.substr(9), b, &err)) return "error: " + err;
        return a + " = " + settings_.get(a);
    }
    if (cmd == "get") {
        std::string v = settings_.get(a);
        return v.empty() ? "error: unknown setting '" + a + "'" : a + " = " + v;
    }
    if (cmd == "dump") return settings_.dump();
    if (cmd == "reload") {
        load_profile(appid_);
        return "reloaded: " + (profile_files_.empty() ? std::string("built-in defaults") : profile_files_);
    }
    if (cmd == "save") {
        std::string dir = config_dir() + "/games";
        mkdir(config_dir().c_str(), 0755);
        mkdir(dir.c_str(), 0755);
        std::string path = appid_ ? dir + "/" + std::to_string(appid_) + ".ini" : config_dir() + "/default.ini";
        std::ofstream f(path);
        if (!f) return "error: cannot write " + path;
        const ClientInfo* c = server_->active();
        f << "; VRLayer profile" << (c ? " for " + c->exe : std::string()) << "\n" << settings_.dump();
        return "saved " + path;
    }
    if (cmd == "next") {
        server_->cycle_active();
        return "ok";
    }
    if (cmd == "quit") {
        quit_ = true;
        return "bye";
    }
    return "commands: status | continue [vr|immersive|theater] | loading [reanalyze] | analyze | calibrate | "
           "recenter | mode theater|immersive|vr|toggle | 3d on|off|toggle | set <key> <value> | get <key> | "
           "dump | reload | save | next | quit";
}

// ---------------------------------------------------------------------------
// Phases
// ---------------------------------------------------------------------------

void App::enter_loading(bool restart_analysis) {
    phase_ = Phase::Loading;
    loading_start_ = now_s();
    ready_since_ = -1;
    reset_probe();
    head_mouse_.stop();
    need_recenter_ = true;
    panel_dirty_ = true;
    if (restart_analysis && server_) server_->restart_analysis();
}

void App::start_playing(ViewMode choice, bool vouch) {
    if (vouch && camera_found()) confirmed_camera_id_ = camera_id();
    // Detected values fill in whatever the profiles did not set.
    if (const AnalysisMsg* a = analysis()) {
        if ((a->flags & kAnalysisProjection) && !settings_.is_explicit("display.game_fov")) settings_.game_fov = a->hfov_deg;
    }
    if (calibrator_.state() == MouseCalibrator::State::Done && !settings_.is_explicit("input.mouse_counts_per_degree")) {
        settings_.mouse_counts_per_degree = calibrator_.counts_per_degree();
        cpd_measured_ = true;
    }
    write_auto_profile();
    // An explicit mode in the profile wins over the recommendation.
    if (!settings_.is_explicit("display.mode")) settings_.mode = choice;
    else if (choice == ViewMode::Theater && loading_view_.recommended != ViewMode::Theater) settings_.mode = choice;
    phase_ = Phase::Playing;
    need_recenter_ = true;
    if (calibrator_.busy() && camera_confirmed()) reset_probe();
    const ViewMode m = effective_mode();
    VRL_LOG("compositor", "starting in %s mode", mode_name(m));
}

void App::update_loading(double now) {
    LoadingInputs in;
    in.elapsed = now - loading_start_;
    const ClientInfo* c = server_->active();
    in.game = c != nullptr;
    if (c) {
        in.game_name = c->exe;
        in.width = c->width;
        in.height = c->height;
        in.fps = game_fps_;
        in.gpu_transport = c->transport == Transport::Gpu;
        in.depth = c->depth;
        in.has_analysis = c->has_analysis;
        if (c->has_analysis) in.analysis = c->analysis;
        if (c->has_analysis && (c->analysis.flags & kAnalysisDepth)) in.depth = true;
    }
    in.calibration = calibrator_.state();
    in.calibration_message = calibrator_.message();
    in.calibration_from_profile = settings_.is_explicit("input.mouse_counts_per_degree");
    in.gameplay_confirmed = camera_found() && confirmed_camera_id_ == camera_id();
    in.probe_possible = probe_possible();
    in.uinput = uinput_ok_;
    in.hand_tracking = hands_.available();
    in.hands_visible = hands_.hand(0).valid || hands_.hand(1).valid;
    in.controllers = controllers_seen_;


    LoadingView v = build_loading_view(in, settings_, ready_since_);
    if (v.ready && ready_since_ < 0) {
        ready_since_ = in.elapsed;
        v = build_loading_view(in, settings_, ready_since_);
    }
    if (!v.ready) ready_since_ = -1;
    loading_view_ = v;
    // Redraw the panel a few times per second (and immediately on input changes).
    if (panel_drawn_at_ < 0 || now - panel_drawn_at_ > 0.2) panel_dirty_ = true;

    if (c && v.ready && settings_.auto_continue && v.countdown == 0.0) start_playing(v.auto_mode);
}

void App::recenter(const XrPosef& head) {
    float yaw, pitch;
    yaw_pitch(to_quat(head.orientation), &yaw, &pitch);
    rest_rot_ = from_yaw_pitch(yaw, 0);
    rest_pos_ = to_vec(head.position);
    base_yaw_ = deg(yaw);
    base_pitch_ = deg(pitch);
    head_mouse_.reset(deg(yaw), deg(pitch));
    cam_history_.clear();
    immersive_rot_ = from_yaw_pitch(yaw, pitch);
    need_recenter_ = false;
    panel_dirty_ = true;
    VRL_DBG("compositor", "recentered (yaw %.1f)", deg(yaw));
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static bool ray_quad(Vec3 o, Vec3 d, Vec3 c, Quat q, float w, float h, float* t, float* u, float* v) {
    Vec3 n = rotate(q, {0, 0, 1});
    float dn = dot(d, n);
    if (std::fabs(dn) < 1e-5f) return false;
    *t = dot(sub(c, o), n) / dn;
    if (*t <= 0) return false;
    Vec3 local = rotate(conjugate(q), sub(add(o, scale(d, *t)), c));
    *u = local.x / w + 0.5f;
    *v = 0.5f - local.y / h;
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

static bool ray_cylinder(Vec3 o, Vec3 d, Vec3 c, Quat q, float radius, float angle, float height, float* t, float* u,
                         float* v) {
    Quat inv = conjugate(q);
    Vec3 lo = rotate(inv, sub(o, c)), ld = rotate(inv, d);
    float a = ld.x * ld.x + ld.z * ld.z;
    if (a < 1e-8f) return false;
    float b = 2 * (lo.x * ld.x + lo.z * ld.z), cc = lo.x * lo.x + lo.z * lo.z - radius * radius;
    float disc = b * b - 4 * a * cc;
    if (disc < 0) return false;
    *t = (-b + std::sqrt(disc)) / (2 * a);  // from inside: the far intersection
    if (*t <= 0) return false;
    Vec3 p = add(lo, scale(ld, *t));
    float phi = std::atan2(p.x, -p.z);
    *u = phi / angle + 0.5f;
    *v = 0.5f - p.y / height;
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

void App::update_pointer(const XrInput::Result& in, bool trigger_left, bool trigger_right) {
    pointer_hit_ = false;
    pointer_active_ = false;
    hovered_button_ = -1;
    for (int h = 0; h < 2; ++h) {
        aim_valid_[h] = in.aim_valid[h];
        if (in.aim_valid[h]) aim_[h] = in.aim[h];
    }
    // Pick the pointing device: right controller, left controller, right hand, left hand.
    Vec3 o, d;
    bool have = false, trigger = false;
    for (int h : {1, 0}) {
        if (!have && in.aim_valid[h]) {
            o = to_vec(in.aim[h].position);
            d = rotate(to_quat(in.aim[h].orientation), {0, 0, -1});
            trigger = h == 1 ? trigger_right : trigger_left;
            have = true;
        }
    }
    for (int h : {1, 0}) {
        const HandTracking::Hand& hand = hands_.hand(h);
        if (!have && hand.valid && !in.active[h]) {
            o = to_vec(hand.ray_origin);
            d = to_vec(hand.ray_dir);
            trigger = hand.index_pinch;
            have = true;
        }
    }
    if (!have) {
        if (pointer_down_) pointer_.button(BTN_LEFT, false);
        pointer_down_ = false;
        return;
    }
    pointer_active_ = true;
    pointer_from_ = o;
    pointer_to_ = add(o, scale(d, 2.0f));

    float best_t = 1e9f, t, u, v;
    enum { None, Panel, Screen } target = None;
    float su = 0, sv = 0;
    if (phase_ == Phase::Loading) {
        Quad p = panel_quad();
        if (ray_quad(o, d, p.pos, p.rot, p.w, p.h, &t, &u, &v) && t < best_t) {
            best_t = t;
            target = Panel;
            hovered_button_ = loading_button_at(u, v);
        }
    }
    const bool screen_shown = have_output_ && (phase_ == Phase::Loading || effective_mode() == ViewMode::Theater);
    if (screen_shown && settings_.pointer) {
        Quad s = screen_quad();
        bool hit;
        if (screen_is_cylinder()) {
            float dist = std::fmax(settings_.screen_distance, 0.3f);
            float angle = std::fmin(s.w / dist, 2.0f * kPi);
            hit = ray_cylinder(o, d, add(rest_pos_, {0, settings_.screen_height, 0}), rest_rot_, dist, angle, s.h, &t,
                               &u, &v);
        } else {
            hit = ray_quad(o, d, s.pos, s.rot, s.w, s.h, &t, &u, &v);
        }
        if (hit && t < best_t) {
            best_t = t;
            target = Screen;
            su = u;
            sv = v;
        }
    }
    if (target != None) {
        pointer_hit_ = true;
        pointer_to_ = add(o, scale(d, best_t));
    }

    if (target == Screen && uinput_ok_) {
        if (!pointer_.is_open()) pointer_.open();
        pointer_.move_to(su, sv);
    }
    const bool pressed = trigger && target != None;
    if (pressed && !pointer_down_) {
        if (target == Panel) pressed_button_ = hovered_button_;
        if (target == Screen) pointer_.button(BTN_LEFT, true);
    } else if (!trigger && pointer_down_) {
        if (pressed_button_ >= 0 && pressed_button_ == hovered_button_ && phase_ == Phase::Loading) {
            if (pressed_button_ == kButtonRescan) handle_command("rescan");
            else if (pressed_button_ == kButtonStart) start_playing(loading_view_.recommended, true);
            else start_playing(ViewMode::Theater);
        }
        pressed_button_ = -1;
        pointer_.button(BTN_LEFT, false);
    }
    if (pressed_button_ != -1 || hovered_button_ != -1) panel_dirty_ = true;
    pointer_down_ = trigger && (pointer_down_ || pressed);
}

void App::update_input(double now, float dt, bool head_ok, XrTime time) {
    const bool game = server_->active() != nullptr;
    XrInput::Result in = input_.sync(xr_.session(), dt, xr_.local_space(), time);
    if (settings_.hand_tracking && hands_.available()) hands_.update(xr_.local_space(), time);
    controllers_seen_ = controllers_seen_ || in.active[0] || in.active[1];
    hands_seen_ = hands_seen_ || hands_.hand(0).valid || hands_.hand(1).valid;

    if (in.recenter) need_recenter_ = true;
    if (in.toggle_mode && phase_ == Phase::Playing) handle_command("mode toggle");
    if (in.open_loading && phase_ == Phase::Playing) enter_loading(false);

    // Hands stand in for controllers that are not held.
    GamepadState pad = in.pad;
    if (settings_.hand_tracking) {
        const HandTracking::Hand& l = hands_.hand(0);
        const HandTracking::Hand& r = hands_.hand(1);
        if (r.valid && !in.active[1]) {
            if (r.index_pinch) pad.rt = 1.0f;
            if (r.middle_pinch) pad.a = true;
        }
        if (l.valid && !in.active[0]) {
            if (l.index_pinch) pad.lt = 1.0f;
            if (l.middle_pinch) pad.b = true;
        }
    }

    update_pointer(in, pad.lt > 0.5f, pad.rt > 0.5f);
    // A trigger that is clicking the panel must not also fire in the game.
    if (pointer_down_ && pressed_button_ >= 0) pad.lt = pad.rt = 0;

    // Game controls.
    const bool kbm = settings_.scheme == "keyboard_mouse";
    // The gameplay probe may hold the right stick for a moment.
    const bool probe_stick = calibrator_.busy() && calibrator_.stick_x() != 0.0f;
    if (probe_stick) pad.rx = calibrator_.stick_x();
    if (game && ((settings_.gamepad && !kbm) || probe_stick)) {
        if (!pad_.is_open()) pad_.open();
        pad_.update(pad);
    } else if (pad_.is_open()) {
        pad_.update(GamepadState{});
    }
    float look_yaw = 0, look_pitch = 0;
    std::set<int> pressed;
    if (game && kbm) {
        if (!keyboard_.is_open()) keyboard_.open();
        if (!mouse_.is_open()) mouse_.open();
        auto out = mapper_.update(pad, dt, settings_.turn_speed,
                                  phase_ == Phase::Playing && effective_mode() == ViewMode::Vr ? settings_.snap_turn : 0.0f);
        pressed = out.pressed;
        look_yaw = out.look_yaw_deg;
        look_pitch = out.look_pitch_deg;
    }
    for (int k : kbm_pressed_)
        if (!pressed.count(k)) is_mouse_button(k) ? mouse_.button(k, false) : keyboard_.key(k, false);
    for (int k : pressed)
        if (!kbm_pressed_.count(k)) is_mouse_button(k) ? mouse_.button(k, true) : keyboard_.key(k, true);
    kbm_pressed_ = pressed;

    if (!head_ok) return;
    if (need_recenter_) recenter(head_);

    // Head look through the virtual mouse.
    const ViewMode mode = effective_mode();
    const bool steer = phase_ == Phase::Playing && game && !calibrator_.busy() &&
                       ((mode == ViewMode::Immersive && settings_.head_mouse) || (mode == ViewMode::Vr && aim_with_head()));
    float yaw, pitch;
    yaw_pitch(to_quat(head_.orientation), &yaw, &pitch);
    const float cpd = settings_.mouse_counts_per_degree;
    if (steer) {
        if (!mouse_.is_open() && mouse_.open()) head_mouse_.reset(deg(yaw), deg(pitch));
        if (!head_mouse_.active()) head_mouse_.reset(deg(yaw), deg(pitch));
        int dx, dy;
        head_mouse_.update(deg(yaw), deg(pitch), cpd, settings_.mouse_invert_y, settings_.head_mouse_pitch, &dx, &dy);
        mouse_.move(dx, dy);
        cam_history_.push_back({now_ns(), head_mouse_.camera_yaw(), head_mouse_.camera_pitch()});
        while (cam_history_.size() > 512) cam_history_.pop_front();
    } else {
        head_mouse_.stop();
    }
    // Stick look / snap turn (keyboard_mouse scheme), converted with the calibration.
    if (look_yaw != 0 || look_pitch != 0) {
        mouse_.move(int(std::lround(look_yaw * cpd)), int(std::lround(-look_pitch * cpd * (settings_.mouse_invert_y ? -1 : 1))));
    }
    (void)now;
}

// ---------------------------------------------------------------------------
// Camera injection (VR mode)
// ---------------------------------------------------------------------------

void App::update_camera_injection(bool head_ok) {
    const bool on = phase_ == Phase::Playing && effective_mode() == ViewMode::Vr && head_ok && server_->active();
    if (!on) {
        if (camera_sent_on_) {
            auto m = make_msg<CameraMsg>(MsgType::Camera);
            m.mode = uint32_t(CameraMode::Off);
            m.rotation[3] = 1;
            server_->send_camera(m);
            camera_sent_on_ = false;
        }
        return;
    }
    // The game camera's own orientation in VR space: with head aiming it
    // follows the counts we sent through the mouse, otherwise it is the
    // rest orientation captured at recenter.
    Quat cam_world = rest_rot_;
    if (aim_with_head() && head_mouse_.active())
        cam_world = from_yaw_pitch(rad(base_yaw_ + head_mouse_.camera_yaw()), rad(base_pitch_ + head_mouse_.camera_pitch()));
    const Quat inv = conjugate(cam_world);
    const Quat delta = normalize(mul(inv, to_quat(head_.orientation)));
    const Vec3 t = rotate(inv, sub(to_vec(head_.position), rest_pos_));

    auto m = make_msg<CameraMsg>(MsgType::Camera);
    m.mode = uint32_t(settings_.vr_camera == "alternate" ? CameraMode::Alternate : CameraMode::Head);
    m.seq = ++cam_seq_ == 0 ? ++cam_seq_ : cam_seq_;
    m.rotation[0] = delta.x;
    m.rotation[1] = delta.y;
    m.rotation[2] = delta.z;
    m.rotation[3] = delta.w;
    m.translation[0] = t.x;
    m.translation[1] = t.y;
    m.translation[2] = t.z;
    m.units_per_metre = settings_.units_per_metre;
    m.ipd = settings_.ipd;
    server_->send_camera(m);
    camera_sent_on_ = true;
    pose_history_.push_back({m.seq, head_});
    while (pose_history_.size() > 512) pose_history_.pop_front();
}

// Called for every new game frame, before it is rendered.
void App::on_game_frame(double now, const FrameView& f) {
    if (calibrator_.busy()) {
        int counts = calibrator_.update(now, f.camera_valid, f.cam_rot);
        if (counts) mouse_.move(counts, 0);
        panel_dirty_ = true;
    }
    const ViewMode mode = effective_mode();
    if (phase_ == Phase::Playing && mode == ViewMode::Vr) {
        // Show the frame from the head pose it was rendered with.
        XrPosef head = head_;
        bool found = false;
        for (auto it = pose_history_.rbegin(); it != pose_history_.rend(); ++it)
            if (it->seq == f.pose_seq) {
                head = it->head;
                found = true;
                break;
            }
        if (!found) {  // not injected: the game's own camera, at the rest pose
            head.orientation = {rest_rot_.x, rest_rot_.y, rest_rot_.z, rest_rot_.w};
            head.position = {rest_pos_.x, rest_pos_.y, rest_pos_.z};
        }
        const Quat q = to_quat(head.orientation);
        for (int e = 0; e < 2; ++e) {
            if (f.eye != 0 && int(f.eye) != e + 1) continue;
            Vec3 off = rotate(q, {e == 0 ? -0.5f * settings_.ipd : 0.5f * settings_.ipd, 0, 0});
            vr_eye_pose_[e] = head;
            vr_eye_pose_[e].position = {head.position.x + off.x, head.position.y + off.y, head.position.z + off.z};
            vr_eye_valid_[e] = true;
        }
    } else if (phase_ == Phase::Playing && mode == ViewMode::Immersive) {
        if (head_mouse_.active() && !cam_history_.empty()) {
            uint64_t t = now_ns() - uint64_t(settings_.immersive_latency_ms * 1e6f);
            const CamSample* pick = &cam_history_.back();
            for (auto it = cam_history_.rbegin(); it != cam_history_.rend(); ++it) {
                pick = &*it;
                if (it->t <= t) break;
            }
            immersive_rot_ = from_yaw_pitch(rad(base_yaw_ + pick->yaw), rad(base_pitch_ + pick->pitch));
        } else {
            float yaw, pitch;
            yaw_pitch(to_quat(head_.orientation), &yaw, &pitch);
            immersive_rot_ = from_yaw_pitch(yaw, pitch);
        }
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int App::run() {
    std::string err;
    if (!xr_.init(&err)) {
        VRL_LOG("compositor", "OpenXR init failed: %s", err.c_str());
        return 1;
    }
    Gpu& gpu = xr_.gpu();
    if (!gpu.external_memory_fd)
        VRL_LOG("compositor", "VK_KHR_external_memory_fd unavailable: games will use the slower cpu transport");
    const uint32_t scene_w = std::min<uint32_t>(xr_.eye_width(), 1440), scene_h = std::min<uint32_t>(xr_.eye_height(), 1440);
    if (!stereo_.init(&gpu) || !scene_.init(&gpu, scene_w, scene_h) ||
        !panel_image_.create(gpu, kPanelWidth, kPanelHeight, VK_FORMAT_R8G8B8A8_SRGB,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !panel_staging_.create(gpu, VkDeviceSize(kPanelWidth) * kPanelHeight * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        VRL_LOG("compositor", "failed to create GPU resources");
        return 1;
    }
    if (!input_.init(xr_.instance(), xr_.session())) VRL_LOG("compositor", "controller input unavailable");
    if (xr_.hand_tracking_supported()) hands_.init(xr_.instance(), xr_.session());

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gpu.family;
    vkCreateCommandPool(gpu.device, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(gpu.device, &ai, &cmd_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(gpu.device, &fci, nullptr, &fence_);

    server_ = std::make_unique<FrameServer>(&gpu);
    if (!server_->listen(socket_name())) {
        VRL_LOG("compositor", "cannot listen on '%s' - is another vrlayer-compositor running?", socket_name().c_str());
        return 1;
    }
    server_->control_handler = [this](const std::string& c) { return handle_command(c); };
    server_->on_active_changed = [this](const ClientInfo* c) {
        load_profile(c ? c->appid : 0);
        have_output_ = false;
        screen_has_image_ = vr_has_image_ = false;
        vr_eye_valid_[0] = vr_eye_valid_[1] = false;
        camera_sent_on_ = false;
        pose_history_.clear();
        confirmed_camera_id_ = 0;
        cpd_measured_ = false;
        reset_probe();
        if (!c || settings_.loading) {
            enter_loading(false);
        } else {
            phase_ = Phase::Playing;
            need_recenter_ = true;
        }
    };
    load_profile(0);
    enter_loading(false);
    // uinput access decides whether head look / calibration are possible.
    uinput_ok_ = access("/dev/uinput", W_OK) == 0;
    VRL_LOG("compositor", "ready; start a game with ENABLE_VRLAYER=1 (Steam launch options: vrlayer-run %%command%%)");

    uint64_t last_ns = now_ns();
    while (!quit_) {
        bool xr_quit = false;
        xr_.poll_events(&xr_quit);
        if (xr_quit) break;
        server_->poll(xr_.running() ? 0 : 50);
        if (!xr_.running()) continue;

        XrFrameState fs;
        if (!xr_.wait_frame(&fs) || !xr_.begin_frame()) continue;
        const uint64_t t_ns = now_ns();
        const float dt = float(double(t_ns - last_ns) * 1e-9);
        last_ns = t_ns;
        const double now = double(t_ns) * 1e-9;

        XrPosef head;
        head_ok_ = xr_.locate_head(fs.predictedDisplayTime, &head);
        if (head_ok_) head_ = head;
        views_ok_ = xr_.locate_views(fs.predictedDisplayTime, views_);
        floor_y_ = xr_.floor_y(fs.predictedDisplayTime);

        update_input(now, dt, head_ok_, fs.predictedDisplayTime);
        update_camera_injection(head_ok_);
        server_->poll(0);  // pick up the newest game frame as late as possible
        update_gameplay_probe(now);
        if (phase_ == Phase::Loading) update_loading(now);

        layers_.clear();
        if (fs.shouldRender) {
            render(now);
            build_layers();
        }
        xr_.end_frame(fs.predictedDisplayTime, layers_);
    }

    VRL_LOG("compositor", "shutting down");
    xr_.request_exit();
    if (fence_pending_) vkWaitForFences(gpu.device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkDeviceWaitIdle(gpu.device);
    pad_.close();
    mouse_.close();
    keyboard_.close();
    pointer_.close();
    server_.reset();
    stereo_.destroy();
    scene_.destroy();
    panel_image_.destroy(gpu);
    panel_staging_.destroy(gpu);
    aer_canvas_.destroy(gpu);
    for (XrImageSwapchain* sc : {&screen_sc_, &vr_sc_, &env_sc_, &overlay_sc_, &panel_sc_}) sc->destroy();
    vkDestroyFence(gpu.device, fence_, nullptr);
    vkDestroyCommandPool(gpu.device, pool_, nullptr);
    hands_.destroy();
    input_.destroy();
    xr_.destroy();
    return 0;
}

}  // namespace vrl
