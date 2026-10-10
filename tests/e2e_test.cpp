// End-to-end test: runs vrlayer_test_game with the capture layer enabled,
// receives its frames through FrameServer (exactly like the compositor does),
// and checks color, depth and the synthesized stereo pair.
//
// usage: vrlayer_e2e_test <test_game> <data dir containing vulkan/implicit_layer.d> gpu|cpu renderpass|dynamic

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "frame_server.h"
#include "gpu.h"
#include "stereo.h"

using namespace vrl;

static int g_failures = 0;
#define EXPECT(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                \
            fprintf(stderr, "\n");                       \
            ++g_failures;                                \
        }                                                \
    } while (0)

static double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(float(mant), -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    return sign ? -v : v;
}

static pid_t spawn_game(const char* game, const char* api, int* out_fd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        execl(game, game, api, nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    *out_fd = pipefd[0];
    return pid;
}

static void drain(int fd, std::string* out) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out->append(buf, size_t(n));
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <test_game> <data dir> gpu|cpu renderpass|dynamic\n", argv[0]);
        return 2;
    }
    const char* game = argv[1];
    const std::string transport = argv[3];

    Gpu gpu;
    std::string err;
    if (!Gpu::create_headless(&gpu, &err)) {
        fprintf(stderr, "SKIP: no Vulkan device (%s)\n", err.c_str());
        return 77;
    }

    std::string sock = "vrlayer-test-" + std::to_string(getpid());
    FrameServer server(&gpu);
    if (!server.listen(sock)) {
        fprintf(stderr, "cannot listen on %s\n", sock.c_str());
        return 1;
    }
    StereoRenderer stereo;
    if (!stereo.init(&gpu)) {
        fprintf(stderr, "stereo init failed\n");
        return 1;
    }

    setenv("ENABLE_VRLAYER", "1", 1);
    // Older loaders only look in the XDG data dirs; newer ones also honour
    // VK_ADD_IMPLICIT_LAYER_PATH.
    setenv("XDG_DATA_HOME", argv[2], 1);
    setenv("VK_ADD_IMPLICIT_LAYER_PATH", (std::string(argv[2]) + "/vulkan/implicit_layer.d").c_str(), 1);
    setenv("VRLAYER_SOCKET", sock.c_str(), 1);
    setenv("VRLAYER_TRANSPORT", transport.c_str(), 1);
    setenv("VRLAYER_DEPTH_MIN_DRAWS", "1", 1);
    setenv("SteamAppId", "4242", 1);
    // Validate the layer's own Vulkan usage when the validation layer exists.
    bool validate = access("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json", R_OK) == 0;
    if (validate) setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
    int game_out = -1;
    pid_t child = spawn_game(game, argv[4], &game_out);
    unsetenv("VK_INSTANCE_LAYERS");
    std::string game_log;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gpu.family;
    VkCommandPool pool;
    vkCreateCommandPool(gpu.device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(gpu.device, &ai, &cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(gpu.device, &fci, nullptr, &fence);

    GpuBuffer color_rb, depth_rb, stereo_rb;
    int frames = 0, depth_frames = 0;
    bool verified = false;
    const double deadline = now_s() + 30.0;

    while (now_s() < deadline && !verified) {
        drain(game_out, &game_log);
        server.poll(20);
        if (!server.has_new_frame()) {
            int status;
            if (waitpid(child, &status, WNOHANG) == child) {
                child = -1;
                fprintf(stderr, "test game exited early (status %d)\n", status);
                break;
            }
            continue;
        }
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        FrameView f;
        if (!server.begin_frame(cmd, &f)) {
            vkEndCommandBuffer(cmd);
            continue;
        }
        ++frames;
        if (frames == 1) {
            const ClientInfo* ci = server.active();
            EXPECT(ci && ci->appid == 4242, "appid not forwarded");
            EXPECT(ci && ci->exe == "vrlayer_test_game", "exe name '%s'", ci ? ci->exe.c_str() : "");
            EXPECT(ci && ci->transport == (transport == "cpu" ? Transport::Cpu : Transport::Gpu),
                   "unexpected transport");
        }
        bool check = f.has_depth;
        if (check) {
            ++depth_frames;
            if (!color_rb.buffer) {
                color_rb.create(gpu, VkDeviceSize(f.width) * f.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                depth_rb.create(gpu, f.depth_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                stereo_rb.create(gpu, VkDeviceSize(f.width) * 2 * f.height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            }
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {f.width, f.height, 1};
            vkCmdCopyImageToBuffer(cmd, f.image, VK_IMAGE_LAYOUT_GENERAL, color_rb.buffer, 1, &region);
            VkBufferCopy bc{0, 0, f.depth_size};
            vkCmdCopyBuffer(cmd, f.depth, depth_rb.buffer, 1, &bc);

            StereoParams p;
            p.separation = 0.05f;
            p.convergence = 0.001f;
            p.popout = 0.5f;
            p.far_plane = 1000.0f;
            stereo.record(cmd, f, p);
            region.imageExtent = {stereo.output_width(), stereo.output_height(), 1};
            vkCmdCopyImageToBuffer(cmd, stereo.output(), VK_IMAGE_LAYOUT_GENERAL, stereo_rb.buffer, 1, &region);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                                 nullptr, 0, nullptr);
        }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(gpu.queue, 1, &si, fence);
        vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(gpu.device, 1, &fence);
        server.gpu_work_done();
        if (!check) continue;

        // --- verify the captured frame
        const uint32_t W = f.width, H = f.height;
        EXPECT(W == 160 && H == 120, "frame size %ux%u", W, H);
        EXPECT(f.format == VK_FORMAT_B8G8R8A8_UNORM, "format %d", f.format);
        auto* px = static_cast<const uint8_t*>(color_rb.mapped);
        const uint8_t* l = px + (H / 2 * W + W / 4) * 4;
        const uint8_t* r = px + (H / 2 * W + 3 * W / 4) * 4;
        EXPECT(l[2] == 255 && l[0] == 0, "left pixel should be red, got BGRA %u,%u,%u,%u", l[0], l[1], l[2], l[3]);
        EXPECT(r[0] == 255 && r[2] == 0, "right pixel should be blue, got BGRA %u,%u,%u,%u", r[0], r[1], r[2], r[3]);

        EXPECT(f.depth_format == DepthFormat::D32F, "depth format %u", unsigned(f.depth_format));
        auto* dz = static_cast<const float*>(depth_rb.mapped);
        float dl = dz[H / 2 * f.depth_width + f.depth_width / 4];
        float dr = dz[H / 2 * f.depth_width + 3 * f.depth_width / 4];
        EXPECT(std::fabs(dl - 0.25f) < 1e-3f, "left depth %f, expected 0.25", dl);
        EXPECT(std::fabs(dr - 0.6f) < 1e-3f, "right depth %f, expected 0.6", dr);

        // --- verify the stereo pair: red/blue boundary must sit at different
        // x positions in the two eyes (parallax), and colors must survive.
        EXPECT(stereo.output_is_stereo() && stereo.output_width() == 2 * W, "stereo output %ux%u",
               stereo.output_width(), stereo.output_height());
        auto* sp = static_cast<const uint16_t*>(stereo_rb.mapped);
        auto boundary = [&](uint32_t eye) {
            for (uint32_t x = 0; x < W; ++x) {
                const uint16_t* p = sp + (size_t(H / 2) * (2 * W) + eye * W + x) * 4;
                if (half_to_float(p[2]) > half_to_float(p[0])) return int(x);  // blue wins
            }
            return -1;
        };
        int bl = boundary(0), br = boundary(1);
        const uint16_t* sl = sp + (size_t(H / 2) * (2 * W) + 10) * 4;
        EXPECT(half_to_float(sl[0]) > 0.9f && half_to_float(sl[2]) < 0.1f, "stereo left eye lost red");
        // The near (red) half pops out: it extends right in the left eye and
        // left in the right eye; the disoccluded gap is filled with background.
        EXPECT(bl > int(W / 2) && br > 0 && br < int(W / 2), "wrong parallax at depth edge (left eye %d, right eye %d)",
               bl, br);
        fprintf(stderr, "frame %d: color/depth ok, stereo boundary L=%d R=%d (mono %u)\n", frames, bl, br, W / 2);
        verified = true;
    }

    EXPECT(verified, "never received a frame with depth (%d frames total)", frames);

    if (child > 0) {
        kill(child, SIGTERM);
        // Keep serving while the game shuts down so it never blocks on us.
        int status = 0;
        double end = now_s() + 10.0;
        while (now_s() < end) {
            server.poll(10);
            if (server.has_new_frame()) {
                vkResetCommandBuffer(cmd, 0);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                vkBeginCommandBuffer(cmd, &bi);
                FrameView f;
                server.begin_frame(cmd, &f);
                vkEndCommandBuffer(cmd);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;
                vkQueueSubmit(gpu.queue, 1, &si, fence);
                vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkResetFences(gpu.device, 1, &fence);
                server.gpu_work_done();
            }
            if (waitpid(child, &status, WNOHANG) == child) {
                child = -1;
                break;
            }
        }
        if (child > 0) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            EXPECT(false, "test game did not exit on SIGTERM");
        } else {
            EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "test game exit status %d", status);
        }
    }
    drain(game_out, &game_log);
    if (game_log.find("Validation Error") != std::string::npos) {
        EXPECT(false, "Vulkan validation errors in the game process");
    }
    if (g_failures || getenv("VRLAYER_TEST_VERBOSE")) fprintf(stderr, "---- game output ----\n%s", game_log.c_str());

    color_rb.destroy(gpu);
    depth_rb.destroy(gpu);
    stereo_rb.destroy(gpu);
    stereo.destroy();
    vkDestroyFence(gpu.device, fence, nullptr);
    vkDestroyCommandPool(gpu.device, pool, nullptr);
    fprintf(stderr, "%s: %d frames received, %d with depth (validation %s)\n", g_failures ? "FAILED" : "PASSED",
            frames, depth_frames, validate ? "on" : "off");
    return g_failures ? 1 : 0;
}
