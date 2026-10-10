#include "config.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace vrl {

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool Config::parse(const std::string& text, std::string* error) {
    std::istringstream in(text);
    std::string line, section;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // Strip comments (';' or '#') that start a line or follow whitespace.
        for (size_t i = 0; i < line.size(); ++i) {
            if ((line[i] == ';' || line[i] == '#') && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line.resize(i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') {
                if (error) *error = "line " + std::to_string(lineno) + ": unterminated section header";
                return false;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "line " + std::to_string(lineno) + ": expected key = value";
            return false;
        }
        std::string key = lower(trim(line.substr(0, eq)));
        std::string value = trim(line.substr(eq + 1));
        if (key.empty()) {
            if (error) *error = "line " + std::to_string(lineno) + ": empty key";
            return false;
        }
        values_[section.empty() ? key : section + "." + key] = value;
    }
    return true;
}

bool Config::load_file(const std::string& path, std::string* error) {
    std::ifstream f(path);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!parse(ss.str(), &err)) {
        if (error) *error = path + ": " + err;
        return false;
    }
    return true;
}

std::string Config::get_string(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

double Config::get_double(const std::string& key, double def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    char* end = nullptr;
    double v = strtod(it->second.c_str(), &end);
    return (end && end != it->second.c_str()) ? v : def;
}

bool Config::get_bool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    std::string v = lower(it->second);
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

std::string config_dir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/vrlayer";
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/vrlayer";
}

std::vector<std::string> profile_paths(uint32_t appid) {
    std::vector<std::string> paths{config_dir() + "/default.ini"};
    if (appid) {
        // Results detected by the loading screen, then the user's own settings.
        paths.push_back(config_dir() + "/games/" + std::to_string(appid) + ".auto.ini");
        paths.push_back(config_dir() + "/games/" + std::to_string(appid) + ".ini");
    }
    return paths;
}

}  // namespace vrl
