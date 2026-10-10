// vrlayer-ctl: send a command to the running vrlayer-compositor.
//
//   vrlayer-ctl status
//   vrlayer-ctl recenter
//   vrlayer-ctl mode immersive
//   vrlayer-ctl set stereo.separation 0.02
//   vrlayer-ctl save

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "ipc.h"
#include "protocol.h"

using namespace vrl;

int main(int argc, char** argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fprintf(stderr,
                "usage: vrlayer-ctl <command> [args]\n\n"
                "  status                      what is running and how\n"
                "  recenter                    put the screen in front of you\n"
                "  mode theater|immersive|toggle\n"
                "  3d on|off|toggle            depth-based stereo 3D\n"
                "  set <section.key> <value>   change a setting live (see 'dump')\n"
                "  get <section.key>\n"
                "  dump                        all current settings as INI\n"
                "  save                        write current settings to the game's profile\n"
                "  reload                      re-read profile files\n"
                "  next                        show the next connected game\n"
                "  ping | quit\n");
        return argc < 2 ? 2 : 0;
    }
    std::string text;
    for (int i = 1; i < argc; ++i) text += (i > 1 ? " " : "") + std::string(argv[i]);

    int fd = ipc_connect(socket_name());
    if (fd < 0) {
        fprintf(stderr, "vrlayer-ctl: compositor is not running\n");
        return 1;
    }
    auto msg = make_msg<ControlMsg>(MsgType::Control);
    snprintf(msg.text, sizeof(msg.text), "%s", text.c_str());
    if (!ipc_send(fd, &msg, sizeof(msg))) {
        fprintf(stderr, "vrlayer-ctl: send failed\n");
        return 1;
    }
    alignas(8) char buf[kMaxMsgSize];
    ssize_t n = ipc_recv_timeout(fd, buf, sizeof(buf), nullptr, 3000);
    close(fd);
    if (n < ssize_t(sizeof(ControlReplyMsg))) {
        fprintf(stderr, "vrlayer-ctl: no reply\n");
        return 1;
    }
    auto* reply = reinterpret_cast<ControlReplyMsg*>(buf);
    reply->text[sizeof(reply->text) - 1] = 0;
    printf("%s\n", reply->text);
    return strncmp(reply->text, "error", 5) == 0 ? 1 : 0;
}
