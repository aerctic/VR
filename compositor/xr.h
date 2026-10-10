// OpenXR session + Vulkan device for the compositor.
#pragma once

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>
#include <vector>

#include "gpu.h"

namespace vrl {

// One XrSwapchain with its Vulkan images. Images are acquired in
// COLOR_ATTACHMENT_OPTIMAL layout and must be returned in it.
class XrImageSwapchain {
public:
    // formats: preferred formats in order; the first one the runtime offers wins.
    bool ensure(XrSession session, const std::vector<int64_t>& runtime_formats, const std::vector<int64_t>& formats,
                uint32_t w, uint32_t h, VkQueue queue);
    void destroy();
    VkImage acquire();
    void release();

    XrSwapchain handle() const { return swapchain_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    VkFormat format() const { return VkFormat(format_); }

private:
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    uint32_t w_ = 0, h_ = 0;
    int64_t format_ = 0;
    std::vector<XrSwapchainImageVulkan2KHR> images_;
};

class XrContext {
public:
    bool init(std::string* error);
    void destroy();

    Gpu& gpu() { return gpu_; }
    XrInstance instance() const { return instance_; }
    XrSession session() const { return session_; }
    XrSpace local_space() const { return local_; }
    bool cylinder_supported() const { return cylinder_; }
    bool hand_tracking_supported() const { return hand_tracking_; }
    const std::string& runtime_name() const { return runtime_name_; }
    const std::vector<int64_t>& formats() const { return formats_; }
    // Recommended per-eye render size.
    uint32_t eye_width() const { return eye_w_; }
    uint32_t eye_height() const { return eye_h_; }

    // Handles session state changes. Sets *quit when the runtime wants us gone.
    void poll_events(bool* quit);
    bool running() const { return running_; }

    bool wait_frame(XrFrameState* fs);
    bool begin_frame();
    bool end_frame(XrTime display_time, const std::vector<const XrCompositionLayerBaseHeader*>& layers);

    // Head pose in local space.
    bool locate_head(XrTime t, XrPosef* pose);
    // Per-eye poses and FOVs in local space.
    bool locate_views(XrTime t, XrView views[2]);
    // Floor height in local space (stage space if the runtime has one).
    float floor_y(XrTime t);

    void request_exit();

private:
    Gpu gpu_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_ = XR_NULL_HANDLE;
    XrSpace view_ = XR_NULL_HANDLE;
    XrSpace stage_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool cylinder_ = false;
    bool hand_tracking_ = false;
    std::string runtime_name_;
    std::vector<int64_t> formats_;
    uint32_t eye_w_ = 1024, eye_h_ = 1024;
    float floor_y_ = -1.6f;
};

}  // namespace vrl
