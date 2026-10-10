// Shared state of the VRLayer capture layer.
#pragma once

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "protocol.h"

namespace vrl {

class Capture;
class Analyzer;

// Next-layer instance entry points we use.
struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;  // core or KHR, may be null
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
};

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_0;
    InstanceDispatch d{};
};

// X-macro of every next-layer device function we call or wrap.
#define VRL_DEVICE_FUNCS(X)          \
    X(GetDeviceProcAddr)             \
    X(DestroyDevice)                 \
    X(DeviceWaitIdle)                \
    X(GetDeviceQueue)                \
    X(GetDeviceQueue2)               \
    X(QueueSubmit)                   \
    X(QueueWaitIdle)                 \
    X(CreateSwapchainKHR)            \
    X(DestroySwapchainKHR)           \
    X(GetSwapchainImagesKHR)         \
    X(QueuePresentKHR)               \
    X(CreateImage)                   \
    X(DestroyImage)                  \
    X(CreateImageView)               \
    X(DestroyImageView)              \
    X(CreateBuffer)                  \
    X(DestroyBuffer)                 \
    X(GetImageMemoryRequirements)    \
    X(GetBufferMemoryRequirements)   \
    X(AllocateMemory)                \
    X(FreeMemory)                    \
    X(BindImageMemory)               \
    X(BindBufferMemory)              \
    X(MapMemory)                     \
    X(UnmapMemory)                   \
    X(InvalidateMappedMemoryRanges)  \
    X(GetMemoryFdKHR)                \
    X(CreateRenderPass)              \
    X(CreateRenderPass2)             \
    X(CreateRenderPass2KHR)          \
    X(DestroyRenderPass)             \
    X(CreateFramebuffer)             \
    X(DestroyFramebuffer)            \
    X(CreateCommandPool)             \
    X(DestroyCommandPool)            \
    X(AllocateCommandBuffers)        \
    X(FreeCommandBuffers)            \
    X(BeginCommandBuffer)            \
    X(EndCommandBuffer)              \
    X(CreateFence)                   \
    X(DestroyFence)                  \
    X(WaitForFences)                 \
    X(ResetFences)                   \
    X(CreateSemaphore)               \
    X(DestroySemaphore)              \
    X(CmdPipelineBarrier)            \
    X(CmdCopyImage)                  \
    X(CmdCopyImageToBuffer)          \
    X(CmdCopyBuffer)                 \
    X(CmdBeginRenderPass)            \
    X(CmdBeginRenderPass2)           \
    X(CmdBeginRenderPass2KHR)        \
    X(CmdEndRenderPass)              \
    X(CmdEndRenderPass2)             \
    X(CmdEndRenderPass2KHR)          \
    X(CmdBeginRendering)             \
    X(CmdBeginRenderingKHR)          \
    X(CmdEndRendering)               \
    X(CmdEndRenderingKHR)            \
    X(CmdDraw)                       \
    X(CmdDrawIndexed)                \
    X(CmdDrawIndirect)               \
    X(CmdDrawIndexedIndirect)        \
    X(CmdDrawIndirectCount)          \
    X(CmdDrawIndexedIndirectCount)   \
    X(CmdExecuteCommands)            \
    X(MapMemory2KHR)                 \
    X(UnmapMemory2KHR)               \
    X(CmdPushConstants)              \
    X(CmdUpdateBuffer)               \
    X(CmdSetViewport)                \
    X(QueueSubmit2)                  \
    X(QueueSubmit2KHR)

struct DeviceDispatch {
#define VRL_MEMBER(name) PFN_vk##name name;
    VRL_DEVICE_FUNCS(VRL_MEMBER)
#undef VRL_MEMBER
};

struct ImageInfo {
    VkFormat format;
    VkExtent3D extent;
    VkSampleCountFlagBits samples;
    bool depth;  // depth (or depth/stencil) format
};

struct ViewInfo {
    VkImage image;
    uint32_t mip;
    uint32_t layer;
};

struct RenderPassInfo {
    int32_t depth_attachment = -1;  // index into attachments, -1 if none
    VkImageLayout depth_final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct CmdState {
    VkCommandPool pool = VK_NULL_HANDLE;
    bool secondary = false;
    // Current render pass instance.
    bool in_pass = false;
    bool suspending = false;
    VkImage depth = VK_NULL_HANDLE;
    VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;  // layout after the pass ends
    uint32_t depth_mip = 0;
    uint32_t depth_layer = 0;
    uint32_t draws = 0;        // draws in the current pass
    uint32_t total_draws = 0;  // draws in the whole buffer (for secondaries)
};

struct SwapchainData {
    VkExtent2D extent;
    VkFormat format;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> copy_done;  // one per image, signalled by our copy
    uint64_t serial;                     // unique id, changes on recreation
};

// GPU buffer that receives the game's depth buffer at the end of the main pass.
struct DepthStaging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;  // depth image it mirrors
    VkExtent2D extent{};
    DepthFormat format = DepthFormat::None;
    VkDeviceSize size = 0;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceData* instance = nullptr;
    DeviceDispatch d{};
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    uint32_t api_version = VK_API_VERSION_1_0;

    std::vector<VkQueueFamilyProperties> families;
    VkPhysicalDeviceMemoryProperties mem_props{};
    bool external_memory_fd = false;  // VK_KHR_external_memory_fd enabled
    bool has_uuid = false;
    uint8_t device_uuid[VK_UUID_SIZE]{};
    uint8_t driver_uuid[VK_UUID_SIZE]{};

    bool depth_capture = true;
    uint32_t depth_min_draws = 8;

    // Everything below is protected by mtx.
    std::mutex mtx;
    std::unordered_map<VkQueue, uint32_t> queue_family;
    std::unordered_map<VkSwapchainKHR, SwapchainData> swapchains;
    std::unordered_map<VkImage, ImageInfo> images;
    std::unordered_map<VkImageView, ViewInfo> views;
    std::unordered_map<VkRenderPass, RenderPassInfo> render_passes;
    std::unordered_map<VkFramebuffer, std::vector<VkImageView>> framebuffers;
    std::unordered_map<VkCommandBuffer, CmdState> cmds;

    // Depth selection: draws per depth image since the last present.
    std::unordered_map<VkImage, uint64_t> depth_draws;
    VkImage selected_depth = VK_NULL_HANDLE;
    DepthStaging depth_staging;
    std::vector<DepthStaging> retired_staging;  // may still be referenced by recorded command buffers
    bool depth_copied_since_present = false;
    uint32_t frames_since_depth_copy = 1000;

    std::unique_ptr<Analyzer> analyzer;  // null when VRLAYER_ANALYZE=0
    std::unique_ptr<Capture> capture;
};

DepthFormat depth_format_of(VkFormat f);
bool format_has_stencil(VkFormat f);
uint32_t find_memory_type(const DeviceData* dev, uint32_t type_bits, VkMemoryPropertyFlags want,
                          VkMemoryPropertyFlags avoid = 0);

}  // namespace vrl
