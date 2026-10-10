// Vulkan context used by the compositor side (created by OpenXR or headless).
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace vrl {

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem_props{};
    uint8_t device_uuid[VK_UUID_SIZE]{};
    uint8_t driver_uuid[VK_UUID_SIZE]{};
    bool external_memory_fd = false;
    bool owns = false;  // created by create_headless(), destroy() tears it down

    // Fill physical-device derived fields (memory props, UUIDs). Call after
    // instance/physical/device are set by whoever created them.
    void query_properties();
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) const;

    // Standalone Vulkan 1.1 device (tests and tools without OpenXR).
    static bool create_headless(Gpu* out, std::string* error);
    void destroy();

    // Device extensions the compositor wants on whatever device it gets.
    static std::vector<const char*> wanted_device_extensions();
};

// Simple helpers for one-off GPU resources.
struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;

    bool create(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void destroy(const Gpu& gpu);
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;

    bool create(const Gpu& gpu, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage);
    void destroy(const Gpu& gpu);
};

}  // namespace vrl
