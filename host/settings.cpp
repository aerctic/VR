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
        F("stereo.enabled", Bool, stereo),
        F("stereo.separation", Float, separation),
        F("stereo.convergence", Float, convergence),
        F("stereo.popout", Float, popout),
        F("stereo.depth_reversed", String, depth_reversed),
        F("stereo.depth_far_plane", Float, depth_far_plane),
        F("stereo.depth_flip_y", Bool, depth_flip_y),
        F("stereo.search_steps", Int, search_steps),
        F("stereo.show_depth", Bool, show_depth),
        F("input.head_mouse", Bool, head_mouse),
        F("input.mouse_counts_per_degree", Float, mouse_counts_per_degree),
        F("input.mouse_invert_y", Bool, mouse_invert_y),
        F("input.head_mouse_pitch", Bool, head_mouse_pitch),
        F("input.gamepad", Bool, gamepad),
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

bool Settings::set(const std::string& key, const std::string& value, std::string* error) {
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
                if (key == "stereo.depth_reversed" && value != "auto" && value != "true" && value != "false") break;
                *static_cast<std::string*>(p) = value;
                return true;
            case Kind::Mode:
                if (value == "theater" || value == "theatre") *static_cast<ViewMode*>(p) = ViewMode::Theater;
                else if (value == "immersive") *static_cast<ViewMode*>(p) = ViewMode::Immersive;
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
            case Kind::Mode: o << (*static_cast<ViewMode*>(p) == ViewMode::Theater ? "theater" : "immersive"); break;
        }
        return o.str();
    }
    return "";
}

Settings Settings::from_config(const Config& cfg) {
    Settings s;
    for (const Field& f : fields())
        if (cfg.has(f.key)) s.set(f.key, cfg.get_string(f.key, ""), nullptr);
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
    return o.str();
}

}  // namespace vrl
