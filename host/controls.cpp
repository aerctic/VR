#include "controls.h"

#include <linux/input-event-codes.h>

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace vrl {

int key_code(const std::string& raw) {
    std::string n;
    for (char c : raw) n += char(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') {
        static const int letters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
                                        KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
                                        KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        return letters[n[0] - 'a'];
    }
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return n[0] == '0' ? KEY_0 : KEY_1 + (n[0] - '1');
    if (n.size() >= 2 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 10) return KEY_F1 + f - 1;
        if (f == 11) return KEY_F11;
        if (f == 12) return KEY_F12;
    }
    static const std::map<std::string, int> named = {
        {"space", KEY_SPACE},           {"enter", KEY_ENTER},          {"esc", KEY_ESC},
        {"escape", KEY_ESC},            {"tab", KEY_TAB},              {"backspace", KEY_BACKSPACE},
        {"shift", KEY_LEFTSHIFT},       {"left_shift", KEY_LEFTSHIFT}, {"right_shift", KEY_RIGHTSHIFT},
        {"ctrl", KEY_LEFTCTRL},         {"left_ctrl", KEY_LEFTCTRL},   {"right_ctrl", KEY_RIGHTCTRL},
        {"alt", KEY_LEFTALT},           {"left_alt", KEY_LEFTALT},     {"right_alt", KEY_RIGHTALT},
        {"up", KEY_UP},                 {"down", KEY_DOWN},            {"left", KEY_LEFT},
        {"right", KEY_RIGHT},           {"capslock", KEY_CAPSLOCK},    {"grave", KEY_GRAVE},
        {"minus", KEY_MINUS},           {"equal", KEY_EQUAL},          {"comma", KEY_COMMA},
        {"dot", KEY_DOT},               {"slash", KEY_SLASH},          {"semicolon", KEY_SEMICOLON},
        {"insert", KEY_INSERT},         {"delete", KEY_DELETE},        {"home", KEY_HOME},
        {"end", KEY_END},               {"pageup", KEY_PAGEUP},        {"pagedown", KEY_PAGEDOWN},
        {"mouse_left", BTN_LEFT},       {"mouse_right", BTN_RIGHT},    {"mouse_middle", BTN_MIDDLE},
        {"mouse_back", BTN_SIDE},       {"mouse_forward", BTN_EXTRA},  {"none", 0},
    };
    auto it = named.find(n);
    return it == named.end() ? -1 : it->second;
}

KeyboardMouseMapper::KeyboardMouseMapper() {
    bindings_ = {
        {"move_forward", KEY_W}, {"move_back", KEY_S}, {"move_left", KEY_A}, {"move_right", KEY_D},
        {"a", KEY_SPACE},        {"b", KEY_C},         {"x", KEY_R},         {"y", KEY_E},
        {"lb", KEY_Q},           {"rb", KEY_F},        {"lt", BTN_RIGHT},    {"rt", BTN_LEFT},
        {"lstick", KEY_LEFTSHIFT}, {"rstick", KEY_V},  {"start", KEY_ESC},   {"back", KEY_TAB},
        {"dpad_up", KEY_1},      {"dpad_right", KEY_2}, {"dpad_down", KEY_3}, {"dpad_left", KEY_4},
    };
}

bool KeyboardMouseMapper::bind(const std::string& button, const std::string& key, std::string* error) {
    if (!bindings_.count(button)) {
        if (error) *error = "unknown button '" + button + "'";
        return false;
    }
    int code = key_code(key);
    if (code < 0) {
        if (error) *error = "unknown key '" + key + "'";
        return false;
    }
    bindings_[button] = code;
    return true;
}

KeyboardMouseMapper::Output KeyboardMouseMapper::update(const GamepadState& s, float dt, float turn_speed,
                                                        float snap_deg) {
    Output o;
    auto hold = [&](const char* button, bool down) {
        int code = bindings_[button];
        if (down && code > 0) o.pressed.insert(code);
    };
    const float dz = 0.4f;
    hold("move_forward", s.ly > dz);
    hold("move_back", s.ly < -dz);
    hold("move_left", s.lx < -dz);
    hold("move_right", s.lx > dz);
    hold("a", s.a);
    hold("b", s.b);
    hold("x", s.x);
    hold("y", s.y);
    hold("lb", s.lb);
    hold("rb", s.rb);
    hold("lt", s.lt > 0.5f);
    hold("rt", s.rt > 0.5f);
    hold("lstick", s.lstick);
    hold("rstick", s.rstick);
    hold("start", s.start);
    hold("back", s.back);
    hold("dpad_up", s.dpad_y > 0);
    hold("dpad_down", s.dpad_y < 0);
    hold("dpad_left", s.dpad_x < 0);
    hold("dpad_right", s.dpad_x > 0);

    auto curve = [](float v) {  // deadzone + quadratic response for fine aim
        float a = std::fabs(v);
        if (a < 0.15f) return 0.0f;
        a = (a - 0.15f) / 0.85f;
        return std::copysign(a * a, v);
    };
    if (snap_deg > 0) {
        if (snap_armed_ && std::fabs(s.rx) > 0.7f) {
            o.look_yaw_deg = std::copysign(snap_deg, s.rx);
            snap_armed_ = false;
        } else if (std::fabs(s.rx) < 0.3f) {
            snap_armed_ = true;
        }
    } else {
        o.look_yaw_deg = curve(s.rx) * turn_speed * dt;
        o.look_pitch_deg = curve(s.ry) * turn_speed * 0.6f * dt;
    }
    return o;
}

}  // namespace vrl
