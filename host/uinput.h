// Virtual input devices via /dev/uinput: a relative mouse for head look and
// an Xbox 360-style pad for the VR controllers. Both are seen by the game
// (and by Proton / SDL / Steam Input) like real hardware.
//
// Needs write access to /dev/uinput; see packaging/70-vrlayer-uinput.rules.
#pragma once

namespace vrl {

class UinputMouse {
public:
    ~UinputMouse();
    bool open();
    bool is_open() const { return fd_ >= 0; }
    void move(int dx, int dy);
    void button(int code, bool down);  // BTN_LEFT / BTN_RIGHT / ...
    void close();

private:
    int fd_ = -1;
};

// Keyboard for the keyboard_mouse control scheme.
class UinputKeyboard {
public:
    ~UinputKeyboard();
    bool open();
    bool is_open() const { return fd_ >= 0; }
    void key(int code, bool down);
    void close();  // releases held keys

private:
    int fd_ = -1;
    bool held_[256] = {};
};

// Absolute pointer (like a tablet) for pointing at the virtual screen with a
// controller or hand. Coordinates are normalized 0..1 over the desktop.
class UinputPointer {
public:
    ~UinputPointer();
    bool open();
    bool is_open() const { return fd_ >= 0; }
    void move_to(float u, float v);
    void button(int code, bool down);
    void close();

private:
    int fd_ = -1;
    int last_x_ = -1, last_y_ = -1;
};

struct GamepadState {
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false;
    bool back = false, start = false, guide = false;
    bool lstick = false, rstick = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, +y = up
    float lt = 0, rt = 0;                  // 0..1
    int dpad_x = 0, dpad_y = 0;            // -1..1, +y = up

    bool operator==(const GamepadState& o) const;
    bool operator!=(const GamepadState& o) const { return !(*this == o); }
};

class UinputGamepad {
public:
    ~UinputGamepad();
    bool open();
    bool is_open() const { return fd_ >= 0; }
    // Emits only what changed since the last call.
    void update(const GamepadState& s);
    void close();

private:
    int fd_ = -1;
    GamepadState last_;
};

}  // namespace vrl
