// The compositor application: OpenXR session, game frames, the VR loading
// screen, the display modes and all input handling.
#pragma once

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "calibrator.h"
#include "canvas.h"
#include "controls.h"
#include "frame_server.h"
#include "hands.h"
#include "head_mouse.h"
#include "input.h"
#include "loading.h"
#include "scene.h"
#include "settings.h"
#include "stereo.h"
#include "uinput.h"
#include "vrmath.h"
#include "xr.h"

namespace vrl {

class App {
public:
    int run();
    void request_quit() { quit_ = true; }

private:
    enum class Phase { Loading, Playing };

    // --- app.cpp: state, profiles, commands, input, camera
    void load_profile(uint32_t appid);
    void write_auto_profile();
    std::string handle_command(const std::string& cmd);
    std::string status() const;
    void enter_loading(bool restart_analysis);
    // vouch: the user says this is gameplay (START pressed), confirm the camera.
    void start_playing(ViewMode choice, bool vouch = false);
    // Gameplay probe: makes sure the camera we use follows player input.
    void update_gameplay_probe(double now);
    void reset_probe();
    bool camera_found() const;      // projection + view matrix known
    bool camera_confirmed() const;  // ... and it follows the player (or the user said so)
    bool probe_possible() const;
    uint32_t camera_id() const;
    void update_loading(double now);
    void recenter(const XrPosef& head);
    void update_input(double now, float dt, bool head_ok, XrTime time);
    void update_pointer(const XrInput::Result& in, bool trigger_left, bool trigger_right);
    // input.hand_mouse: the game's mouse cursor follows your hand (VR mode).
    void update_hand_mouse(const XrInput::Result& in, GamepadState* pad);
    void update_camera_injection(bool head_ok);
    void on_game_frame(double now, const FrameView& f);
    bool vr_available() const;
    bool aim_with_head() const;
    bool cpd_known() const;
    const AnalysisMsg* analysis() const;
    ViewMode effective_mode() const;

    // --- render.cpp: GPU work and composition layers
    void render(double now);
    void build_layers();
    void blit(VkCommandBuffer cmd, VkImage src, int32_t sx, int32_t sy, uint32_t w, uint32_t h, VkImage dst, int32_t dx,
              int32_t dy, uint32_t dw, uint32_t dh);
    void collect_prims();
    StereoParams stereo_params(bool force_mono) const;
    // Geometry of the virtual screen / panel in local space.
    struct Quad {
        Vec3 pos;
        Quat rot;
        float w, h;
    };
    Quad panel_quad() const;
    Quad screen_quad() const;
    bool screen_is_cylinder() const;

    // Core.
    XrContext xr_;
    std::unique_ptr<FrameServer> server_;
    StereoRenderer stereo_;
    SceneRenderer scene_;
    XrInput input_;
    HandTracking hands_;
    UinputMouse mouse_;
    UinputGamepad pad_;
    UinputKeyboard keyboard_;
    UinputPointer pointer_;
    HeadMouse head_mouse_;
    MouseCalibrator calibrator_;
    KeyboardMouseMapper mapper_;
    Settings settings_;
    uint32_t appid_ = 0;
    std::string profile_files_;
    volatile bool quit_ = false;

    // Phase.
    Phase phase_ = Phase::Loading;
    double loading_start_ = 0;
    double ready_since_ = -1;
    bool calibration_started_ = false;
    uint32_t probed_camera_id_ = 0;     // camera the probe is about
    uint32_t confirmed_camera_id_ = 0;  // camera known to follow the player
    double probe_retry_at_ = -1;
    bool cpd_measured_ = false;         // mouse_counts_per_degree came from a calibration
    bool fixed_camera_detected_ = false;
    // Pitch of the game's own camera (from the tracked camera). The VR rest
    // view is aligned to it so the game's horizon stays level.
    bool game_pitch_valid_ = false;
    float game_pitch_ = 0;
    Quat game_camera_rest() const;  // START pressed on a still camera that ignores input

    // Hand mouse.
    bool hand_cursor_valid_ = false;
    Vec3 hand_cursor_;
    float hand_cursor_u_ = 0.5f, hand_cursor_v_ = 0.5f;
    bool hand_click_ = false, hand_rclick_ = false;
    RelativeCursor rel_cursor_;
    LoadingView loading_view_;
    bool panel_dirty_ = true;
    double panel_drawn_at_ = -1;
    bool uinput_ok_ = false;

    // GPU.
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool fence_pending_ = false;
    XrImageSwapchain screen_sc_, vr_sc_, env_sc_, overlay_sc_, panel_sc_;
    bool screen_has_image_ = false, vr_has_image_ = false, panel_has_image_ = false;
    bool env_this_frame_ = false, overlay_this_frame_ = false;
    GpuImage panel_image_;
    GpuBuffer panel_staging_;
    Canvas panel_canvas_{kPanelWidth, kPanelHeight};
    GpuImage aer_canvas_;  // alternate-eye frames, left | right

    // Last game frame.
    bool have_output_ = false;
    bool new_output_ = false;
    uint32_t frame_w_ = 0, frame_h_ = 0;
    bool frame_stereo_ = false;
    uint64_t fps_window_start_ = 0, fps_window_frames_ = 0;
    float game_fps_ = 0;
    int reversed_score_ = 0;

    // Head / placement.
    bool need_recenter_ = true;
    bool head_ok_ = false;
    XrPosef head_{{0, 0, 0, 1}, {0, 0, 0}};
    XrView views_[2];
    bool views_ok_ = false;
    float floor_y_ = -1.6f;
    Quat rest_rot_;  // yaw-only orientation at recenter
    Vec3 rest_pos_;
    float base_yaw_ = 0, base_pitch_ = 0;  // head yaw/pitch at recenter (degrees)
    Quat immersive_rot_;
    struct CamSample {
        uint64_t t;
        float yaw, pitch;
    };
    std::deque<CamSample> cam_history_;

    // Camera injection (VR mode).
    uint32_t cam_seq_ = 0;
    bool camera_sent_on_ = false;
    struct PoseRecord {
        uint32_t seq;
        XrPosef head;
    };
    std::deque<PoseRecord> pose_history_;
    XrPosef vr_eye_pose_[2];
    bool vr_eye_valid_[2] = {false, false};

    // Input state.
    std::set<int> kbm_pressed_;
    bool pointer_down_ = false;
    int hovered_button_ = -1, pressed_button_ = -1;
    bool pointer_hit_ = false;
    bool pointer_active_ = false;
    bool aim_valid_[2] = {false, false};
    XrPosef aim_[2];
    Vec3 pointer_from_, pointer_to_;
    bool hands_seen_ = false, controllers_seen_ = false;

    // Layer storage (must outlive xrEndFrame).
    XrCompositionLayerProjectionView env_views_[2], overlay_views_[2], vr_views_[2];
    XrCompositionLayerProjection env_layer_, overlay_layer_, vr_layer_;
    XrCompositionLayerQuad quads_[3];
    XrCompositionLayerCylinderKHR cylinders_[2];
    std::vector<const XrCompositionLayerBaseHeader*> layers_;
    std::vector<ScenePrim> prims_;
};

}  // namespace vrl
