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

class XrContext {
public:
    bool init(std::string* error);
    void destroy();

    Gpu& gpu() { return gpu_; }
    XrInstance instance() const { return instance_; }
    XrSession session() const { return session_; }
    XrSpace local_space() const { return local_; }
    bool cylinder_supported() const { return cylinder_; }
    const std::string& runtime_name() const { return runtime_name_; }

    // Handles session state changes. Sets *quit when the runtime wants us gone.
    void poll_events(bool* quit);
    bool running() const { return running_; }

    bool wait_frame(XrFrameState* fs);
    bool begin_frame();
    bool end_frame(XrTime display_time, const std::vector<const XrCompositionLayerBaseHeader*>& layers);

    // Head pose in local space.
    bool locate_head(XrTime t, XrPosef* pose);

    // Screen swapchain (recreated when the size changes).
    bool ensure_swapchain(uint32_t w, uint32_t h);
    XrSwapchain swapchain() const { return swapchain_; }
    uint32_t swapchain_width() const { return sw_w_; }
    uint32_t swapchain_height() const { return sw_h_; }
    // Acquire + wait; returns the Vulkan image to write (layout COLOR_ATTACHMENT_OPTIMAL).
    VkImage acquire_image();
    void release_image();

    void request_exit();

private:
    void destroy_swapchain();

    Gpu gpu_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_ = XR_NULL_HANDLE;
    XrSpace view_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool cylinder_ = false;
    std::string runtime_name_;

    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    uint32_t sw_w_ = 0, sw_h_ = 0;
    int64_t sw_format_ = 0;
    std::vector<XrSwapchainImageVulkan2KHR> images_;
};

}  // namespace vrl
