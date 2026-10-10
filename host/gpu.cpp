#include "gpu.h"

#include <cstring>

namespace vrl {

void Gpu::query_properties() {
    vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    memcpy(device_uuid, id.deviceUUID, VK_UUID_SIZE);
    memcpy(driver_uuid, id.driverUUID, VK_UUID_SIZE);
}

uint32_t Gpu::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

std::vector<const char*> Gpu::wanted_device_extensions() {
    return {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
}

bool Gpu::create_headless(Gpu* out, std::string* error) {
    Gpu g;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vrlayer";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) {
        if (error) *error = "vkCreateInstance failed";
        return false;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
    if (devs.empty()) {
        if (error) *error = "no Vulkan devices";
        vkDestroyInstance(g.instance, nullptr);
        return false;
    }
    g.physical = devs[0];

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &nq, fams.data());
    g.family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            g.family = i;
            break;
        }
    if (g.family == UINT32_MAX) {
        if (error) *error = "no graphics+compute queue";
        vkDestroyInstance(g.instance, nullptr);
        return false;
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &ne, exts.data());
    std::vector<const char*> enable;
    for (const char* want : wanted_device_extensions())
        for (auto& e : exts)
            if (!strcmp(e.extensionName, want)) enable.push_back(want);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    if (vkCreateDevice(g.physical, &dci, nullptr, &g.device) != VK_SUCCESS) {
        if (error) *error = "vkCreateDevice failed";
        vkDestroyInstance(g.instance, nullptr);
        return false;
    }
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
    g.external_memory_fd = !enable.empty();
    g.owns = true;
    g.query_properties();
    *out = g;
    return true;
}

void Gpu::destroy() {
    if (!owns) return;
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

bool GpuBuffer::create(const Gpu& gpu, VkDeviceSize sz, VkBufferUsageFlags usage, bool host_visible) {
    size = sz;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sz;
    bci.usage = usage;
    if (vkCreateBuffer(gpu.device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.device, buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu.find_memory_type(
        req.memoryTypeBits, host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                         : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX && !host_visible) mai.memoryTypeIndex = gpu.find_memory_type(req.memoryTypeBits, 0);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(gpu.device, &mai, nullptr, &memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(gpu.device, buffer, memory, 0) != VK_SUCCESS) return false;
    if (host_visible && vkMapMemory(gpu.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
    return true;
}

void GpuBuffer::destroy(const Gpu& gpu) {
    if (buffer) vkDestroyBuffer(gpu.device, buffer, nullptr);
    if (memory) vkFreeMemory(gpu.device, memory, nullptr);
    *this = GpuBuffer{};
}

bool GpuImage::create(const Gpu& gpu, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    width = w;
    height = h;
    format = fmt;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    if (vkCreateImage(gpu.device, &ici, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.device, image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu.find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = gpu.find_memory_type(req.memoryTypeBits, 0);
    if (vkAllocateMemory(gpu.device, &mai, nullptr, &memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(gpu.device, image, memory, 0) != VK_SUCCESS) return false;
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(gpu.device, &vci, nullptr, &view) != VK_SUCCESS) return false;
    }
    return true;
}

void GpuImage::destroy(const Gpu& gpu) {
    if (view) vkDestroyImageView(gpu.device, view, nullptr);
    if (image) vkDestroyImage(gpu.device, image, nullptr);
    if (memory) vkFreeMemory(gpu.device, memory, nullptr);
    *this = GpuImage{};
}

}  // namespace vrl
