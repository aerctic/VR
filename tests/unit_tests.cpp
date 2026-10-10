// Unit tests for the pieces that do not need a GPU or a headset.

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
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
    static_assert(sizeof(FrameMsg) == 16 + 8 + 16 + 8, "frame");
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
    EXPECT(paths.size() == 2 && paths[1].find("games/1234.ini") != std::string::npos);
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
    test_head_mouse();
    test_math();
    test_ipc();
    fprintf(stderr, "%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
