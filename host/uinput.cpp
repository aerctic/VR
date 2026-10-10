#include "uinput.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <initializer_list>

#include "log.h"

namespace vrl {

static void emit(int fd, int type, int code, int value) {
    input_event ev{};
    ev.type = static_cast<__u16>(type);
    ev.code = static_cast<__u16>(code);
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        // Dropped input is not fatal.
    }
}

static int open_uinput() {
    // Callers retry every frame while the device is wanted; keep that cheap
    // and quiet when /dev/uinput is not accessible.
    static timespec last_fail{};
    static bool logged = false;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last_fail.tv_sec && now.tv_sec - last_fail.tv_sec < 5) return -1;
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        last_fail = now;
        if (!logged)
            VRL_LOG("input", "cannot open /dev/uinput (%s): head mouse and gamepad disabled; "
                    "install packaging/70-vrlayer-uinput.rules", strerror(errno));
        logged = true;
    }
    return fd;
}

static bool create_device(int fd, const char* name, uint16_t vendor, uint16_t product, uint16_t version) {
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = vendor;
    setup.id.product = product;
    setup.id.version = version;
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    return ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
}

// ---------------------------------------------------------------------------

UinputMouse::~UinputMouse() { close(); }

bool UinputMouse::open() {
    if (fd_ >= 0) return true;
    int fd = open_uinput();
    if (fd < 0) return false;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);  // needed for the device to be classified as a mouse
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    if (!create_device(fd, "VRLayer Head Mouse", 0x1209, 0x5652, 1)) {
        VRL_LOG("input", "failed to create virtual mouse");
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

void UinputMouse::move(int dx, int dy) {
    if (fd_ < 0 || (dx == 0 && dy == 0)) return;
    if (dx) emit(fd_, EV_REL, REL_X, dx);
    if (dy) emit(fd_, EV_REL, REL_Y, dy);
    emit(fd_, EV_SYN, SYN_REPORT, 0);
}

void UinputMouse::close() {
    if (fd_ < 0) return;
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
    fd_ = -1;
}

// ---------------------------------------------------------------------------

bool GamepadState::operator==(const GamepadState& o) const {
    return a == o.a && b == o.b && x == o.x && y == o.y && lb == o.lb && rb == o.rb && back == o.back &&
           start == o.start && guide == o.guide && lstick == o.lstick && rstick == o.rstick && lx == o.lx &&
           ly == o.ly && rx == o.rx && ry == o.ry && lt == o.lt && rt == o.rt && dpad_x == o.dpad_x &&
           dpad_y == o.dpad_y;
}

UinputGamepad::~UinputGamepad() { close(); }

static void abs_axis(int fd, int code, int min, int max, int fuzz, int flat) {
    uinput_abs_setup a{};
    a.code = static_cast<__u16>(code);
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.fuzz = fuzz;
    a.absinfo.flat = flat;
    ioctl(fd, UI_ABS_SETUP, &a);
}

bool UinputGamepad::open() {
    if (fd_ >= 0) return true;
    int fd = open_uinput();
    if (fd < 0) return false;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int k : {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR})
        ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int a : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y}) ioctl(fd, UI_SET_ABSBIT, a);
    abs_axis(fd, ABS_X, -32768, 32767, 16, 128);
    abs_axis(fd, ABS_Y, -32768, 32767, 16, 128);
    abs_axis(fd, ABS_RX, -32768, 32767, 16, 128);
    abs_axis(fd, ABS_RY, -32768, 32767, 16, 128);
    abs_axis(fd, ABS_Z, 0, 255, 0, 0);
    abs_axis(fd, ABS_RZ, 0, 255, 0, 0);
    abs_axis(fd, ABS_HAT0X, -1, 1, 0, 0);
    abs_axis(fd, ABS_HAT0Y, -1, 1, 0, 0);
    // Xbox 360 pad IDs so SDL, Proton and games pick the standard mapping.
    if (!create_device(fd, "Microsoft X-Box 360 pad (VRLayer)", 0x045e, 0x028e, 0x0114)) {
        VRL_LOG("input", "failed to create virtual gamepad");
        ::close(fd);
        return false;
    }
    fd_ = fd;
    last_ = GamepadState{};
    return true;
}

static int stick(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return int(std::lround(v * 32767.0f));
}

static int trigger(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    return int(std::lround(v * 255.0f));
}

void UinputGamepad::update(const GamepadState& s) {
    if (fd_ < 0 || s == last_) return;
    auto key = [&](int code, bool now, bool before) {
        if (now != before) emit(fd_, EV_KEY, code, now ? 1 : 0);
    };
    key(BTN_A, s.a, last_.a);
    key(BTN_B, s.b, last_.b);
    key(BTN_X, s.x, last_.x);
    key(BTN_Y, s.y, last_.y);
    key(BTN_TL, s.lb, last_.lb);
    key(BTN_TR, s.rb, last_.rb);
    key(BTN_SELECT, s.back, last_.back);
    key(BTN_START, s.start, last_.start);
    key(BTN_MODE, s.guide, last_.guide);
    key(BTN_THUMBL, s.lstick, last_.lstick);
    key(BTN_THUMBR, s.rstick, last_.rstick);
    // Linux joystick convention: +Y is down.
    if (s.lx != last_.lx) emit(fd_, EV_ABS, ABS_X, stick(s.lx));
    if (s.ly != last_.ly) emit(fd_, EV_ABS, ABS_Y, stick(-s.ly));
    if (s.rx != last_.rx) emit(fd_, EV_ABS, ABS_RX, stick(s.rx));
    if (s.ry != last_.ry) emit(fd_, EV_ABS, ABS_RY, stick(-s.ry));
    if (s.lt != last_.lt) emit(fd_, EV_ABS, ABS_Z, trigger(s.lt));
    if (s.rt != last_.rt) emit(fd_, EV_ABS, ABS_RZ, trigger(s.rt));
    if (s.dpad_x != last_.dpad_x) emit(fd_, EV_ABS, ABS_HAT0X, s.dpad_x);
    if (s.dpad_y != last_.dpad_y) emit(fd_, EV_ABS, ABS_HAT0Y, -s.dpad_y);
    emit(fd_, EV_SYN, SYN_REPORT, 0);
    last_ = s;
}

void UinputGamepad::close() {
    if (fd_ < 0) return;
    // Release everything so the game does not see stuck buttons.
    update(GamepadState{});
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
    fd_ = -1;
}

}  // namespace vrl
