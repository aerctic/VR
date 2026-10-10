// Unit tests for the pieces that do not need a GPU or a headset.

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "camera_math.h"
#include "calibrator.h"
#include "canvas.h"
#include "config.h"
#include "controls.h"
#include "loading.h"
#include "head_mouse.h"
#include "ipc.h"
#include "protocol.h"
#include "settings.h"
#include "vrmath.h"

using namespace vrl;

static int g_failures = 0;
static int g_checks = 0;
#define EXPECT(cond)                                                      \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)
#define EXPECT_NEAR(a, b, eps) EXPECT(std::fabs(double(a) - double(b)) <= (eps))

static void test_protocol() {
    // The layer may be 32-bit while the compositor is 64-bit: layouts must match.
    static_assert(sizeof(MsgHeader) == 16, "header");
    static_assert(sizeof(HelloMsg) == 16 + 16 + 32 + 8 + 128, "hello");
    static_assert(sizeof(SlotsMsg) == 16 + 48 + 16, "slots");
    static_assert(sizeof(FrameMsg) == 16 + 8 + 16 + 16 + 48, "frame");
    static_assert(sizeof(AnalysisMsg) == 188 && sizeof(CameraMsg) == 64, "analysis / camera");
    static_assert(offsetof(FrameMsg, cam_rot) == 56, "frame camera");
    static_assert(offsetof(SlotsMsg, color_alloc_size) % 8 == 0, "alignment");
    static_assert(offsetof(FrameMsg, frame_index) % 8 == 0, "alignment");
    EXPECT(depth_bytes_per_texel(DepthFormat::D16) == 2);
    EXPECT(depth_bytes_per_texel(DepthFormat::X8D24) == 4);
    EXPECT(depth_bytes_per_texel(DepthFormat::None) == 0);
    auto m = make_msg<FrameMsg>(MsgType::Frame);
    EXPECT(m.h.magic == kMagic && m.h.size == sizeof(FrameMsg) && m.h.type == uint32_t(MsgType::Frame));
}

static void test_config() {
    Config c;
    std::string err;
    EXPECT(c.parse("top = 1\n[Display]\nMode = immersive ; comment\nscreen_width=5.5\n# full line\n"
                   "[input]\nhead_mouse = off\nname = a;b\n",
                   &err));
    EXPECT(c.get_string("top", "") == "1");
    EXPECT(c.get_string("display.mode", "") == "immersive");
    EXPECT_NEAR(c.get_double("display.screen_width", 0), 5.5, 1e-9);
    EXPECT(!c.get_bool("input.head_mouse", true));
    EXPECT(c.get_string("input.name", "") == "a;b");  // ';' only starts a comment after whitespace
    EXPECT(c.get_double("missing", 7.0) == 7.0);
    EXPECT(!c.parse("[broken\n", &err) && !err.empty());
    EXPECT(!c.parse("no equals sign\n", &err));

    // Later files override earlier ones.
    Config d;
    d.parse("[stereo]\nseparation = 0.01\nconvergence = 0.5\n");
    d.parse("[stereo]\nseparation = 0.02\n");
    EXPECT_NEAR(d.get_double("stereo.separation", 0), 0.02, 1e-9);
    EXPECT_NEAR(d.get_double("stereo.convergence", 0), 0.5, 1e-9);

    auto paths = profile_paths(1234);
    EXPECT(paths.size() == 3 && paths[1].find("games/1234.auto.ini") != std::string::npos && paths[2].find("games/1234.ini") != std::string::npos);
    EXPECT(profile_paths(0).size() == 1);
}

static void test_settings() {
    Config c;
    c.parse("[display]\nmode = immersive\ngame_fov = 100\n[stereo]\nenabled = false\nsearch_steps = 40\n"
            "depth_reversed = true\n[input]\nmouse_counts_per_degree = 12.5\n");
    Settings s = Settings::from_config(c);
    EXPECT(s.mode == ViewMode::Immersive);
    EXPECT_NEAR(s.game_fov, 100, 1e-6);
    EXPECT(!s.stereo);
    EXPECT(s.search_steps == 40);
    EXPECT(s.depth_reversed == "true");
    EXPECT_NEAR(s.mouse_counts_per_degree, 12.5, 1e-6);

    std::string err;
    EXPECT(s.set("display.mode", "theater", &err) && s.mode == ViewMode::Theater);
    EXPECT(!s.set("display.mode", "sideways", &err));
    EXPECT(!s.set("nope.nothing", "1", &err) && err.find("unknown") != std::string::npos);
    EXPECT(!s.set("stereo.depth_reversed", "maybe", &err));
    EXPECT(!s.set("stereo.separation", "abc", &err));
    EXPECT(s.set("stereo.separation", "0.04", &err) && s.get("stereo.separation") == "0.04");

    // dump() must round-trip through the INI parser.
    Config back;
    EXPECT(back.parse(s.dump(), &err));
    Settings t = Settings::from_config(back);
    EXPECT(t.dump() == s.dump());
}

static void test_shipped_profile() {
    // Every key in the shipped default profile must be a real setting, and its
    // values must match the built-in defaults so docs and code never disagree.
    Config c;
    std::string err;
    EXPECT(c.load_file(std::string(VRLAYER_SOURCE_DIR) + "/profiles/default.ini", &err));
    Settings defaults;
    for (auto& [key, value] : c.values()) {
        Settings s;
        bool ok = s.set(key, value, &err);
        if (!ok) fprintf(stderr, "  bad key in default.ini: %s (%s)\n", key.c_str(), err.c_str());
        EXPECT(ok);
        if (ok && s.get(key) != defaults.get(key))
            fprintf(stderr, "  default.ini %s = %s, built-in %s\n", key.c_str(), s.get(key).c_str(),
                    defaults.get(key).c_str());
        EXPECT(!ok || s.get(key) == defaults.get(key));
    }
    EXPECT(Settings::from_config(c).dump() == defaults.dump());
    // Values the loading screen detects must not be pinned by the shipped
    // profile, or detection could never apply them.
    Settings shipped = Settings::from_config(c);
    for (const char* auto_key : {"display.mode", "display.game_fov", "input.mouse_counts_per_degree"})
        EXPECT(!shipped.is_explicit(auto_key));
}


static Mat4 perspective_rh_vk(float vfov_deg, float aspect, float n, float f) {
    // Right-handed, -Z forward, [0,1] depth, Vulkan y-down (sy < 0).
    float t = std::tan(rad(vfov_deg) * 0.5f);
    Mat4 p{};
    p.at(0, 0) = 1.0f / (aspect * t);
    p.at(1, 1) = -1.0f / t;
    p.at(2, 2) = f / (n - f);
    p.at(2, 3) = n * f / (n - f);
    p.at(3, 2) = -1.0f;
    return p;
}

static Mat4 yaw_matrix(float deg_yaw, float tx, float ty, float tz) {
    Mat4 m = mat_identity();
    float c = std::cos(rad(deg_yaw)), s = std::sin(rad(deg_yaw));
    m.at(0, 0) = c;
    m.at(0, 2) = s;
    m.at(2, 0) = -s;
    m.at(2, 2) = c;
    m.at(0, 3) = tx;
    m.at(1, 3) = ty;
    m.at(2, 3) = tz;
    return m;
}

static void test_projection_detection() {
    float buf[16];
    ProjectionInfo pi;

    // Vulkan-style, column-major (GLSL default).
    store_matrix(buf, perspective_rh_vk(60, 4.0f / 3.0f, 0.1f, 100.0f), MatOrder::ColMajor);
    EXPECT(maybe_projection(buf));
    EXPECT(detect_projection(buf, &pi));
    EXPECT(pi.order == MatOrder::ColMajor);
    EXPECT_NEAR(pi.vfov_deg, 60, 1e-3);
    EXPECT_NEAR(pi.aspect, 4.0 / 3.0, 1e-4);
    EXPECT_NEAR(pi.hfov_deg, 2 * deg(std::atan(std::tan(rad(30)) * 4.0f / 3.0f)), 1e-3);
    EXPECT_NEAR(pi.near_d, 0.1, 1e-5);
    EXPECT_NEAR(pi.far_d, 100, 1e-2);
    EXPECT(!pi.reversed);
    EXPECT(pi.fwd == -1.0f && pi.sy < 0);
    // depth_a + depth_b / d reproduces the depth buffer value.
    EXPECT_NEAR(pi.depth_a + pi.depth_b / 0.1f, 0.0, 1e-4);
    EXPECT_NEAR(pi.depth_a + pi.depth_b / 100.0f, 1.0, 1e-4);

    // D3D left-handed reversed-Z infinite far, stored as HLSL row-vector data.
    Mat4 d3d{};
    d3d.at(0, 0) = 1.0f;          // 90 deg hfov
    d3d.at(1, 1) = 16.0f / 9.0f;  // 16:9
    d3d.at(2, 3) = 0.25f;         // near
    d3d.at(3, 2) = 1.0f;          // +Z forward
    store_matrix(buf, d3d, MatOrder::RowMajor);
    EXPECT(detect_projection(buf, &pi));
    EXPECT(pi.order == MatOrder::RowMajor);
    EXPECT(pi.reversed && pi.far_d == 0.0f && pi.fwd == 1.0f);
    EXPECT_NEAR(pi.near_d, 0.25, 1e-6);
    EXPECT_NEAR(pi.hfov_deg, 90, 1e-3);
    EXPECT_NEAR(pi.aspect, 16.0 / 9.0, 1e-4);
    EXPECT_NEAR(pi.depth_a + pi.depth_b / 0.25f, 1.0, 1e-5);  // near = 1

    // An OpenGL-convention matrix is still recognised as a projection (its
    // near/far are read in Vulkan's [0, 1] convention).
    Mat4 gl = perspective_rh_vk(70, 1.5f, 0.5f, 500.0f);
    gl.at(2, 2) = -(500.0f + 0.5f) / (500.0f - 0.5f);
    gl.at(2, 3) = -2.0f * 500.0f * 0.5f / (500.0f - 0.5f);
    store_matrix(buf, gl, MatOrder::ColMajor);
    EXPECT(detect_projection(buf, &pi));
    EXPECT_NEAR(pi.vfov_deg, 70, 1e-3);

    // TAA jitter is tolerated.
    Mat4 jit = perspective_rh_vk(60, 1.0f, 0.1f, 10.0f);
    jit.at(0, 2) = 0.0013f;
    jit.at(1, 2) = -0.0009f;
    store_matrix(buf, jit, MatOrder::ColMajor);
    EXPECT(detect_projection(buf, &pi));

    // Not projections: orthographic, identity, view matrices, garbage.
    Mat4 ortho = mat_identity();
    ortho.at(0, 0) = 0.01f;
    ortho.at(1, 1) = 0.01f;
    store_matrix(buf, ortho, MatOrder::ColMajor);
    EXPECT(!detect_projection(buf, &pi));
    store_matrix(buf, yaw_matrix(30, 1, 2, 3), MatOrder::ColMajor);
    EXPECT(!detect_projection(buf, &pi));
    for (int i = 0; i < 16; ++i) buf[i] = float(i) * 0.37f;
    EXPECT(!detect_projection(buf, &pi));
    Mat4 nan = perspective_rh_vk(60, 1.0f, 0.1f, 10.0f);
    nan.at(2, 2) = NAN;
    store_matrix(buf, nan, MatOrder::ColMajor);
    EXPECT(!detect_projection(buf, &pi));
}

static void test_rigid_and_injection() {
    float buf[16];
    Mat4 v;
    store_matrix(buf, yaw_matrix(30, 1, 2, 3), MatOrder::RowMajor);
    EXPECT(detect_rigid(buf, MatOrder::RowMajor, &v));
    EXPECT(mat_near(v, yaw_matrix(30, 1, 2, 3), 1e-6f));
    EXPECT(!detect_rigid(buf, MatOrder::ColMajor, &v));  // wrong order: translation in the bottom row
    store_matrix(buf, mat_identity(), MatOrder::ColMajor);
    EXPECT(!detect_rigid(buf, MatOrder::ColMajor, &v));  // identity = static object, not a camera
    Mat4 scaled = yaw_matrix(30, 0, 0, 0);
    for (int i = 0; i < 3; ++i) scaled.at(i, i) *= 2.0f;
    store_matrix(buf, scaled, MatOrder::ColMajor);
    EXPECT(!detect_rigid(buf, MatOrder::ColMajor, &v));

    EXPECT_NEAR(rotation_angle_deg(yaw_matrix(30, 0, 0, 0), yaw_matrix(50, 5, 5, 5)), 20, 1e-3);
    Mat4 m = yaw_matrix(40, 1, 2, 3);
    EXPECT(mat_near(mat_mul(m, rigid_inverse(m)), mat_identity(), 1e-5f));

    // camera_delta: the game camera's forward axis must turn exactly like the
    // VR head forward axis, mapped through the basis change, for every basis.
    const Quat q = from_yaw_pitch(rad(25), rad(-10));
    const float qa[4] = {q.x, q.y, q.z, q.w}, t[3] = {0.1f, 0.2f, -0.3f};
    for (float up : {1.0f, -1.0f})
        for (float fwd : {1.0f, -1.0f}) {
            CameraBasis b;
            b.up = up;
            b.fwd = fwd;
            Mat4 d = camera_delta(b, qa, t, 100.0f);
            Vec3 head_fwd = rotate(q, {0, 0, -1});
            // Game forward (0,0,fwd) -> D -> compare with S * head_fwd, S = diag(1, up, -fwd).
            EXPECT_NEAR(d.at(0, 2) * fwd, head_fwd.x, 1e-5);
            EXPECT_NEAR(d.at(1, 2) * fwd, up * head_fwd.y, 1e-5);
            EXPECT_NEAR(d.at(2, 2) * fwd, -fwd * head_fwd.z, 1e-5);
            EXPECT_NEAR(d.at(0, 3), 10.0f, 1e-4);  // metres -> game units
            EXPECT_NEAR(d.at(1, 3), up * 20.0f, 1e-4);
            EXPECT_NEAR(d.at(2, 3), -fwd * -30.0f, 1e-4);
            EXPECT(std::fabs(rotation_angle_deg(mat_identity(), d) -
                             rotation_angle_deg(mat_identity(), mat_identity())) > 1.0f);
        }

    // Basis from projection + viewport flip.
    ProjectionInfo p;
    p.sy = -1;
    p.fwd = -1;
    EXPECT(camera_basis(p, false).up == 1.0f);  // native Vulkan: sy < 0, no flip
    p.sy = 1;
    EXPECT(camera_basis(p, true).up == 1.0f);   // DXVK: D3D matrix + negative viewport
    EXPECT(camera_basis(p, false).up == -1.0f);

    // Injecting a head yaw into a view matrix turns the rendered camera.
    Mat4 view = rigid_inverse(yaw_matrix(10, 4, 5, 6));  // camera at (4,5,6), yawed 10 deg
    CameraBasis b;  // OpenXR-like game
    const Quat yaw20 = from_yaw_pitch(rad(20), 0);
    const float q20[4] = {yaw20.x, yaw20.y, yaw20.z, yaw20.w}, zero[3] = {0, 0, 0};
    Mat4 v2 = inject_view(view, camera_delta(b, q20, zero, 1.0f));
    Mat4 cam2 = rigid_inverse(v2);
    EXPECT(mat_near(cam2, yaw_matrix(30, 4, 5, 6), 1e-5f));
}


// Simulated game: camera yaw follows mouse counts with a lag of `lag` frames.
struct SimGame {
    float cpd = 12.5f;          // true counts per degree
    float accel = 1.0f;         // >1: positive moves turn more (mouse acceleration)
    bool responds = true;        // camera follows the mouse
    bool stick = false;          // camera follows the right stick
    bool orbit = false;          // scripted menu camera turning on its own
    int lag = 3;
    float stick_x = 0;
    std::vector<int> pending;   // counts queued for future frames
    float yaw = 15.0f;
    void push(int counts) { pending.push_back(counts); }
    void frame(float rot[9]) {
        if (int(pending.size()) > lag) {
            int c = pending.front();
            pending.erase(pending.begin());
            if (responds) yaw -= float(c) / cpd * (c > 0 ? accel : 1.0f);  // mouse right = turn right
        }
        if (stick) yaw -= stick_x * 90.0f / 60.0f;  // 90 deg/s at full deflection
        if (orbit) yaw += 0.5f;
        float r = rad(yaw);
        const float m[9] = {std::cos(r), 0, std::sin(r), 0, 1, 0, -std::sin(r), 0, std::cos(r)};
        memcpy(rot, m, sizeof(m));
    }
};

static int run_calibration(SimGame& g, MouseCalibrator& cal, int* net_counts) {
    cal.start(0.0, 400);
    *net_counts = 0;
    float rot[9];
    int frames = 0;
    for (; frames < 60 * 60 && cal.busy(); ++frames) {
        g.frame(rot);
        int c = cal.update(frames / 60.0, true, rot);
        *net_counts += c;
        g.push(c);
        g.stick_x = cal.stick_x();
    }
    return frames;
}

static void test_calibrator() {
    {
        SimGame g;
        MouseCalibrator cal;
        int net;
        run_calibration(g, cal, &net);
        EXPECT(cal.state() == MouseCalibrator::State::Done);
        EXPECT_NEAR(cal.counts_per_degree(), 12.5, 0.25);
        EXPECT(net == 0);  // the camera ends where it started
    }
    {
        SimGame g;
        g.responds = false;
        MouseCalibrator cal;
        int net;
        run_calibration(g, cal, &net);
        // A main menu: ignores the mouse and the stick -> not gameplay.
        EXPECT(cal.state() == MouseCalibrator::State::Failed);
        EXPECT(!cal.gameplay_confirmed());
        EXPECT(cal.message().find("ignores the mouse and the controller") != std::string::npos);
        EXPECT(net == 0);
        EXPECT(cal.stick_x() == 0.0f);
    }
    {
        // Controller-only game: the right stick turns the camera.
        SimGame g;
        g.responds = false;
        g.stick = true;
        const float start_yaw = g.yaw;
        MouseCalibrator cal;
        int net;
        run_calibration(g, cal, &net);
        EXPECT(cal.state() == MouseCalibrator::State::DoneStick);
        EXPECT(cal.gameplay_confirmed());
        EXPECT(cal.stick_x() == 0.0f);
        EXPECT_NEAR(g.yaw, start_yaw, 1.0);  // turned back
    }
    {
        // Scripted orbiting menu camera: never still -> not gameplay.
        SimGame g;
        g.responds = false;
        g.orbit = true;
        MouseCalibrator cal;
        int net;
        run_calibration(g, cal, &net);
        EXPECT(cal.state() == MouseCalibrator::State::Failed && !cal.gameplay_confirmed());
        EXPECT(cal.message().find("moving on its own") != std::string::npos);
    }
    {
        // Gameplay with mouse acceleration: no calibration, but it IS gameplay.
        SimGame g;
        g.accel = 1.6f;
        MouseCalibrator cal;
        int net;
        run_calibration(g, cal, &net);
        EXPECT(cal.gameplay_confirmed());
    }
    {
        SimGame g;
        g.accel = 1.6f;
        MouseCalibrator cal;
        int net;
        run_calibration(g, cal, &net);
        EXPECT(cal.state() == MouseCalibrator::State::Failed);
        EXPECT(cal.message().find("symmetric") != std::string::npos);
    }
    {
        // No camera at all: gives up instead of hanging.
        MouseCalibrator cal;
        cal.start(0.0);
        float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (int i = 0; i < 60 * 20; ++i) cal.update(i / 60.0, false, rot);
        EXPECT(cal.state() == MouseCalibrator::State::Failed);
    }
}

static void test_controls() {
    EXPECT(key_code("space") == KEY_SPACE && key_code("E") == KEY_E && key_code("f5") == KEY_F5);
    EXPECT(key_code("0") == KEY_0 && key_code("9") == KEY_9 && key_code("mouse_left") == BTN_LEFT);
    EXPECT(key_code("nonsense") == -1);
    KeyboardMouseMapper m;
    std::string err;
    EXPECT(m.bind("a", "enter", &err));
    EXPECT(!m.bind("a", "nope", &err));
    EXPECT(!m.bind("nobutton", "e", &err));
    GamepadState s;
    s.ly = 1.0f;
    s.a = true;
    s.rt = 1.0f;
    auto o = m.update(s, 0.016f, 120, 30);
    EXPECT(o.pressed.count(KEY_W) && o.pressed.count(KEY_ENTER) && o.pressed.count(BTN_LEFT));
    EXPECT(!o.pressed.count(KEY_S) && !o.pressed.count(KEY_SPACE));
    // Snap turn fires once per flick.
    s = GamepadState{};
    s.rx = 0.9f;
    EXPECT(m.update(s, 0.016f, 120, 30).look_yaw_deg == 30.0f);
    EXPECT(m.update(s, 0.016f, 120, 30).look_yaw_deg == 0.0f);
    s.rx = 0.0f;
    m.update(s, 0.016f, 120, 30);
    s.rx = -0.9f;
    EXPECT(m.update(s, 0.016f, 120, 30).look_yaw_deg == -30.0f);
    // Smooth turning scales with time.
    s.rx = 1.0f;
    EXPECT_NEAR(m.update(s, 0.5f, 120, 0).look_yaw_deg, 60.0, 1e-3);
}

static void test_loading_logic() {
    Settings s;
    LoadingInputs in;
    LoadingView v = build_loading_view(in, s, -1);
    EXPECT(!v.ready && v.subtitle.find("Waiting") != std::string::npos);

    in.game = true;
    in.game_name = "game.exe";
    in.fps = 60;
    in.uinput = true;
    in.elapsed = 5;
    v = build_loading_view(in, s, -1);
    EXPECT(!v.ready && v.recommended == ViewMode::Theater);

    in.has_analysis = true;
    in.analysis.flags = kAnalysisProjection | kAnalysisView | kAnalysisDepth;
    in.analysis.hfov_deg = 90;
    in.probe_possible = true;
    in.calibration = MouseCalibrator::State::Turning;
    v = build_loading_view(in, s, -1);
    EXPECT(v.recommended == ViewMode::Vr && !v.ready);  // still checking

    // The main menu's 3D camera: found, but it ignores input. Never auto-starts.
    in.calibration = MouseCalibrator::State::Failed;
    v = build_loading_view(in, s, -1);
    EXPECT(!v.ready && v.countdown < 0);
    EXPECT(v.hint.find("menu") != std::string::npos);
    // Everything on the panel fits without being cut off.
    for (auto& it : v.items) EXPECT(Canvas::text_width(it.detail, 2) <= kPanelWidth - 172);
    EXPECT(Canvas::text_width(v.hint, 2) <= kPanelWidth - 96);
    EXPECT(v.recommended == ViewMode::Vr);  // what START would do if you vouch for it

    // Gameplay confirmed: counts down into VR.
    in.calibration = MouseCalibrator::State::Done;
    in.gameplay_confirmed = true;
    v = build_loading_view(in, s, 5.0);
    EXPECT(v.ready && v.recommended == ViewMode::Vr && v.auto_mode == ViewMode::Vr);
    EXPECT(v.countdown > 4.9 && v.countdown <= 5.0);
    in.elapsed = 12;
    EXPECT(build_loading_view(in, s, 5.0).countdown == 0.0);

    // Timeout on an unconfirmed camera: theater, never VR.
    in.gameplay_confirmed = false;
    in.calibration = MouseCalibrator::State::Failed;
    in.elapsed = s.loading_timeout + 1;
    v = build_loading_view(in, s, -1);
    EXPECT(v.ready && v.auto_mode == ViewMode::Theater);

    // No way to probe (no uinput): wait for START, theater on timeout.
    in.probe_possible = false;
    in.elapsed = 10;
    EXPECT(!build_loading_view(in, s, -1).ready);
    in.elapsed = s.loading_timeout + 1;
    EXPECT(build_loading_view(in, s, -1).auto_mode == ViewMode::Theater);
    in.probe_possible = true;
    in.elapsed = 12;

    // Projection but no view: immersive when mouse look works, else theater.
    in.analysis.flags = kAnalysisProjection;
    in.calibration = MouseCalibrator::State::Done;
    in.gameplay_confirmed = true;
    EXPECT(build_loading_view(in, s, -1).recommended == ViewMode::Immersive);
    in.calibration = MouseCalibrator::State::DoneStick;
    EXPECT(build_loading_view(in, s, -1).recommended == ViewMode::Theater);

    // Nothing found: ready only after the timeout.
    in.has_analysis = false;
    in.gameplay_confirmed = false;
    in.elapsed = 10;
    EXPECT(!build_loading_view(in, s, -1).ready);
    in.elapsed = s.loading_timeout + 1;
    EXPECT(build_loading_view(in, s, -1).ready);

    // Buttons.
    EXPECT(loading_button_at(100.0f / kPanelWidth, 630.0f / kPanelHeight) == 0);
    EXPECT(loading_button_at(600.0f / kPanelWidth, 630.0f / kPanelHeight) == kButtonTheater);
    EXPECT(loading_button_at(850.0f / kPanelWidth, 630.0f / kPanelHeight) == kButtonRescan);
    EXPECT(loading_button_at(0.5f, 0.1f) == -1);
}

static void test_canvas() {
    for (char ch : std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:-+/()%!?=_<>[]*#@abcxyz&"))
        EXPECT(font_has_glyph(ch));
    Canvas c(64, 16);
    c.clear(0x000000FF);
    int w = c.text(1, 1, "HI", 1, 0xFFFFFFFF);
    EXPECT(w == 12 && Canvas::text_width("HI", 2) == 24);
    // 'H' column 0 is lit on every row, column 1 only in the middle row.
    auto px = [&](int x, int y) { return c.data()[(y * 64 + x) * 4]; };
    EXPECT(px(1, 1) == 255 && px(1, 7) == 255 && px(2, 1) == 0 && px(2, 4) == 255);
    c.fill_rect(0, 0, 4, 4, 0xFF000080);  // 50% red over black
    EXPECT(std::abs(int(c.data()[0]) - 128) <= 1 && c.data()[1] == 0);
}

static void test_head_mouse() {
    EXPECT_NEAR(angle_diff_deg(10, 350), 20, 1e-4);
    EXPECT_NEAR(angle_diff_deg(350, 10), -20, 1e-4);
    EXPECT_NEAR(angle_diff_deg(-179, 179), 2, 1e-4);

    HeadMouse hm;
    int dx, dy;
    hm.update(10, 0, 10, false, true, &dx, &dy);  // inactive: no motion
    EXPECT(dx == 0 && dy == 0);
    hm.reset(0, 0);
    hm.update(-1.0f, 0.5f, 10, false, true, &dx, &dy);  // turn right 1 deg, look up 0.5
    EXPECT(dx == 10 && dy == -5);
    hm.update(-1.0f, 0.5f, 10, false, true, &dx, &dy);
    EXPECT(dx == 0 && dy == 0);
    // Sub-count motion accumulates instead of being lost.
    int total = 0;
    float yaw = -1.0f;
    for (int i = 0; i < 100; ++i) {
        yaw -= 0.01f;
        hm.update(yaw, 0.5f, 10, false, true, &dx, &dy);
        total += dx;
    }
    EXPECT(total >= 9 && total <= 10);
    EXPECT_NEAR(hm.camera_yaw(), yaw, 0.1);
    EXPECT_NEAR(hm.camera_pitch(), 0.5, 0.1);
    // Wrap-around at +-180 must not produce a huge jump.
    hm.reset(179, 0);
    hm.update(-179, 0, 10, false, false, &dx, &dy);
    EXPECT(dx == -20 && dy == 0);
    // Inverted Y.
    hm.reset(0, 0);
    hm.update(0, 1, 10, true, true, &dx, &dy);
    EXPECT(dy == 10);
}

static void test_math() {
    float yaw, pitch;
    yaw_pitch(Quat{}, &yaw, &pitch);
    EXPECT_NEAR(yaw, 0, 1e-6);
    EXPECT_NEAR(pitch, 0, 1e-6);
    for (float y : {-170.0f, -45.0f, 0.0f, 30.0f, 120.0f}) {
        for (float p : {-60.0f, 0.0f, 25.0f}) {
            yaw_pitch(from_yaw_pitch(rad(y), rad(p)), &yaw, &pitch);
            EXPECT_NEAR(deg(yaw), y, 1e-3);
            EXPECT_NEAR(deg(pitch), p, 1e-3);
        }
    }
    // Yaw +90 (turn left) makes forward point to -X.
    Vec3 f = rotate(from_yaw_pitch(rad(90), 0), {0, 0, -1});
    EXPECT_NEAR(f.x, -1, 1e-5);
    EXPECT_NEAR(f.z, 0, 1e-5);
    // Roll does not affect yaw/pitch.
    Quat rolled = mul(from_yaw_pitch(rad(30), rad(10)), from_axis_angle({0, 0, 1}, rad(40)));
    yaw_pitch(rolled, &yaw, &pitch);
    EXPECT_NEAR(deg(yaw), 30, 1e-3);
    EXPECT_NEAR(deg(pitch), 10, 1e-3);
}

static void test_ipc() {
    std::string name = "vrlayer-unit-" + std::to_string(getpid());
    int l = ipc_listen(name);
    EXPECT(l >= 0);
    EXPECT(ipc_listen(name) < 0);  // already taken
    int c = ipc_connect(name);
    EXPECT(c >= 0);
    int s = -1;
    for (int i = 0; i < 100 && s < 0; ++i) {
        s = ipc_accept(l);
        if (s < 0) usleep(1000);
    }
    EXPECT(s >= 0);

    // Message with two fds.
    int p[2];
    EXPECT(pipe(p) == 0);
    auto m = make_msg<ReleaseMsg>(MsgType::Release);
    m.slot = 2;
    int fds[2] = {p[0], p[1]};
    EXPECT(ipc_send(c, &m, sizeof(m), fds, 2));
    char buf[kMaxMsgSize];
    std::vector<int> got;
    ssize_t n = ipc_recv_timeout(s, buf, sizeof(buf), &got, 1000);
    EXPECT(n == ssize_t(sizeof(m)));
    EXPECT(reinterpret_cast<ReleaseMsg*>(buf)->slot == 2);
    EXPECT(got.size() == 2);
    if (got.size() == 2) {
        // The received fds refer to the same pipe.
        EXPECT(write(got[1], "x", 1) == 1);
        char ch = 0;
        EXPECT(read(p[0], &ch, 1) == 1 && ch == 'x');
    }
    close_fds(got);
    close(p[0]);
    close(p[1]);

    EXPECT(ipc_recv(s, buf, sizeof(buf), nullptr) == kIpcWouldBlock);
    close(c);
    EXPECT(ipc_recv_timeout(s, buf, sizeof(buf), nullptr, 1000) == kIpcClosed);
    close(s);
    close(l);
}

int main() {
    test_protocol();
    test_config();
    test_settings();
    test_shipped_profile();
    test_head_mouse();
    test_math();
    test_projection_detection();
    test_rigid_and_injection();
    test_ipc();
    test_calibrator();
    test_controls();
    test_loading_logic();
    test_canvas();
    fprintf(stderr, "%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
