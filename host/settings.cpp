#include "settings.h"

#include <cstdlib>
#include <functional>
#include <sstream>
#include <vector>

namespace vrl {

namespace {

enum class Kind { Float, Int, Bool, String, Mode };

struct Field {
    const char* key;
    Kind kind;
    std::function<void*(Settings&)> ptr;
};

#define F(key, kind, member) {key, Kind::kind, [](Settings& s) -> void* { return &s.member; }}
const std::vector<Field>& fields() {
    static const std::vector<Field> f = {
        F("display.mode", Mode, mode),
        F("display.screen_distance", Float, screen_distance),
        F("display.screen_width", Float, screen_width),
        F("display.screen_height", Float, screen_height),
        F("display.curved", Bool, curved),
        F("display.game_fov", Float, game_fov),
        F("display.immersive_distance", Float, immersive_distance),
        F("display.immersive_latency_ms", Float, immersive_latency_ms),
        F("display.environment", Bool, environment),
        F("stereo.enabled", Bool, stereo),
        F("stereo.separation", Float, separation),
        F("stereo.convergence", Float, convergence),
        F("stereo.popout", Float, popout),
        F("stereo.depth_reversed", String, depth_reversed),
        F("stereo.depth_far_plane", Float, depth_far_plane),
        F("stereo.depth_flip_y", Bool, depth_flip_y),
        F("stereo.search_steps", Int, search_steps),
        F("stereo.show_depth", Bool, show_depth),
        F("stereo.convergence_near", Float, convergence_near),
        F("vr.camera", String, vr_camera),
        F("vr.units_per_metre", Float, units_per_metre),
        F("vr.ipd", Float, ipd),
        F("vr.aim", String, aim),
        F("vr.snap_turn", Float, snap_turn),
        F("vr.turn_speed", Float, turn_speed),
        F("vr.fixed_camera", Bool, fixed_camera),
        F("vr.show_room", Bool, show_room),
        F("loading.enabled", Bool, loading),
        F("loading.auto_continue", Bool, auto_continue),
        F("loading.timeout", Float, loading_timeout),
        F("loading.calibrate_mouse", Bool, calibrate_mouse),
        F("input.head_mouse", Bool, head_mouse),
        F("input.mouse_counts_per_degree", Float, mouse_counts_per_degree),
        F("input.mouse_invert_y", Bool, mouse_invert_y),
        F("input.head_mouse_pitch", Bool, head_mouse_pitch),
        F("input.gamepad", Bool, gamepad),
        F("input.scheme", String, scheme),
        F("input.pointer", Bool, pointer),
        F("input.hand_tracking", Bool, hand_tracking),
        F("input.hand_mouse", String, hand_mouse),
        F("input.hand_mouse_hand", String, hand_mouse_hand),
        F("input.hand_mouse_offset", Float, hand_mouse_offset),
        F("input.hand_mouse_counts_per_pixel", Float, hand_mouse_counts_per_pixel),
    };
    return f;
}
#undef F

bool parse_bool(const std::string& v, bool* out) {
    Config c;
    c.set("x", v);
    bool a = c.get_bool("x", true), b = c.get_bool("x", false);
    if (a != b) return false;  // not a recognised boolean
    *out = a;
    return true;
}

}  // namespace

static bool valid_choice(const std::string& key, const std::string& v) {
    static const std::map<std::string, std::set<std::string>> choices = {
        {"stereo.depth_reversed", {"auto", "true", "false"}},
        {"vr.camera", {"head", "alternate"}},
        {"vr.aim", {"auto", "head", "body"}},
        {"input.scheme", {"gamepad", "keyboard_mouse"}},
        {"input.hand_mouse", {"off", "absolute", "relative"}},
        {"input.hand_mouse_hand", {"right", "left"}},
    };
    auto it = choices.find(key);
    return it == choices.end() || it->second.count(v) != 0;
}

bool Settings::set(const std::string& key, const std::string& value, std::string* error) {
    if (key.rfind("bindings.", 0) == 0 && key.size() > 9) {
        bindings[key.substr(9)] = value;
        return true;
    }
    for (const Field& f : fields()) {
        if (key != f.key) continue;
        void* p = f.ptr(*this);
        char* end = nullptr;
        switch (f.kind) {
            case Kind::Float: {
                float v = strtof(value.c_str(), &end);
                if (end == value.c_str()) break;
                *static_cast<float*>(p) = v;
                return true;
            }
            case Kind::Int: {
                long v = strtol(value.c_str(), &end, 10);
                if (end == value.c_str()) break;
                *static_cast<int*>(p) = int(v);
                return true;
            }
            case Kind::Bool:
                if (parse_bool(value, static_cast<bool*>(p))) return true;
                break;
            case Kind::String:
                if (!valid_choice(key, value)) break;
                *static_cast<std::string*>(p) = value;
                return true;
            case Kind::Mode:
                if (value == "theater" || value == "theatre") *static_cast<ViewMode*>(p) = ViewMode::Theater;
                else if (value == "immersive") *static_cast<ViewMode*>(p) = ViewMode::Immersive;
                else if (value == "vr") *static_cast<ViewMode*>(p) = ViewMode::Vr;
                else break;
                return true;
        }
        if (error) *error = "invalid value '" + value + "' for " + key;
        return false;
    }
    if (error) *error = "unknown setting '" + key + "'";
    return false;
}

std::string Settings::get(const std::string& key) const {
    if (key.rfind("bindings.", 0) == 0) {
        auto it = bindings.find(key.substr(9));
        return it == bindings.end() ? "" : it->second;
    }
    Settings& self = const_cast<Settings&>(*this);
    for (const Field& f : fields()) {
        if (key != f.key) continue;
        void* p = f.ptr(self);
        std::ostringstream o;
        switch (f.kind) {
            case Kind::Float: o << *static_cast<float*>(p); break;
            case Kind::Int: o << *static_cast<int*>(p); break;
            case Kind::Bool: o << (*static_cast<bool*>(p) ? "true" : "false"); break;
            case Kind::String: o << *static_cast<std::string*>(p); break;
            case Kind::Mode: {
                ViewMode m = *static_cast<ViewMode*>(p);
                o << (m == ViewMode::Theater ? "theater" : m == ViewMode::Immersive ? "immersive" : "vr");
                break;
            }
        }
        return o.str();
    }
    return "";
}

Settings Settings::from_config(const Config& cfg) {
    Settings s;
    for (const Field& f : fields())
        if (cfg.has(f.key) && s.set(f.key, cfg.get_string(f.key, ""), nullptr)) s.explicit_keys.insert(f.key);
    for (auto& [k, v] : cfg.values())
        if (k.rfind("bindings.", 0) == 0) s.set(k, v, nullptr);
    return s;
}

std::string Settings::dump() const {
    std::ostringstream o;
    std::string section;
    for (const Field& f : fields()) {
        std::string k = f.key;
        size_t dot = k.find('.');
        if (k.substr(0, dot) != section) {
            section = k.substr(0, dot);
            o << (o.tellp() > 0 ? "\n" : "") << "[" << section << "]\n";
        }
        o << k.substr(dot + 1) << " = " << get(k) << "\n";
    }
    if (!bindings.empty()) {
        o << "\n[bindings]\n";
        for (auto& [b, key] : bindings) o << b << " = " << key << "\n";
    }
    return o.str();
}

}  // namespace vrl
