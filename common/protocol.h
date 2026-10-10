// Wire protocol between the capture layer (inside the game process) and the
// VRLayer compositor (OpenXR application).
//
// Transport: AF_UNIX SOCK_SEQPACKET socket in the abstract namespace
// ("\0vrlayer" by default, override with VRLAYER_SOCKET). Every message is a
// single fixed-size packet starting with MsgHeader. File descriptors (shared
// GPU memory or memfds) travel as SCM_RIGHTS ancillary data on Slots messages.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vrl {

constexpr uint32_t kMagic = 0x594C5256;  // "VRLY"
constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaxSlots = 3;
constexpr const char* kDefaultSocketName = "vrlayer";

enum class MsgType : uint32_t {
    Hello = 1,       // layer -> compositor: identify game + GPU
    Welcome,         // compositor -> layer: chosen transport
    Slots,           // layer -> compositor: shared frame slots (+ fds)
    Frame,           // layer -> compositor: slot N holds a new frame
    Release,         // compositor -> layer: done reading slot N
    Control,         // vrlayer-ctl -> compositor: text command
    ControlReply,    // compositor -> vrlayer-ctl: text reply
};

// How frames are shared.
//  Gpu: the slot memory is exported with VK_KHR_external_memory_fd (opaque fd)
//       and imported by the compositor on the same physical device. Zero copy.
//  Cpu: the layer reads frames back into memfd-backed shared memory and the
//       compositor uploads them. Slow but works across GPUs / drivers.
enum class Transport : uint32_t { Gpu = 1, Cpu = 2 };

// Raw layout of the depth texels as copied with VK_IMAGE_ASPECT_DEPTH_BIT.
enum class DepthFormat : uint32_t {
    None = 0,
    D16 = 1,    // 16-bit unorm, 2 bytes per texel
    X8D24 = 2,  // 24-bit unorm in the low bits of a 32-bit word
    D32F = 3,   // 32-bit float
};

inline uint32_t depth_bytes_per_texel(DepthFormat f) {
    switch (f) {
        case DepthFormat::D16: return 2;
        case DepthFormat::X8D24:
        case DepthFormat::D32F: return 4;
        default: return 0;
    }
}

// Usage flags both sides must use when creating the shared resources so the
// imported objects match the exported ones exactly (raw VkImageUsageFlags /
// VkBufferUsageFlags values, kept here to avoid a Vulkan include).
constexpr uint32_t kSlotImageUsage = 0x1 /*TRANSFER_SRC*/ | 0x2 /*TRANSFER_DST*/ | 0x4 /*SAMPLED*/;
constexpr uint32_t kSlotDepthBufferUsage = 0x1 /*TRANSFER_SRC*/ | 0x2 /*TRANSFER_DST*/ | 0x20 /*STORAGE*/;

struct MsgHeader {
    uint32_t magic;
    uint32_t type;
    uint32_t size;
    uint32_t reserved;
};

struct HelloMsg {
    MsgHeader h;
    uint32_t version;
    int32_t pid;
    uint32_t appid;          // SteamAppId, 0 if unknown
    uint32_t has_uuid;       // device/driver UUID valid
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
    uint32_t requested_transport;  // 0 = let the compositor decide
    uint32_t pad;
    char exe[128];
};

struct WelcomeMsg {
    MsgHeader h;
    uint32_t transport;  // Transport
    uint32_t pad;
};

// Followed by SCM_RIGHTS fds: for each slot, the color fd, then (if
// depth_format != None) the depth fd. Gpu: opaque memory fds. Cpu: memfds.
struct SlotsMsg {
    MsgHeader h;
    uint32_t generation;  // bumped every time slots are recreated
    uint32_t transport;
    uint32_t slot_count;
    uint32_t width;
    uint32_t height;
    uint32_t color_format;       // VkFormat of the game swapchain
    uint32_t color_memory_type;  // Gpu only
    uint32_t depth_format;       // DepthFormat
    uint32_t depth_width;
    uint32_t depth_height;
    uint32_t depth_memory_type;  // Gpu only
    uint32_t pad;
    uint64_t color_alloc_size;
    uint64_t depth_alloc_size;
};

struct FrameMsg {
    MsgHeader h;
    uint32_t generation;
    uint32_t slot;
    uint64_t frame_index;
    uint64_t timestamp_ns;  // CLOCK_MONOTONIC when the copy finished
    uint32_t has_depth;     // depth slot contents are valid for this frame
    uint32_t pad;
};

struct ReleaseMsg {
    MsgHeader h;
    uint32_t generation;
    uint32_t slot;
};

struct ControlMsg {
    MsgHeader h;
    char text[256];
};

struct ControlReplyMsg {
    MsgHeader h;
    char text[2048];
};

constexpr size_t kMaxMsgSize = sizeof(ControlReplyMsg);

template <typename T>
inline T make_msg(MsgType type) {
    T m{};
    m.h.magic = kMagic;
    m.h.type = static_cast<uint32_t>(type);
    m.h.size = sizeof(T);
    return m;
}

}  // namespace vrl
