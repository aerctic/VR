// VRLayer Vulkan implicit layer.
//
// Loaded into the game process (native Vulkan games, and D3D games running on
// Proton through DXVK / VKD3D-Proton). It
//   * adds TRANSFER_SRC to swapchain and depth images so they can be copied,
//   * tracks which depth buffer belongs to the main 3D scene (the one with the
//     most draws whose size matches the swapchain),
//   * copies that depth buffer at the end of each render pass that uses it,
//   * and on every present hands the frame (color + depth) to Capture, which
//     streams it to the VRLayer compositor.
// The game is never blocked: if the compositor is not running or is busy the
// frame is simply not exported.

#include "layer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "analyzer.h"
#include "capture.h"
#include "log.h"

namespace vrl {

// ---------------------------------------------------------------------------
// Dispatch bookkeeping
// ---------------------------------------------------------------------------

using DispatchKey = void*;
template <typename T>
static DispatchKey key_of(T handle) {
    return *reinterpret_cast<DispatchKey*>(handle);
}

static std::mutex g_mtx;
static std::unordered_map<DispatchKey, InstanceData*> g_instances;
static std::unordered_map<DispatchKey, DeviceData*> g_devices;
static std::atomic<uint64_t> g_device_epoch{1};

static InstanceData* get_instance(DispatchKey k) {
    std::lock_guard<std::mutex> l(g_mtx);
    auto it = g_instances.find(k);
    return it == g_instances.end() ? nullptr : it->second;
}

// Device lookups happen on every draw call, so cache the last hit per thread.
// The epoch invalidates the cache whenever a device is destroyed.
static DeviceData* get_device(DispatchKey k) {
    thread_local DispatchKey cached_key = nullptr;
    thread_local DeviceData* cached_dev = nullptr;
    thread_local uint64_t cached_epoch = 0;
    uint64_t epoch = g_device_epoch.load(std::memory_order_acquire);
    if (cached_key == k && cached_epoch == epoch) return cached_dev;
    std::lock_guard<std::mutex> l(g_mtx);
    auto it = g_devices.find(k);
    cached_key = k;
    cached_dev = it == g_devices.end() ? nullptr : it->second;
    cached_epoch = epoch;
    return cached_dev;
}

template <typename T>
static DeviceData* dev_of(T handle) {
    return get_device(key_of(handle));
}

static bool env_flag(const char* name, bool def) {
    const char* v = getenv(name);
    if (!v || !*v) return def;
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

// ---------------------------------------------------------------------------
// Helpers shared with capture.cpp
// ---------------------------------------------------------------------------

DepthFormat depth_format_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT: return DepthFormat::D16;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT: return DepthFormat::X8D24;
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return DepthFormat::D32F;
        default: return DepthFormat::None;
    }
}

bool format_has_stencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}

uint32_t find_memory_type(const DeviceData* dev, uint32_t type_bits, VkMemoryPropertyFlags want,
                          VkMemoryPropertyFlags avoid) {
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < dev->mem_props.memoryTypeCount; ++i) {
            if (!(type_bits & (1u << i))) continue;
            VkMemoryPropertyFlags f = dev->mem_props.memoryTypes[i].propertyFlags;
            if ((f & want) != want) continue;
            if (pass == 0 && (f & avoid)) continue;
            return i;
        }
    }
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                                     VkInstance* out) {
    auto* link = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(link->pNext));
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    auto* inst = new InstanceData;
    inst->instance = *out;
    if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) inst->api_version = ci->pApplicationInfo->apiVersion;

    bool props2_ext = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) props2_ext = true;

    InstanceDispatch& d = inst->d;
#define LOAD(name) d.name = reinterpret_cast<PFN_vk##name>(gipa(*out, "vk" #name))
    LOAD(GetInstanceProcAddr);
    LOAD(DestroyInstance);
    LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(GetPhysicalDeviceFormatProperties);
    LOAD(EnumerateDeviceExtensionProperties);
#undef LOAD
    d.GetInstanceProcAddr = gipa;
    if (VK_API_VERSION_MINOR(inst->api_version) >= 1 || VK_API_VERSION_MAJOR(inst->api_version) > 1) {
        d.GetPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(*out, "vkGetPhysicalDeviceProperties2"));
        d.GetPhysicalDeviceImageFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            gipa(*out, "vkGetPhysicalDeviceImageFormatProperties2"));
    } else if (props2_ext) {
        d.GetPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(*out, "vkGetPhysicalDeviceProperties2KHR"));
    }

    std::lock_guard<std::mutex> l(g_mtx);
    g_instances[key_of(*out)] = inst;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    InstanceData* inst;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        auto it = g_instances.find(key_of(instance));
        if (it == g_instances.end()) return;
        inst = it->second;
        g_instances.erase(it);
    }
    inst->d.DestroyInstance(instance, alloc);
    delete inst;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

static bool device_supports_ext(InstanceData* inst, VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0;
    inst->d.EnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    inst->d.EnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    for (auto& e : exts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                   const VkAllocationCallbacks* alloc, VkDevice* out) {
    InstanceData* inst = get_instance(key_of(phys));
    auto* link = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(link->pNext));
    auto* cb = static_cast<const VkLayerDeviceCreateInfo*>(ci->pNext);
    while (cb && !(cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && cb->function == VK_LOADER_DATA_CALLBACK))
        cb = static_cast<const VkLayerDeviceCreateInfo*>(cb->pNext);
    if (!inst || !link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));

    VkPhysicalDeviceProperties props;
    inst->d.GetPhysicalDeviceProperties(phys, &props);
    uint32_t api = inst->api_version < props.apiVersion ? inst->api_version : props.apiVersion;
    bool api11 = VK_API_VERSION_MAJOR(api) > 1 || VK_API_VERSION_MINOR(api) >= 1;

    // Enable VK_KHR_external_memory_fd for zero-copy sharing with the compositor.
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    bool want_fd = api11 && env_flag("VRLAYER_GPU_SHARING", true) &&
                   device_supports_ext(inst, phys, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    bool have_fd = false;
    for (const char* e : exts)
        if (!strcmp(e, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) have_fd = true;
    if (want_fd && !have_fd) exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    VkDeviceCreateInfo mod = *ci;
    mod.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    mod.ppEnabledExtensionNames = exts.data();

    VkResult r = create(phys, &mod, alloc, out);
    if (r != VK_SUCCESS && want_fd && !have_fd) {
        // Be conservative: retry exactly as the application asked.
        want_fd = false;
        r = create(phys, ci, alloc, out);
    }
    if (r != VK_SUCCESS) return r;

    auto* dev = new DeviceData;
    dev->device = *out;
    dev->physical = phys;
    dev->instance = inst;
    dev->api_version = api;
    dev->set_loader_data = cb ? cb->u.pfnSetDeviceLoaderData : nullptr;
    dev->external_memory_fd = want_fd || have_fd;
    dev->depth_capture = env_flag("VRLAYER_DEPTH", true);
    if (const char* md = getenv("VRLAYER_DEPTH_MIN_DRAWS")) dev->depth_min_draws = static_cast<uint32_t>(atoi(md));

#define VRL_LOAD(name) dev->d.name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name));
    VRL_DEVICE_FUNCS(VRL_LOAD)
#undef VRL_LOAD
    dev->d.GetDeviceProcAddr = gdpa;

    uint32_t nfam = 0;
    inst->d.GetPhysicalDeviceQueueFamilyProperties(phys, &nfam, nullptr);
    dev->families.resize(nfam);
    inst->d.GetPhysicalDeviceQueueFamilyProperties(phys, &nfam, dev->families.data());
    inst->d.GetPhysicalDeviceMemoryProperties(phys, &dev->mem_props);

    if (inst->d.GetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        inst->d.GetPhysicalDeviceProperties2(phys, &p2);
        memcpy(dev->device_uuid, id.deviceUUID, VK_UUID_SIZE);
        memcpy(dev->driver_uuid, id.driverUUID, VK_UUID_SIZE);
        dev->has_uuid = true;
    }

    VRL_LOG("layer", "attached to device '%s' (Vulkan %u.%u, gpu sharing %s, depth capture %s)", props.deviceName,
            VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), dev->external_memory_fd ? "on" : "off",
            dev->depth_capture ? "on" : "off");

    if (env_flag("VRLAYER_ANALYZE", true)) dev->analyzer = std::make_unique<Analyzer>();
    dev->capture = std::make_unique<Capture>(dev);

    std::lock_guard<std::mutex> l(g_mtx);
    g_devices[key_of(*out)] = dev;
    return VK_SUCCESS;
}

static void destroy_staging(DeviceData* dev, DepthStaging& s) {
    if (s.buffer) dev->d.DestroyBuffer(dev->device, s.buffer, nullptr);
    if (s.memory) dev->d.FreeMemory(dev->device, s.memory, nullptr);
    s = DepthStaging{};
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DeviceData* dev;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        auto it = g_devices.find(key_of(device));
        if (it == g_devices.end()) return;
        dev = it->second;
        g_devices.erase(it);
        g_device_epoch.fetch_add(1, std::memory_order_release);
    }
    dev->capture.reset();
    dev->analyzer.reset();
    for (auto& [sc, data] : dev->swapchains)
        for (VkSemaphore s : data.copy_done)
            if (s) dev->d.DestroySemaphore(device, s, nullptr);
    destroy_staging(dev, dev->depth_staging);
    for (auto& s : dev->retired_staging) destroy_staging(dev, s);
    dev->d.DestroyDevice(device, alloc);
    delete dev;
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceData* dev = dev_of(device);
    dev->d.GetDeviceQueue(device, family, index, queue);
    std::lock_guard<std::mutex> l(dev->mtx);
    dev->queue_family[*queue] = family;
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceData* dev = dev_of(device);
    dev->d.GetDeviceQueue2(device, info, queue);
    if (!*queue) return;
    std::lock_guard<std::mutex> l(dev->mtx);
    dev->queue_family[*queue] = info->queueFamilyIndex;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

static std::atomic<uint64_t> g_swapchain_serial{1};

static VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                         const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceData* dev = dev_of(device);
    VkSwapchainCreateInfoKHR mod = *ci;
    mod.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = dev->d.CreateSwapchainKHR(device, &mod, alloc, out);
    if (r != VK_SUCCESS) {
        // Extremely unlikely, but never break the game because of us.
        r = dev->d.CreateSwapchainKHR(device, ci, alloc, out);
        if (r != VK_SUCCESS) return r;
        VRL_LOG("layer", "swapchain does not support TRANSFER_SRC, capture disabled for it");
        return r;
    }

    SwapchainData sc;
    sc.extent = ci->imageExtent;
    sc.format = ci->imageFormat;
    sc.serial = g_swapchain_serial.fetch_add(1);
    uint32_t n = 0;
    dev->d.GetSwapchainImagesKHR(device, *out, &n, nullptr);
    sc.images.resize(n);
    dev->d.GetSwapchainImagesKHR(device, *out, &n, sc.images.data());
    sc.copy_done.assign(n, VK_NULL_HANDLE);
    VRL_DBG("layer", "swapchain %ux%u format %d, %u images", sc.extent.width, sc.extent.height, sc.format, n);

    std::lock_guard<std::mutex> l(dev->mtx);
    dev->swapchains[*out] = std::move(sc);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    if (swapchain) {
        // Our copies might still be waiting on the semaphores.
        dev->capture->wait_idle();
        std::lock_guard<std::mutex> l(dev->mtx);
        auto it = dev->swapchains.find(swapchain);
        if (it != dev->swapchains.end()) {
            for (VkSemaphore s : it->second.copy_done)
                if (s) dev->d.DestroySemaphore(device, s, nullptr);
            dev->swapchains.erase(it);
        }
    }
    dev->d.DestroySwapchainKHR(device, swapchain, alloc);
}

static bool create_staging(DeviceData* dev, VkImage image, const ImageInfo& info, DepthStaging* out) {
    DepthStaging s;
    s.image = image;
    s.extent = {info.extent.width, info.extent.height};
    s.format = depth_format_of(info.format);
    s.size = VkDeviceSize(s.extent.width) * s.extent.height * depth_bytes_per_texel(s.format);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = s.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (dev->d.CreateBuffer(dev->device, &bci, nullptr, &s.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    dev->d.GetBufferMemoryRequirements(dev->device, s.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = find_memory_type(dev, req.memoryTypeBits, 0);
    if (dev->d.AllocateMemory(dev->device, &mai, nullptr, &s.memory) != VK_SUCCESS ||
        dev->d.BindBufferMemory(dev->device, s.buffer, s.memory, 0) != VK_SUCCESS) {
        destroy_staging(dev, s);
        return false;
    }
    *out = s;
    return true;
}

// Pick the depth buffer of the main scene. Called with dev->mtx held.
static void select_depth(DeviceData* dev, VkExtent2D target) {
    VkImage best = VK_NULL_HANDLE;
    uint64_t best_draws = 0, current_draws = 0;
    double target_aspect = double(target.width) / double(target.height ? target.height : 1);
    for (auto& [image, draws] : dev->depth_draws) {
        auto it = dev->images.find(image);
        if (it == dev->images.end()) continue;
        const ImageInfo& info = it->second;
        if (info.samples != VK_SAMPLE_COUNT_1_BIT || depth_format_of(info.format) == DepthFormat::None) continue;
        // Must look like a full-screen buffer (allowing for render scaling).
        double aspect = double(info.extent.width) / double(info.extent.height ? info.extent.height : 1);
        if (std::abs(aspect - target_aspect) > 0.03 * target_aspect) continue;
        if (info.extent.width * 3 < target.width) continue;
        if (image == dev->selected_depth) current_draws = draws;
        if (draws > best_draws) {
            best_draws = draws;
            best = image;
        }
    }
    dev->depth_draws.clear();
    if (!best) return;
    // Hysteresis: only switch when the new candidate clearly wins.
    if (dev->selected_depth && current_draws * 2 >= best_draws) return;
    if (best == dev->selected_depth) return;

    const ImageInfo& info = dev->images[best];
    DepthStaging s;
    if (!create_staging(dev, best, info, &s)) return;
    if (dev->depth_staging.buffer) dev->retired_staging.push_back(dev->depth_staging);
    dev->depth_staging = s;
    dev->selected_depth = best;
    dev->frames_since_depth_copy = 1000;
    VRL_LOG("layer", "selected depth buffer %ux%u (format %d, %llu draws/frame)", info.extent.width,
            info.extent.height, info.format, static_cast<unsigned long long>(best_draws));
}

static VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceData* dev = dev_of(queue);
    if (dev->depth_capture && info->swapchainCount > 0) {
        std::lock_guard<std::mutex> l(dev->mtx);
        auto it = dev->swapchains.find(info->pSwapchains[0]);
        if (it != dev->swapchains.end()) select_depth(dev, it->second.extent);
        if (dev->depth_copied_since_present) dev->frames_since_depth_copy = 0;
        else if (dev->frames_since_depth_copy < 1000) dev->frames_since_depth_copy++;
        dev->depth_copied_since_present = false;
    }

    if (dev->analyzer && info->swapchainCount > 0) {
        VkExtent2D extent{};
        bool depth;
        {
            std::lock_guard<std::mutex> l(dev->mtx);
            auto it = dev->swapchains.find(info->pSwapchains[0]);
            if (it != dev->swapchains.end()) extent = it->second.extent;
            depth = dev->selected_depth != VK_NULL_HANDLE;
        }
        if (extent.width && dev->analyzer->on_present(extent, depth)) dev->capture->report_analysis();
    }

    VkSemaphore wait = VK_NULL_HANDLE;
    if (dev->capture->on_present(queue, info, &wait)) {
        VkPresentInfoKHR mod = *info;
        mod.waitSemaphoreCount = 1;
        mod.pWaitSemaphores = &wait;
        return dev->d.QueuePresentKHR(queue, &mod);
    }
    return dev->d.QueuePresentKHR(queue, info);
}

// ---------------------------------------------------------------------------
// Resource tracking (only hooked when depth capture is enabled)
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* ci,
                                                  const VkAllocationCallbacks* alloc, VkImage* out) {
    DeviceData* dev = dev_of(device);
    bool depth = (ci->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                 depth_format_of(ci->format) != DepthFormat::None && ci->imageType == VK_IMAGE_TYPE_2D &&
                 ci->samples == VK_SAMPLE_COUNT_1_BIT;
    VkResult r;
    if (depth) {
        VkImageCreateInfo mod = *ci;
        mod.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        r = dev->d.CreateImage(device, &mod, alloc, out);
        if (r != VK_SUCCESS) {
            depth = false;
            r = dev->d.CreateImage(device, ci, alloc, out);
        }
    } else {
        r = dev->d.CreateImage(device, ci, alloc, out);
    }
    if (r == VK_SUCCESS && depth) {
        std::lock_guard<std::mutex> l(dev->mtx);
        dev->images[*out] = ImageInfo{ci->format, ci->extent, ci->samples, true};
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    if (image) {
        std::lock_guard<std::mutex> l(dev->mtx);
        dev->images.erase(image);
        dev->depth_draws.erase(image);
        if (dev->selected_depth == image) dev->selected_depth = VK_NULL_HANDLE;
    }
    dev->d.DestroyImage(device, image, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* ci,
                                                      const VkAllocationCallbacks* alloc, VkImageView* out) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.CreateImageView(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(dev->mtx);
        if (dev->images.count(ci->image))
            dev->views[*out] = ViewInfo{ci->image, ci->subresourceRange.baseMipLevel, ci->subresourceRange.baseArrayLayer};
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    if (view) {
        std::lock_guard<std::mutex> l(dev->mtx);
        dev->views.erase(view);
    }
    dev->d.DestroyImageView(device, view, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* ci,
                                                       const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.CreateRenderPass(device, ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    RenderPassInfo rp;
    for (uint32_t s = 0; s < ci->subpassCount && rp.depth_attachment < 0; ++s) {
        const VkAttachmentReference* ds = ci->pSubpasses[s].pDepthStencilAttachment;
        if (ds && ds->attachment != VK_ATTACHMENT_UNUSED) {
            rp.depth_attachment = static_cast<int32_t>(ds->attachment);
            rp.depth_final_layout = ci->pAttachments[ds->attachment].finalLayout;
        }
    }
    std::lock_guard<std::mutex> l(dev->mtx);
    dev->render_passes[*out] = rp;
    return r;
}

template <PFN_vkCreateRenderPass2 DeviceDispatch::*Next>
static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* ci,
                                                        const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    DeviceData* dev = dev_of(device);
    VkResult r = (dev->d.*Next)(device, ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    RenderPassInfo rp;
    for (uint32_t s = 0; s < ci->subpassCount && rp.depth_attachment < 0; ++s) {
        const VkAttachmentReference2* ds = ci->pSubpasses[s].pDepthStencilAttachment;
        if (ds && ds->attachment != VK_ATTACHMENT_UNUSED) {
            rp.depth_attachment = static_cast<int32_t>(ds->attachment);
            rp.depth_final_layout = ci->pAttachments[ds->attachment].finalLayout;
        }
    }
    std::lock_guard<std::mutex> l(dev->mtx);
    dev->render_passes[*out] = rp;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    if (rp) {
        std::lock_guard<std::mutex> l(dev->mtx);
        dev->render_passes.erase(rp);
    }
    dev->d.DestroyRenderPass(device, rp, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* ci,
                                                        const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.CreateFramebuffer(device, ci, alloc, out);
    if (r == VK_SUCCESS && !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
        std::lock_guard<std::mutex> l(dev->mtx);
        dev->framebuffers[*out].assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    if (fb) {
        std::lock_guard<std::mutex> l(dev->mtx);
        dev->framebuffers.erase(fb);
    }
    dev->d.DestroyFramebuffer(device, fb, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* ai,
                                                             VkCommandBuffer* out) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.AllocateCommandBuffers(device, ai, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::mutex> l(dev->mtx);
    for (uint32_t i = 0; i < ai->commandBufferCount; ++i) {
        CmdState& s = dev->cmds[out[i]];
        s = CmdState{};
        s.pool = ai->commandPool;
        s.secondary = ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n,
                                                     const VkCommandBuffer* cbs) {
    DeviceData* dev = dev_of(device);
    {
        std::lock_guard<std::mutex> l(dev->mtx);
        for (uint32_t i = 0; i < n; ++i)
            if (cbs[i]) dev->cmds.erase(cbs[i]);
    }
    dev->d.FreeCommandBuffers(device, pool, n, cbs);
}

static VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    if (pool) {
        std::lock_guard<std::mutex> l(dev->mtx);
        for (auto it = dev->cmds.begin(); it != dev->cmds.end();)
            it = it->second.pool == pool ? dev->cmds.erase(it) : std::next(it);
    }
    dev->d.DestroyCommandPool(device, pool, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    DeviceData* dev = dev_of(cb);
    {
        std::lock_guard<std::mutex> l(dev->mtx);
        auto it = dev->cmds.find(cb);
        if (it != dev->cmds.end()) {
            CmdState& s = it->second;
            VkCommandPool pool = s.pool;
            bool secondary = s.secondary;
            s = CmdState{};
            s.pool = pool;
            s.secondary = secondary;
            s.in_pass = secondary && (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
        }
    }
    return dev->d.BeginCommandBuffer(cb, bi);
}

static void begin_pass(DeviceData* dev, VkCommandBuffer cb, VkImageView depth_view, VkImageLayout layout_after,
                       bool suspending) {
    std::lock_guard<std::mutex> l(dev->mtx);
    auto it = dev->cmds.find(cb);
    if (it == dev->cmds.end()) return;
    CmdState& s = it->second;
    s.in_pass = true;
    s.suspending = suspending;
    s.draws = 0;
    s.depth = VK_NULL_HANDLE;
    if (!depth_view) return;
    auto v = dev->views.find(depth_view);
    if (v == dev->views.end()) return;
    s.depth = v->second.image;
    s.depth_mip = v->second.mip;
    s.depth_layer = v->second.layer;
    s.depth_layout = layout_after;
}

static void begin_render_pass(DeviceData* dev, VkCommandBuffer cb, const VkRenderPassBeginInfo* bi) {
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        std::lock_guard<std::mutex> l(dev->mtx);
        auto rp = dev->render_passes.find(bi->renderPass);
        if (rp != dev->render_passes.end() && rp->second.depth_attachment >= 0) {
            uint32_t idx = static_cast<uint32_t>(rp->second.depth_attachment);
            layout = rp->second.depth_final_layout;
            // Imageless framebuffers pass their views at begin time.
            auto* att = static_cast<const VkRenderPassAttachmentBeginInfo*>(bi->pNext);
            while (att && att->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                att = static_cast<const VkRenderPassAttachmentBeginInfo*>(att->pNext);
            if (att && idx < att->attachmentCount) {
                view = att->pAttachments[idx];
            } else {
                auto fb = dev->framebuffers.find(bi->framebuffer);
                if (fb != dev->framebuffers.end() && idx < fb->second.size()) view = fb->second[idx];
            }
        }
    }
    begin_pass(dev, cb, view, layout, false);
}

static VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi,
                                                     VkSubpassContents contents) {
    DeviceData* dev = dev_of(cb);
    dev->d.CmdBeginRenderPass(cb, bi, contents);
    begin_render_pass(dev, cb, bi);
}

template <PFN_vkCmdBeginRenderPass2 DeviceDispatch::*Next>
static VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi,
                                                      const VkSubpassBeginInfo* sbi) {
    DeviceData* dev = dev_of(cb);
    (dev->d.*Next)(cb, bi, sbi);
    begin_render_pass(dev, cb, bi);
}

template <PFN_vkCmdBeginRendering DeviceDispatch::*Next>
static VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    DeviceData* dev = dev_of(cb);
    (dev->d.*Next)(cb, ri);
    const VkRenderingAttachmentInfo* d = ri->pDepthAttachment;
    bool has = d && d->imageView;
    begin_pass(dev, cb, has ? d->imageView : VK_NULL_HANDLE, has ? d->imageLayout : VK_IMAGE_LAYOUT_UNDEFINED,
               (ri->flags & VK_RENDERING_SUSPENDING_BIT) != 0);
}

// Called right after the game's render pass instance ended. If it rendered to
// the selected depth buffer, append a copy of it into the staging buffer.
static void end_pass(DeviceData* dev, VkCommandBuffer cb) {
    VkImage image;
    VkImageLayout layout;
    uint32_t mip, layer;
    DepthStaging staging;
    VkFormat format;
    {
        std::lock_guard<std::mutex> l(dev->mtx);
        auto it = dev->cmds.find(cb);
        if (it == dev->cmds.end()) return;
        CmdState& s = it->second;
        bool was_in_pass = s.in_pass;
        s.in_pass = false;
        if (!was_in_pass || !s.depth) return;
        dev->depth_draws[s.depth] += s.draws;
        if (s.suspending || s.depth != dev->selected_depth || s.draws < dev->depth_min_draws) return;
        if (s.depth_layout == VK_IMAGE_LAYOUT_UNDEFINED || s.depth_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return;
        if (dev->depth_staging.image != s.depth || s.depth_mip != 0) return;
        image = s.depth;
        layout = s.depth_layout;
        mip = s.depth_mip;
        layer = s.depth_layer;
        staging = dev->depth_staging;
        format = dev->images[image].format;
        dev->depth_copied_since_present = true;
    }

    VkImageAspectFlags barrier_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    bool depth_only_layout = layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
                             layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    if (format_has_stencil(format) && !depth_only_layout) barrier_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {barrier_aspect, mip, 1, layer, 1};
    // TRANSFER in the source scope also orders this copy after the previous
    // frame's present-time read of the staging buffer (write-after-read).
    dev->d.CmdPipelineBarrier(cb,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, mip, layer, 1};
    region.imageExtent = {staging.extent.width, staging.extent.height, 1};
    dev->d.CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);

    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = layout;
    dev->d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &b);
}

static VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer cb) {
    DeviceData* dev = dev_of(cb);
    dev->d.CmdEndRenderPass(cb);
    end_pass(dev, cb);
}

template <PFN_vkCmdEndRenderPass2 DeviceDispatch::*Next>
static VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo* info) {
    DeviceData* dev = dev_of(cb);
    (dev->d.*Next)(cb, info);
    end_pass(dev, cb);
}

template <PFN_vkCmdEndRendering DeviceDispatch::*Next>
static VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer cb) {
    DeviceData* dev = dev_of(cb);
    (dev->d.*Next)(cb);
    end_pass(dev, cb);
}

static inline void count_draw(VkCommandBuffer cb) {
    DeviceData* dev = dev_of(cb);
    std::lock_guard<std::mutex> l(dev->mtx);
    auto it = dev->cmds.find(cb);
    if (it != dev->cmds.end() && it->second.in_pass) {
        it->second.draws++;
        it->second.total_draws++;
    }
}

static VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    count_draw(cb);
    dev_of(cb)->d.CmdDraw(cb, a, b, c, d);
}
static VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d,
                                                 uint32_t e) {
    count_draw(cb);
    dev_of(cb)->d.CmdDrawIndexed(cb, a, b, c, d, e);
}
static VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t n,
                                                  uint32_t stride) {
    count_draw(cb);
    dev_of(cb)->d.CmdDrawIndirect(cb, buf, off, n, stride);
}
static VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t n,
                                                         uint32_t stride) {
    count_draw(cb);
    dev_of(cb)->d.CmdDrawIndexedIndirect(cb, buf, off, n, stride);
}
static VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkBuffer cbuf,
                                                       VkDeviceSize coff, uint32_t max, uint32_t stride) {
    count_draw(cb);
    dev_of(cb)->d.CmdDrawIndirectCount(cb, buf, off, cbuf, coff, max, stride);
}
static VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                                              VkBuffer cbuf, VkDeviceSize coff, uint32_t max,
                                                              uint32_t stride) {
    count_draw(cb);
    dev_of(cb)->d.CmdDrawIndexedIndirectCount(cb, buf, off, cbuf, coff, max, stride);
}

static VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* secondaries) {
    DeviceData* dev = dev_of(cb);
    {
        // Draws recorded in secondaries count towards the primary's pass.
        std::lock_guard<std::mutex> l(dev->mtx);
        auto it = dev->cmds.find(cb);
        if (it != dev->cmds.end() && it->second.in_pass) {
            for (uint32_t i = 0; i < n; ++i) {
                auto s = dev->cmds.find(secondaries[i]);
                if (s != dev->cmds.end()) it->second.draws += s->second.total_draws;
            }
        }
    }
    dev->d.CmdExecuteCommands(cb, n, secondaries);
}

// ---------------------------------------------------------------------------
// Camera analysis hooks (only exposed when the analyzer is enabled)
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* ai,
                                                     const VkAllocationCallbacks* alloc, VkDeviceMemory* mem) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.AllocateMemory(device, ai, alloc, mem);
    if (r == VK_SUCCESS && ai->memoryTypeIndex < dev->mem_props.memoryTypeCount &&
        (dev->mem_props.memoryTypes[ai->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        dev->analyzer->on_allocate(*mem, ai->allocationSize);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks* alloc) {
    DeviceData* dev = dev_of(device);
    std::lock_guard<std::mutex> l(dev->analyzer->region_lock());
    if (mem) dev->analyzer->on_free(mem);
    dev->d.FreeMemory(device, mem, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice device, VkDeviceMemory mem, VkDeviceSize offset,
                                                VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.MapMemory(device, mem, offset, size, flags, data);
    if (r == VK_SUCCESS) dev->analyzer->on_map(mem, offset, size, *data);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice device, VkDeviceMemory mem) {
    DeviceData* dev = dev_of(device);
    std::lock_guard<std::mutex> l(dev->analyzer->region_lock());
    dev->analyzer->on_unmap(mem);
    dev->d.UnmapMemory(device, mem);
}

static VKAPI_ATTR VkResult VKAPI_CALL MapMemory2KHR(VkDevice device, const VkMemoryMapInfoKHR* info, void** data) {
    DeviceData* dev = dev_of(device);
    VkResult r = dev->d.MapMemory2KHR(device, info, data);
    if (r == VK_SUCCESS) dev->analyzer->on_map(info->memory, info->offset, info->size, *data);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL UnmapMemory2KHR(VkDevice device, const VkMemoryUnmapInfoKHR* info) {
    DeviceData* dev = dev_of(device);
    std::lock_guard<std::mutex> l(dev->analyzer->region_lock());
    dev->analyzer->on_unmap(info->memory);
    return dev->d.UnmapMemory2KHR(device, info);
}

static VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                                   uint32_t offset, uint32_t size, const void* values) {
    DeviceData* dev = dev_of(cb);
    std::vector<uint8_t> patched;
    if (dev->analyzer->on_inline_data(values, size, &patched))
        dev->d.CmdPushConstants(cb, layout, stages, offset, size, patched.data());
    else
        dev->d.CmdPushConstants(cb, layout, stages, offset, size, values);
}

static VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                  VkDeviceSize size, const void* data) {
    DeviceData* dev = dev_of(cb);
    std::vector<uint8_t> patched;
    if (dev->analyzer->on_inline_data(data, uint32_t(size), &patched))
        dev->d.CmdUpdateBuffer(cb, buffer, offset, size, patched.data());
    else
        dev->d.CmdUpdateBuffer(cb, buffer, offset, size, data);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    DeviceData* dev = dev_of(cb);
    dev->analyzer->on_viewport(v, n);
    dev->d.CmdSetViewport(cb, first, n, v);
}

static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo* submits, VkFence fence) {
    DeviceData* dev = dev_of(queue);
    dev->analyzer->before_submit();
    return dev->d.QueueSubmit(queue, n, submits, fence);
}

template <PFN_vkQueueSubmit2 DeviceDispatch::*Next>
static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t n, const VkSubmitInfo2* submits,
                                                   VkFence fence) {
    DeviceData* dev = dev_of(queue);
    dev->analyzer->before_submit();
    return (dev->d.*Next)(queue, n, submits, fence);
}

// ---------------------------------------------------------------------------
// Proc address resolution
// ---------------------------------------------------------------------------

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name);

enum Needs { kAlways = 0, kDepth = 1, kAnalyze = 2 };
struct Hook {
    const char* name;
    PFN_vkVoidFunction fn;
    int needs;  // only exposed when that feature is enabled
};

#define HOOK(name, fn, needs) {"vk" name, reinterpret_cast<PFN_vkVoidFunction>(fn), needs}
static const Hook kDeviceHooks[] = {
    HOOK("GetDeviceProcAddr", GetDeviceProcAddr, kAlways),
    HOOK("DestroyDevice", DestroyDevice, kAlways),
    HOOK("GetDeviceQueue", GetDeviceQueue, kAlways),
    HOOK("GetDeviceQueue2", GetDeviceQueue2, kAlways),
    HOOK("CreateSwapchainKHR", CreateSwapchainKHR, kAlways),
    HOOK("DestroySwapchainKHR", DestroySwapchainKHR, kAlways),
    HOOK("QueuePresentKHR", QueuePresentKHR, kAlways),
    HOOK("CreateImage", CreateImage, kDepth),
    HOOK("DestroyImage", DestroyImage, kDepth),
    HOOK("CreateImageView", CreateImageView, kDepth),
    HOOK("DestroyImageView", DestroyImageView, kDepth),
    HOOK("CreateRenderPass", CreateRenderPass, kDepth),
    HOOK("CreateRenderPass2", CreateRenderPass2<&DeviceDispatch::CreateRenderPass2>, kDepth),
    HOOK("CreateRenderPass2KHR", CreateRenderPass2<&DeviceDispatch::CreateRenderPass2KHR>, kDepth),
    HOOK("DestroyRenderPass", DestroyRenderPass, kDepth),
    HOOK("CreateFramebuffer", CreateFramebuffer, kDepth),
    HOOK("DestroyFramebuffer", DestroyFramebuffer, kDepth),
    HOOK("AllocateCommandBuffers", AllocateCommandBuffers, kDepth),
    HOOK("FreeCommandBuffers", FreeCommandBuffers, kDepth),
    HOOK("DestroyCommandPool", DestroyCommandPool, kDepth),
    HOOK("BeginCommandBuffer", BeginCommandBuffer, kDepth),
    HOOK("CmdBeginRenderPass", CmdBeginRenderPass, kDepth),
    HOOK("CmdBeginRenderPass2", CmdBeginRenderPass2<&DeviceDispatch::CmdBeginRenderPass2>, kDepth),
    HOOK("CmdBeginRenderPass2KHR", CmdBeginRenderPass2<&DeviceDispatch::CmdBeginRenderPass2KHR>, kDepth),
    HOOK("CmdEndRenderPass", CmdEndRenderPass, kDepth),
    HOOK("CmdEndRenderPass2", CmdEndRenderPass2<&DeviceDispatch::CmdEndRenderPass2>, kDepth),
    HOOK("CmdEndRenderPass2KHR", CmdEndRenderPass2<&DeviceDispatch::CmdEndRenderPass2KHR>, kDepth),
    HOOK("CmdBeginRendering", CmdBeginRendering<&DeviceDispatch::CmdBeginRendering>, kDepth),
    HOOK("CmdBeginRenderingKHR", CmdBeginRendering<&DeviceDispatch::CmdBeginRenderingKHR>, kDepth),
    HOOK("CmdEndRendering", CmdEndRendering<&DeviceDispatch::CmdEndRendering>, kDepth),
    HOOK("CmdEndRenderingKHR", CmdEndRendering<&DeviceDispatch::CmdEndRenderingKHR>, kDepth),
    HOOK("CmdDraw", CmdDraw, kDepth),
    HOOK("CmdDrawIndexed", CmdDrawIndexed, kDepth),
    HOOK("CmdDrawIndirect", CmdDrawIndirect, kDepth),
    HOOK("CmdDrawIndexedIndirect", CmdDrawIndexedIndirect, kDepth),
    HOOK("CmdDrawIndirectCount", CmdDrawIndirectCount, kDepth),
    HOOK("CmdDrawIndirectCountKHR", CmdDrawIndirectCount, kDepth),
    HOOK("CmdDrawIndexedIndirectCount", CmdDrawIndexedIndirectCount, kDepth),
    HOOK("CmdDrawIndexedIndirectCountKHR", CmdDrawIndexedIndirectCount, kDepth),
    HOOK("CmdExecuteCommands", CmdExecuteCommands, kDepth),
    HOOK("AllocateMemory", AllocateMemory, kAnalyze),
    HOOK("FreeMemory", FreeMemory, kAnalyze),
    HOOK("MapMemory", MapMemory, kAnalyze),
    HOOK("UnmapMemory", UnmapMemory, kAnalyze),
    HOOK("MapMemory2KHR", MapMemory2KHR, kAnalyze),
    HOOK("UnmapMemory2KHR", UnmapMemory2KHR, kAnalyze),
    HOOK("CmdPushConstants", CmdPushConstants, kAnalyze),
    HOOK("CmdUpdateBuffer", CmdUpdateBuffer, kAnalyze),
    HOOK("CmdSetViewport", CmdSetViewport, kAnalyze),
    HOOK("QueueSubmit", QueueSubmit, kAnalyze),
    HOOK("QueueSubmit2", QueueSubmit2<&DeviceDispatch::QueueSubmit2>, kAnalyze),
    HOOK("QueueSubmit2KHR", QueueSubmit2<&DeviceDispatch::QueueSubmit2KHR>, kAnalyze),
};
static const Hook kInstanceHooks[] = {
    HOOK("GetInstanceProcAddr", GetInstanceProcAddr, kAlways),
    HOOK("CreateInstance", CreateInstance, kAlways),
    HOOK("DestroyInstance", DestroyInstance, kAlways),
    HOOK("CreateDevice", CreateDevice, kAlways),
};
#undef HOOK

static const Hook* find_hook(const Hook* hooks, size_t n, const char* name) {
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(hooks[i].name, name)) return &hooks[i];
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    DeviceData* dev = dev_of(device);
    if (!dev) return nullptr;
    PFN_vkVoidFunction next = dev->d.GetDeviceProcAddr(device, name);
    if (!next) return nullptr;  // never expose functions the device lacks
    const Hook* h = find_hook(kDeviceHooks, sizeof(kDeviceHooks) / sizeof(kDeviceHooks[0]), name);
    if (h && (h->needs == kAlways || (h->needs == kDepth && dev->depth_capture) ||
              (h->needs == kAnalyze && dev->analyzer))) {
        // The *IndirectCount KHR aliases call the core pointer; make sure it exists.
        if (!strcmp(name, "vkCmdDrawIndirectCountKHR") && !dev->d.CmdDrawIndirectCount)
            dev->d.CmdDrawIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(next);
        if (!strcmp(name, "vkCmdDrawIndexedIndirectCountKHR") && !dev->d.CmdDrawIndexedIndirectCount)
            dev->d.CmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(next);
        return h->fn;
    }
    return next;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (const Hook* h = find_hook(kInstanceHooks, sizeof(kInstanceHooks) / sizeof(kInstanceHooks[0]), name))
        return h->fn;
    if (!instance) return nullptr;
    InstanceData* inst = get_instance(key_of(instance));
    if (!inst) return nullptr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
    return inst->d.GetInstanceProcAddr(instance, name);
}

}  // namespace vrl

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    if (v->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    v->pfnGetInstanceProcAddr = vrl::GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = vrl::GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
