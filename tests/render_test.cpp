// Renders the VR loading room, the hands/pointer overlay and the loading panel
// on the GPU (lavapipe works) and checks a few pixels. With VRLAYER_DUMP_DIR
// set, writes the images as PPM files for inspection.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "canvas.h"
#include "gpu.h"
#include "loading.h"
#include "scene.h"

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

static float half(uint16_t h) {
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 0x400), int(e) - 25);
    return (h & 0x8000) ? -v : v;
}

static void dump_rgba16f(const std::string& path, const uint16_t* px, uint32_t w, uint32_t h, bool alpha_bg) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; ++i) {
        float a = half(px[i * 4 + 3]);
        for (int c = 0; c < 3; ++c) {
            float v = half(px[i * 4 + c]);
            if (alpha_bg) v += (1.0f - a) * 0.15f;  // grey background behind transparency
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            fputc(int(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f), f);
        }
    }
    fclose(f);
}

int main() {
    Gpu gpu;
    std::string err;
    if (!Gpu::create_headless(&gpu, &err)) {
        fprintf(stderr, "SKIP: %s\n", err.c_str());
        return 77;
    }
    const char* dump = getenv("VRLAYER_DUMP_DIR");
    const uint32_t W = 256, H = 256;
    SceneRenderer scene;
    EXPECT(scene.init(&gpu, W, H), "scene init");

    // Eyes 1.6 m above the floor, looking slightly down, 100 deg FOV.
    SceneEye eyes[2];
    const float pitch = -0.25f;  // radians
    for (int e = 0; e < 2; ++e) {
        eyes[e] = SceneEye{{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)},
                           {e == 0 ? -0.032f : 0.032f, 0.0f, 0.0f},
                           -1.19f, 1.19f, 1.19f, -1.19f};
    }
    std::vector<ScenePrim> prims;
    ScenePrim ball{};
    ball.a[0] = 0.0f;
    ball.a[1] = -0.2f;
    ball.a[2] = -0.6f;
    ball.radius = 0.08f;
    ball.type = ScenePrim::Sphere;
    ball.color[0] = 0.9f;
    ball.color[1] = 0.6f;
    ball.color[2] = 0.45f;
    ball.color[3] = 1.0f;
    prims.push_back(ball);
    ScenePrim laser{};
    laser.a[0] = 0.15f;
    laser.a[1] = -0.3f;
    laser.a[2] = -0.4f;
    laser.radius = 0.004f;
    laser.b[0] = 0.4f;
    laser.b[1] = 0.1f;
    laser.b[2] = -3.0f;
    laser.type = ScenePrim::Glow;
    laser.color[0] = 0.3f;
    laser.color[1] = 0.7f;
    laser.color[2] = 1.0f;
    laser.color[3] = 0.9f;
    prims.push_back(laser);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = gpu.family;
    VkCommandPool pool;
    vkCreateCommandPool(gpu.device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(gpu.device, &ai, &cmd);
    GpuBuffer rb[2];
    for (auto& b : rb) b.create(gpu, VkDeviceSize(W) * 2 * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    scene.record(cmd, eyes, -1.6f, 0.0f, false, prims);
    scene.record(cmd, eyes, -1.6f, 0.0f, true, prims);
    for (int i = 0; i < 2; ++i) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W * 2, H, 1};
        vkCmdCopyImageToBuffer(cmd, scene.output(i == 1), VK_IMAGE_LAYOUT_GENERAL, rb[i].buffer, 1, &region);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(gpu.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(gpu.queue);

    auto at = [&](int i, uint32_t x, uint32_t y, int c) {
        return half(static_cast<const uint16_t*>(rb[i].mapped)[(size_t(y) * 2 * W + x) * 4 + c]);
    };
    // Environment: opaque everywhere, sky above the horizon darker-blue than the lit pad below.
    EXPECT(at(0, W / 2, 2, 3) == 1.0f && at(0, W / 2, H - 2, 3) == 1.0f, "environment must be opaque");
    EXPECT(at(0, W / 2, 5, 2) > at(0, W / 2, 5, 0), "sky should be bluish");
    // Overlay: transparent background, opaque sphere in the middle-low part of the view.
    EXPECT(at(1, 5, 5, 3) == 0.0f, "overlay background must be transparent");
    float best_alpha = 0;
    for (uint32_t y = H / 2; y < H; ++y) best_alpha = std::fmax(best_alpha, at(1, W / 2, y, 3));
    EXPECT(best_alpha > 0.99f, "sphere not visible in overlay");
    // Both eyes see the sphere at slightly different x (stereo).
    auto sphere_x = [&](uint32_t eye) {
        for (uint32_t x = 0; x < W; ++x)
            for (uint32_t y = H / 2; y < H; ++y)
                if (at(1, eye * W + x, y, 3) > 0.99f && at(1, eye * W + x, y, 0) > 0.3f) return int(x);
        return -1;
    };
    int lx = sphere_x(0), rx = sphere_x(1);
    EXPECT(lx > 0 && rx > 0 && lx > rx, "sphere parallax wrong (left %d right %d)", lx, rx);

    // Loading panel.
    Settings s;
    LoadingInputs in;
    in.game = true;
    in.game_name = "ExampleGame.exe";
    in.width = 2560;
    in.height = 1440;
    in.fps = 87;
    in.depth = true;
    in.has_analysis = true;
    in.analysis.flags = kAnalysisProjection | kAnalysisView | kAnalysisViewProj | kAnalysisDepth;
    in.analysis.hfov_deg = 103;
    in.analysis.vfov_deg = 70.5f;
    in.analysis.near_d = 0.1f;
    in.analysis.reversed = 1;
    in.uinput = true;
    in.calibration = MouseCalibrator::State::Returning;
    in.calibration_message = "hold still: measuring mouse look";
    in.controllers = true;
    in.elapsed = 14;
    Canvas c(kPanelWidth, kPanelHeight);
    draw_loading_panel(c, build_loading_view(in, s, -1), 0, -1);
    EXPECT(c.data()[(size_t(630) * kPanelWidth + 300) * 4 + 2] > 150, "start button not drawn");

    if (dump) {
        std::string d = dump;
        dump_rgba16f(d + "/room.ppm", static_cast<const uint16_t*>(rb[0].mapped), W * 2, H, false);
        dump_rgba16f(d + "/overlay.ppm", static_cast<const uint16_t*>(rb[1].mapped), W * 2, H, true);
        c.save_ppm(d + "/panel.ppm");
        fprintf(stderr, "images written to %s\n", dump);
    }

    for (auto& b : rb) b.destroy(gpu);
    vkDestroyCommandPool(gpu.device, pool, nullptr);
    scene.destroy();
    fprintf(stderr, "%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
