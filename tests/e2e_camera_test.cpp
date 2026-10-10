// End-to-end camera test: vrlayer_test_game in "camera" mode uploads its
// camera matrices to a host-mapped ring buffer like a DXVK game does. We check
// that the layer finds the projection and view matrices, reports the game's
// camera, and that a head pose sent by the compositor really changes what the
// GPU renders (the game encodes the yaw its shader saw in the green channel).
//
// With "menu" the game first shows a main menu with its own 3D camera; the
// layer must move on to the gameplay camera once the level is loaded.
//
// usage: vrlayer_e2e_camera_test <test_game> <data dir> gpu|cpu [menu]

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "frame_server.h"
#include "gpu.h"

using namespace vrl;

static int g_failures = 0;
#define EXPECT(cond, ...)                                        \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

static double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

struct Harness {
    Gpu gpu;
    FrameServer* server = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    GpuBuffer readback;
    pid_t child = -1;
    int out_fd = -1;
    std::string log;

    void drain() {
        char buf[4096];
        ssize_t n;
        while ((n = read(out_fd, buf, sizeof(buf))) > 0) log.append(buf, size_t(n));
    }

    // Pumps frames until pred(frame, green byte of the right half) is true.
    bool pump(double seconds, const std::function<bool(const FrameView&, int)>& pred) {
        double end = now_s() + seconds;
        while (now_s() < end) {
            drain();
            server->poll(10);
            if (!server->has_new_frame()) continue;
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkBeginCommandBuffer(cmd, &bi);
            FrameView f;
            if (!server->begin_frame(cmd, &f)) {
                vkEndCommandBuffer(cmd);
                continue;
            }
            if (!readback.buffer)
                readback.create(gpu, VkDeviceSize(f.width) * f.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {f.width, f.height, 1};
            vkCmdCopyImageToBuffer(cmd, f.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                                 0, nullptr);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(gpu.queue, 1, &si, fence);
            vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(gpu.device, 1, &fence);
            server->gpu_work_done();
            const uint8_t* px =
                static_cast<const uint8_t*>(readback.mapped) + (size_t(f.height / 2) * f.width + 3 * f.width / 4) * 4;
            if (pred(f, px[1])) return true;
        }
        return false;
    }

    const AnalysisMsg* analysis() const {
        const ClientInfo* c = server->active();
        return c && c->has_analysis ? &c->analysis : nullptr;
    }
};

static CameraMsg camera_msg(CameraMode mode, uint32_t seq, float yaw_deg) {
    auto m = make_msg<CameraMsg>(MsgType::Camera);
    m.mode = uint32_t(mode);
    m.seq = seq;
    float h = yaw_deg * 3.14159265f / 180.0f * 0.5f;
    m.rotation[1] = std::sin(h);
    m.rotation[3] = std::cos(h);
    m.units_per_metre = 1.0f;
    m.ipd = 0.064f;
    return m;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <test_game> <data dir> gpu|cpu\n", argv[0]);
        return 2;
    }
    const bool menu = argc > 4 && !strcmp(argv[4], "menu");
    Harness h;
    std::string err;
    if (!Gpu::create_headless(&h.gpu, &err)) {
        fprintf(stderr, "SKIP: no Vulkan device (%s)\n", err.c_str());
        return 77;
    }
    std::string sock = "vrlayer-camtest-" + std::to_string(getpid());
    FrameServer server(&h.gpu);
    h.server = &server;
    if (!server.listen(sock)) return 1;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = h.gpu.family;
    vkCreateCommandPool(h.gpu.device, &pci, nullptr, &h.pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = h.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(h.gpu.device, &ai, &h.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(h.gpu.device, &fci, nullptr, &h.fence);

    setenv("ENABLE_VRLAYER", "1", 1);
    setenv("XDG_DATA_HOME", argv[2], 1);
    setenv("VK_ADD_IMPLICIT_LAYER_PATH", (std::string(argv[2]) + "/vulkan/implicit_layer.d").c_str(), 1);
    setenv("VRLAYER_SOCKET", sock.c_str(), 1);
    setenv("VRLAYER_TRANSPORT", argv[3], 1);
    setenv("VRLAYER_DEPTH_MIN_DRAWS", "1", 1);
    bool validate = access("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json", R_OK) == 0;
    if (validate) setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    h.child = fork();
    if (h.child == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        execl(argv[1], argv[1], "renderpass", menu ? "menu" : "camera", nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    h.out_fd = pipefd[0];
    unsetenv("VK_INSTANCE_LAYERS");

    const double cos20 = std::cos(20.0 * 3.14159265 / 180.0);
    const int green_rest = int(std::lround((0.5 + 0.5 * cos20) * 255.0));  // game's own camera
    const int green_90 = 128;                                                // after +70 deg injection
    auto near_green = [](int g, int want) { return std::abs(g - want) <= 2; };

    // 0. Main menu: its camera is found first, then replaced by the gameplay
    //    camera when the level loads (its copies stay behind in memory).
    uint32_t menu_id = 0;
    if (menu) {
        bool menu_cam = h.pump(10, [&](const FrameView&, int) {
            const AnalysisMsg* a = h.analysis();
            return a && (a->flags & kAnalysisProjection) && std::fabs(a->vfov_deg - 50.0f) < 0.1f;
        });
        EXPECT(menu_cam, "menu camera not seen first");
        if (h.analysis()) menu_id = h.analysis()->camera_id;
        EXPECT(menu_id != 0, "no camera id");
    }

    // 1. The projection and the view matrix are found.
    bool found = h.pump(20, [&](const FrameView&, int) {
        const AnalysisMsg* a = h.analysis();
        return a && (a->flags & kAnalysisProjection) && (a->flags & kAnalysisView) && std::fabs(a->vfov_deg - 60.0f) < 0.1f;
    });
    if (menu) {
        EXPECT(found, "stuck on the main menu camera (status: %s)", h.analysis() ? h.analysis()->status : "-");
        EXPECT(h.analysis() && h.analysis()->camera_id != menu_id, "camera id did not change");
    }
    EXPECT(found, "camera not found (status: %s)", h.analysis() ? h.analysis()->status : "no analysis");
    if (const AnalysisMsg* a = h.analysis()) {
        EXPECT(std::fabs(a->vfov_deg - 60.0f) < 0.01f, "vfov %f", a->vfov_deg);
        EXPECT(std::fabs(a->aspect - 4.0f / 3.0f) < 1e-3f, "aspect %f", a->aspect);
        EXPECT(std::fabs(a->near_d - 0.1f) < 1e-4f && std::fabs(a->far_d - 100.0f) < 0.05f, "near %f far %f",
               a->near_d, a->far_d);
        EXPECT(!a->reversed, "reported reversed-z");
        EXPECT(a->flags & kAnalysisViewProj, "view-projection not found");
        EXPECT(a->flags & kAnalysisDepth, "depth not reported");
        EXPECT(a->basis_up == 1.0f && a->basis_fwd == -1.0f, "basis up %f fwd %f", a->basis_up, a->basis_fwd);
        fprintf(stderr, "analysis: %s (scanned %u MB)\n", a->status, a->scanned_mb);
    }

    // 2. The game's camera is reported with every frame.
    FrameView seen;
    bool cam = h.pump(10, [&](const FrameView& f, int g) {
        seen = f;
        return f.camera_valid && near_green(g, green_rest);
    });
    EXPECT(cam, "no camera in frames / unexpected green");
    if (cam) {
        EXPECT(std::fabs(seen.cam_rot[0] - cos20) < 1e-4 && std::fabs(seen.cam_rot[2] - std::sin(20 * 3.14159265 / 180)) < 1e-4,
               "camera rotation %f %f", seen.cam_rot[0], seen.cam_rot[2]);
        EXPECT(std::fabs(seen.cam_pos[0] - 1) < 1e-4 && std::fabs(seen.cam_pos[1] - 2) < 1e-4 &&
                   std::fabs(seen.cam_pos[2] - 3) < 1e-4,
               "camera position %f %f %f", seen.cam_pos[0], seen.cam_pos[1], seen.cam_pos[2]);
    }

    // 3. Inject a +70 deg head yaw: the GPU must render a 90 deg camera, while
    //    the reported camera stays the game's own.
    server.send_camera(camera_msg(CameraMode::Head, 7, 70.0f));
    int green = -1;
    bool injected = h.pump(10, [&](const FrameView& f, int g) {
        seen = f;
        green = g;
        return f.pose_seq == 7 && near_green(g, green_90);
    });
    EXPECT(injected, "injection not visible in the rendered frame (green %d, want %d)", green, green_90);
    EXPECT(std::fabs(seen.cam_rot[0] - cos20) < 1e-4, "reported camera should stay the game's own");
    h.pump(2, [&](const FrameView&, int) {
        const AnalysisMsg* a = h.analysis();
        return a && (a->flags & kAnalysisInjecting) && a->patched > 0;
    });
    EXPECT(h.analysis() && (h.analysis()->flags & kAnalysisInjecting), "analysis does not report injection");

    // 4. Alternate-eye rendering tags frames left / right.
    server.send_camera(camera_msg(CameraMode::Alternate, 8, 70.0f));
    bool left = false, right = false;
    h.pump(10, [&](const FrameView& f, int) {
        if (f.pose_seq == 8 && f.eye == 1) left = true;
        if (f.pose_seq == 8 && f.eye == 2) right = true;
        return left && right;
    });
    EXPECT(left && right, "alternate eyes not reported (left %d right %d)", left, right);

    // 5. Off again: the game's own camera comes back.
    server.send_camera(camera_msg(CameraMode::Off, 9, 0.0f));
    bool restored = h.pump(10, [&](const FrameView& f, int g) {
        green = g;
        return f.pose_seq == 0 && near_green(g, green_rest);
    });
    EXPECT(restored, "camera not restored (green %d)", green);

    // 6. Restart the analysis: the camera is found again.
    server.restart_analysis();
    bool again = h.pump(20, [&](const FrameView&, int) {
        const AnalysisMsg* a = h.analysis();
        return a && (a->flags & kAnalysisView);
    });
    EXPECT(again, "camera not found again after restart");

    kill(h.child, SIGTERM);
    int status = 0;
    double end = now_s() + 10;
    bool exited = false;
    while (now_s() < end) {
        h.pump(0.05, [](const FrameView&, int) { return false; });
        if (waitpid(h.child, &status, WNOHANG) == h.child) {
            exited = true;
            break;
        }
    }
    if (!exited) {
        kill(h.child, SIGKILL);
        waitpid(h.child, &status, 0);
    }
    EXPECT(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0, "game did not exit cleanly (%d)", status);
    h.drain();
    EXPECT(h.log.find("Validation Error") == std::string::npos, "Vulkan validation errors in the game");
    if (g_failures || getenv("VRLAYER_TEST_VERBOSE")) fprintf(stderr, "---- game output ----\n%s", h.log.c_str());

    h.readback.destroy(h.gpu);
    vkDestroyFence(h.gpu.device, h.fence, nullptr);
    vkDestroyCommandPool(h.gpu.device, h.pool, nullptr);
    fprintf(stderr, "%s (validation %s)\n", g_failures ? "FAILED" : "PASSED", validate ? "on" : "off");
    return g_failures ? 1 : 0;
}
