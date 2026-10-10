// vrlayer-compositor: OpenXR application that shows games captured by the
// VRLayer Vulkan layer in VR.
//
//  theater   - the game on a large (optionally curved) world-locked screen,
//              in stereo 3D reconstructed from the depth buffer.
//  immersive - the game fills your field of view at its own FOV; head
//              rotation drives the game camera through a virtual mouse and the
//              last frame stays world-locked until the next one arrives.

#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "frame_server.h"
#include "head_mouse.h"
#include "input.h"
#include "ipc.h"
#include "log.h"
#include "settings.h"
#include "stereo.h"
#include "uinput.h"
#include "vrmath.h"
#include "xr.h"

#ifndef VRLAYER_DATADIR
#define VRLAYER_DATADIR "/usr/local/share/vrlayer"
#endif

namespace vrl {

static volatile sig_atomic_t g_quit = 0;

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static Quat to_quat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
static XrQuaternionf to_xr(const Quat& q) { return {q.x, q.y, q.z, q.w}; }
static Vec3 to_vec(const XrVector3f& v) { return {v.x, v.y, v.z}; }
static XrVector3f to_xr(const Vec3& v) { return {v.x, v.y, v.z}; }

class App {
public:
    int run();

private:
    void load_profile(uint32_t appid);
    std::string handle_command(const std::string& cmd);
    std::string status() const;
    void recenter(const XrPosef& head);
    bool render_frame();  // true if the screen swapchain got a new image
    void update_input(const XrFrameState& fs, bool head_ok, const XrPosef& head);
    void build_layers(const XrPosef& head);

    XrContext xr_;
    FrameServer* server_ = nullptr;
    StereoRenderer stereo_;
    XrInput input_;
    UinputMouse mouse_;
    UinputGamepad pad_;
    HeadMouse head_mouse_;
    Settings settings_;
    uint32_t appid_ = 0;
    std::string profile_files_;
    bool quit_ = false;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool fence_pending_ = false;

    // What we last rendered.
    bool have_output_ = false;
    uint32_t frame_w_ = 0, frame_h_ = 0;
    bool frame_stereo_ = false;
    uint64_t frames_shown_ = 0;
    uint64_t fps_window_start_ = 0, fps_window_frames_ = 0;
    float game_fps_ = 0;

    // Placement.
    bool need_recenter_ = true;
    XrPosef head_{{0, 0, 0, 1}, {0, 0, 0}};
    Quat theater_rot_;
    Vec3 theater_pos_;
    Quat immersive_rot_;  // orientation of the game camera for the frame on screen
    float base_yaw_ = 0, base_pitch_ = 0;
    struct CamSample {
        uint64_t t;
        float yaw, pitch;
    };
    std::deque<CamSample> cam_history_;
    int reversed_score_ = 0;
    uint64_t last_input_ns_ = 0;

    // Layers (storage must outlive xrEndFrame).
    XrCompositionLayerQuad quads_[2];
    XrCompositionLayerCylinderKHR cylinders_[2];
    std::vector<const XrCompositionLayerBaseHeader*> layers_;
};

void App::load_profile(uint32_t appid) {
    appid_ = appid;
    Config cfg;
    std::vector<std::string> files;
    std::vector<std::string> candidates{std::string(VRLAYER_DATADIR) + "/profiles/default.ini"};
    if (appid) candidates.push_back(std::string(VRLAYER_DATADIR) + "/profiles/games/" + std::to_string(appid) + ".ini");
    for (auto& p : profile_paths(appid)) candidates.push_back(p);
    for (auto& p : candidates) {
        if (access(p.c_str(), R_OK) != 0) continue;
        std::string err;
        if (cfg.load_file(p, &err)) files.push_back(p);
        else VRL_LOG("compositor", "%s", err.c_str());
    }
    settings_ = Settings::from_config(cfg);
    profile_files_.clear();
    for (auto& f : files) profile_files_ += (profile_files_.empty() ? "" : ", ") + f;
    VRL_LOG("compositor", "profile for appid %u: %s", appid, profile_files_.empty() ? "built-in defaults" : profile_files_.c_str());
    reversed_score_ = 0;
    need_recenter_ = true;
}

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
    o << "mode: " << (settings_.mode == ViewMode::Theater ? "theater" : "immersive")
      << ", stereo: " << (frame_stereo_ ? "on" : "off") << " (reversed-z "
      << (settings_.depth_reversed == "auto" ? (reversed_score_ > 0 ? "auto: yes" : "auto: no")
                                             : settings_.depth_reversed)
      << ")\n"
      << "head mouse: " << (mouse_.is_open() ? "available" : "unavailable")
      << ", gamepad: " << (pad_.is_open() ? "available" : "unavailable") << "\n"
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
    if (cmd == "mode") {
        if (a == "toggle") a = settings_.mode == ViewMode::Theater ? "immersive" : "theater";
        std::string err;
        if (!settings_.set("display.mode", a, &err)) return "error: " + err;
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
        if (a.rfind("display.", 0) == 0) need_recenter_ = true;
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
    return "commands: status | recenter | mode theater|immersive|toggle | 3d on|off|toggle | set <key> <value> | "
           "get <key> | dump | reload | save | next | quit";
}

void App::recenter(const XrPosef& head) {
    float yaw, pitch;
    yaw_pitch(to_quat(head.orientation), &yaw, &pitch);
    theater_rot_ = from_yaw_pitch(yaw, 0);
    theater_pos_ = to_vec(head.position);
    base_yaw_ = deg(yaw);
    base_pitch_ = deg(pitch);
    head_mouse_.reset(deg(yaw), deg(pitch));
    cam_history_.clear();
    immersive_rot_ = from_yaw_pitch(yaw, pitch);
    need_recenter_ = false;
    VRL_DBG("compositor", "recentered (yaw %.1f)", deg(yaw));
}

void App::update_input(const XrFrameState& fs, bool head_ok, const XrPosef& head) {
    uint64_t now = now_ns();
    float dt = last_input_ns_ ? float(double(now - last_input_ns_) * 1e-9) : 0.0f;
    last_input_ns_ = now;
    const bool game = server_->active() != nullptr;

    XrInput::Result in = input_.sync(xr_.session(), dt);
    if (in.recenter) need_recenter_ = true;
    if (in.toggle_mode) handle_command("mode toggle");
    if (settings_.gamepad && game) {
        if (!pad_.is_open()) pad_.open();
        pad_.update(in.pad);
    } else if (pad_.is_open()) {
        pad_.update(GamepadState{});
    }

    if (!head_ok) return;
    if (need_recenter_) recenter(head);

    const bool steer = settings_.mode == ViewMode::Immersive && settings_.head_mouse && game;
    float yaw, pitch;
    yaw_pitch(to_quat(head.orientation), &yaw, &pitch);
    if (steer) {
        if (!mouse_.is_open() && mouse_.open()) head_mouse_.reset(deg(yaw), deg(pitch));
        if (!head_mouse_.active()) head_mouse_.reset(deg(yaw), deg(pitch));
        int dx, dy;
        head_mouse_.update(deg(yaw), deg(pitch), settings_.mouse_counts_per_degree, settings_.mouse_invert_y,
                           settings_.head_mouse_pitch, &dx, &dy);
        mouse_.move(dx, dy);
        cam_history_.push_back({now, head_mouse_.camera_yaw(), head_mouse_.camera_pitch()});
        while (cam_history_.size() > 512) cam_history_.pop_front();
    } else {
        head_mouse_.stop();
    }
    (void)fs;
}

bool App::render_frame() {
    // The previous submission is done: hand its game frame back.
    if (fence_pending_) {
        vkWaitForFences(xr_.gpu().device, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(xr_.gpu().device, 1, &fence_);
        fence_pending_ = false;
        server_->gpu_work_done();
        reversed_score_ += stereo_.read_depth_vote();
        if (reversed_score_ > 30) reversed_score_ = 30;
        if (reversed_score_ < -30) reversed_score_ = -30;
    }

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    FrameView f;
    if (server_->has_new_frame() && server_->begin_frame(cmd_, &f)) {
        StereoParams p;
        p.stereo = settings_.stereo;
        p.reversed_depth = settings_.depth_reversed == "auto" ? reversed_score_ > 0 : settings_.depth_reversed == "true";
        p.flip_y = settings_.depth_flip_y;
        p.show_depth = settings_.show_depth;
        p.separation = settings_.separation;
        p.convergence = settings_.convergence;
        p.popout = settings_.popout;
        p.far_plane = settings_.depth_far_plane;
        p.search_steps = settings_.search_steps;
        if (stereo_.record(cmd_, f, p)) {
            have_output_ = true;
            frame_w_ = f.width;
            frame_h_ = f.height;
            frame_stereo_ = stereo_.output_is_stereo();
            ++frames_shown_;
            ++fps_window_frames_;

            // Immersive: the frame shows the game camera as it was when the
            // game rendered it; place it there so it stays world-locked.
            if (settings_.mode == ViewMode::Immersive) {
                float cy = 0, cp = 0;
                if (head_mouse_.active() && !cam_history_.empty()) {
                    uint64_t t = now_ns() - uint64_t(settings_.immersive_latency_ms * 1e6f);
                    const CamSample* pick = &cam_history_.back();
                    for (auto it = cam_history_.rbegin(); it != cam_history_.rend(); ++it) {
                        pick = &*it;
                        if (it->t <= t) break;
                    }
                    cy = pick->yaw;
                    cp = pick->pitch;
                    immersive_rot_ = from_yaw_pitch(rad(base_yaw_ + cy), rad(base_pitch_ + cp));
                } else {
                    float yaw, pitch;
                    yaw_pitch(to_quat(head_.orientation), &yaw, &pitch);
                    immersive_rot_ = from_yaw_pitch(yaw, pitch);
                }
            }
        }
    }
    uint64_t now = now_ns();
    if (!fps_window_start_) fps_window_start_ = now;
    if (now - fps_window_start_ > 1000000000ull) {
        game_fps_ = float(fps_window_frames_) / float(double(now - fps_window_start_) * 1e-9);
        fps_window_start_ = now;
        fps_window_frames_ = 0;
    }

    VkImage target = VK_NULL_HANDLE;
    if (have_output_ && xr_.ensure_swapchain(stereo_.output_width(), stereo_.output_height()))
        target = xr_.acquire_image();
    if (target) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = target;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(stereo_.output_width()), int32_t(stereo_.output_height()), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cmd_, stereo_.output(), VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_NEAREST);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(xr_.gpu().queue, 1, &si, fence_);
    fence_pending_ = true;
    if (target) xr_.release_image();
    return target != VK_NULL_HANDLE;
}

void App::build_layers(const XrPosef& head) {
    layers_.clear();
    if (!have_output_ || !xr_.swapchain() || frame_w_ == 0 || frame_h_ == 0) return;
    const float aspect = float(frame_w_) / float(frame_h_);
    const int eyes = frame_stereo_ ? 2 : 1;
    const XrEyeVisibility vis[2] = {frame_stereo_ ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH,
                                    XR_EYE_VISIBILITY_RIGHT};

    XrSwapchainSubImage sub[2] = {};
    for (int e = 0; e < eyes; ++e) {
        sub[e].swapchain = xr_.swapchain();
        sub[e].imageRect.offset = {int32_t(e * frame_w_), 0};
        sub[e].imageRect.extent = {int32_t(frame_w_), int32_t(frame_h_)};
    }

    if (settings_.mode == ViewMode::Theater) {
        const float dist = settings_.screen_distance > 0.3f ? settings_.screen_distance : 0.3f;
        const float width = settings_.screen_width > 0.1f ? settings_.screen_width : 0.1f;
        if (settings_.curved && xr_.cylinder_supported()) {
            for (int e = 0; e < eyes; ++e) {
                XrCompositionLayerCylinderKHR& c = cylinders_[e];
                c = XrCompositionLayerCylinderKHR{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                c.space = xr_.local_space();
                c.eyeVisibility = vis[e];
                c.subImage = sub[e];
                c.pose.orientation = to_xr(theater_rot_);
                c.pose.position = to_xr(add(theater_pos_, {0, settings_.screen_height, 0}));
                c.radius = dist;
                c.centralAngle = std::fmin(width / dist, 2.0f * kPi);
                c.aspectRatio = aspect;
                layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&c));
            }
            return;
        }
        for (int e = 0; e < eyes; ++e) {
            XrCompositionLayerQuad& q = quads_[e];
            q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.space = xr_.local_space();
            q.eyeVisibility = vis[e];
            q.subImage = sub[e];
            q.pose.orientation = to_xr(theater_rot_);
            q.pose.position = to_xr(add(add(theater_pos_, {0, settings_.screen_height, 0}),
                                        rotate(theater_rot_, {0, 0, -dist})));
            q.size = {width, width / aspect};
            layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
        }
        return;
    }

    // Immersive: a quad covering exactly the game's FOV, in front of the head
    // position, oriented like the game camera that rendered the frame.
    const float d = settings_.immersive_distance > 0.3f ? settings_.immersive_distance : 0.3f;
    float fov = settings_.game_fov;
    if (fov < 30.0f) fov = 30.0f;
    if (fov > 170.0f) fov = 170.0f;
    const float width = 2.0f * d * std::tan(rad(fov) * 0.5f);
    for (int e = 0; e < eyes; ++e) {
        XrCompositionLayerQuad& q = quads_[e];
        q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.space = xr_.local_space();
        q.eyeVisibility = vis[e];
        q.subImage = sub[e];
        q.pose.orientation = to_xr(immersive_rot_);
        q.pose.position = to_xr(add(to_vec(head.position), rotate(immersive_rot_, {0, 0, -d})));
        q.size = {width, width / aspect};
        layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
    }
}

int App::run() {
    std::string err;
    if (!xr_.init(&err)) {
        VRL_LOG("compositor", "OpenXR init failed: %s", err.c_str());
        return 1;
    }
    Gpu& gpu = xr_.gpu();
    if (!gpu.external_memory_fd)
        VRL_LOG("compositor", "VK_KHR_external_memory_fd unavailable: games will use the slower cpu transport");
    if (!stereo_.init(&gpu)) {
        VRL_LOG("compositor", "failed to create the stereo pipeline");
        return 1;
    }
    if (!input_.init(xr_.instance(), xr_.session())) VRL_LOG("compositor", "controller input unavailable");

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

    auto server = std::make_unique<FrameServer>(&gpu);
    server_ = server.get();
    if (!server->listen(socket_name())) {
        VRL_LOG("compositor", "cannot listen on '%s' - is another vrlayer-compositor running?", socket_name().c_str());
        return 1;
    }
    server->control_handler = [this](const std::string& c) { return handle_command(c); };
    server->on_active_changed = [this](const ClientInfo* c) {
        load_profile(c ? c->appid : 0);
        have_output_ = false;
    };
    load_profile(0);
    VRL_LOG("compositor", "ready; start a game with ENABLE_VRLAYER=1 (Steam launch options: vrlayer-run %%command%%)");

    while (!quit_ && !g_quit) {
        bool xr_quit = false;
        xr_.poll_events(&xr_quit);
        if (xr_quit) break;
        server->poll(xr_.running() ? 0 : 50);
        if (!xr_.running()) continue;

        XrFrameState fs;
        if (!xr_.wait_frame(&fs) || !xr_.begin_frame()) continue;
        XrPosef head;
        bool head_ok = xr_.locate_head(fs.predictedDisplayTime, &head);
        if (head_ok) head_ = head;
        update_input(fs, head_ok, head_);
        server->poll(0);  // pick up the newest game frame as late as possible
        layers_.clear();
        if (fs.shouldRender && render_frame()) build_layers(head_);
        xr_.end_frame(fs.predictedDisplayTime, layers_);
    }

    VRL_LOG("compositor", "shutting down");
    xr_.request_exit();
    if (fence_pending_) vkWaitForFences(gpu.device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkDeviceWaitIdle(gpu.device);
    pad_.close();
    mouse_.close();
    server_ = nullptr;
    server.reset();
    stereo_.destroy();
    vkDestroyFence(gpu.device, fence_, nullptr);
    vkDestroyCommandPool(gpu.device, pool_, nullptr);
    input_.destroy();
    xr_.destroy();
    return 0;
}

}  // namespace vrl

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: vrlayer-compositor\n\n"
                   "Shows games running with the VRLayer Vulkan layer in VR via OpenXR.\n"
                   "Start games with ENABLE_VRLAYER=1 (Steam: vrlayer-run %%command%%).\n"
                   "Control it with vrlayer-ctl. Environment: VRLAYER_SOCKET, VRLAYER_DEBUG=1.\n");
            return 0;
        }
    }
    struct sigaction sa {};
    sa.sa_handler = [](int) { vrl::g_quit = 1; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
    vrl::App app;
    return app.run();
}
