#pragma once

#include <cstdio>
#include <cstdlib>

namespace vrl {

inline bool log_verbose() {
    static const bool v = [] {
        const char* e = getenv("VRLAYER_DEBUG");
        return e && *e && *e != '0';
    }();
    return v;
}

}  // namespace vrl

#define VRL_LOG(tag, fmt, ...) fprintf(stderr, "[vrlayer:" tag "] " fmt "\n", ##__VA_ARGS__)
#define VRL_DBG(tag, fmt, ...)                                   \
    do {                                                         \
        if (::vrl::log_verbose()) VRL_LOG(tag, fmt, ##__VA_ARGS__); \
    } while (0)
