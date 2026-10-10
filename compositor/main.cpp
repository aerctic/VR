// vrlayer-compositor: OpenXR application that turns games captured by the
// VRLayer Vulkan layer into a VR experience.
//
// When a game starts you are in the VR loading room: a status panel shows
// what the layer is learning about the game (video, depth buffer, camera
// lens, camera position, mouse-look calibration, hands and controllers) and
// recommends a mode. Then:
//
//  vr        - your head pose is written into the game's own camera (6DOF),
//              frames are shown as projection layers from the pose they were
//              rendered with; stereo from depth or alternate-eye rendering.
//  immersive - the game fills your field of view at its own FOV; head
//              rotation turns the game camera through a virtual mouse.
//  theater   - the game on a big (curved) screen in a virtual room, in 3D
//              reconstructed from the depth buffer. Works with every game.

#include <signal.h>

#include <cstdio>
#include <cstring>

#include "app.h"

static vrl::App* g_app = nullptr;

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: vrlayer-compositor\n\n"
                   "Turns games running with the VRLayer Vulkan layer into VR via OpenXR.\n"
                   "Start games with ENABLE_VRLAYER=1 (Steam: vrlayer-run %%command%%).\n"
                   "Control it with vrlayer-ctl. Environment: VRLAYER_SOCKET, VRLAYER_DEBUG=1.\n");
            return 0;
        }
    }
    static vrl::App app;
    g_app = &app;
    struct sigaction sa {};
    sa.sa_handler = [](int) {
        if (g_app) g_app->request_quit();
    };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
    return app.run();
}
