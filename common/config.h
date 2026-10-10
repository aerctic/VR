// Tiny INI reader used for global settings and per-game profiles.
//
//   ~/.config/vrlayer/default.ini          global defaults
//   ~/.config/vrlayer/games/<appid>.ini    per Steam AppID overrides
//
// Keys are addressed as "section.key". Later files override earlier ones.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vrl {

class Config {
public:
    // Parse INI text, merging into the current values. Returns false and fills
    // *error on a syntax error (values parsed so far are kept).
    bool parse(const std::string& text, std::string* error = nullptr);
    bool load_file(const std::string& path, std::string* error = nullptr);

    bool has(const std::string& key) const { return values_.count(key) != 0; }
    void set(const std::string& key, const std::string& value) { values_[key] = value; }

    std::string get_string(const std::string& key, const std::string& def) const;
    double get_double(const std::string& key, double def) const;
    bool get_bool(const std::string& key, bool def) const;

    const std::map<std::string, std::string>& values() const { return values_; }

private:
    std::map<std::string, std::string> values_;
};

// $XDG_CONFIG_HOME/vrlayer (or ~/.config/vrlayer).
std::string config_dir();

// Files that make up the profile for an AppID, in load order.
std::vector<std::string> profile_paths(uint32_t appid);

}  // namespace vrl
