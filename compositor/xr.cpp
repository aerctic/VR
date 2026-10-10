#include "xr.h"

#include <cstring>

#include "log.h"

namespace vrl {

#define XR_CHECK(call, what)                                              \
    do {                                                                  \
        XrResult r_ = (call);                                             \
        if (XR_FAILED(r_)) {                                              \
            if (error) *error = std::string(what) + " failed (" + std::to_string(int(r_)) + ")"; \
            return false;                                                 \
        }                                                                 \
    } while (0)

template <typename T>
static bool load_fn(XrInstance inst, const char* name, T* out) {
    return XR_SUCCEEDED(xrGetInstanceProcAddr(inst, name, reinterpret_cast<PFN_xrVoidFunction*>(out))) && *out;
}

bool XrContext::init(std::string* error) {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool vk2 = false;
    for (auto& e : exts) {
        if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) vk2 = true;
        if (!strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) cylinder_ = true;
    }
    if (!vk2) {
        if (error) *error = "OpenXR runtime lacks XR_KHR_vulkan_enable2 (is an OpenXR runtime such as Monado, "
                            "WiVRn or SteamVR installed and active?)";
        return false;
    }
    std::vector<const char*> enable{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    if (cylinder_) enable.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "VRLayer");
    snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "VRLayer");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = uint32_t(enable.size());
    ici.enabledExtensionNames = enable.data();
    XR_CHECK(xrCreateInstance(&ici, &instance_), "xrCreateInstance");

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &props))) runtime_name_ = props.runtimeName;

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &sgi, &system_), "xrGetSystem (is the headset connected?)");

    PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device = nullptr;
    if (!load_fn(instance_, "xrGetVulkanGraphicsRequirements2KHR", &get_reqs) ||
        !load_fn(instance_, "xrCreateVulkanInstanceKHR", &create_instance) ||
        !load_fn(instance_, "xrGetVulkanGraphicsDevice2KHR", &get_device) ||
        !load_fn(instance_, "xrCreateVulkanDeviceKHR", &create_device)) {
        if (error) *error = "missing XR_KHR_vulkan_enable2 entry points";
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(get_reqs(instance_, system_, &reqs), "xrGetVulkanGraphicsRequirements2KHR");

    // --- Vulkan instance
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VRLayer";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvici.systemId = system_;
    xvici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xvici.vulkanCreateInfo = &vici;
    VkResult vr;
    XR_CHECK(create_instance(instance_, &xvici, &gpu_.instance, &vr), "xrCreateVulkanInstanceKHR");
    if (vr != VK_SUCCESS) {
        if (error) *error = "vkCreateInstance via OpenXR failed";
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = system_;
    gdi.vulkanInstance = gpu_.instance;
    XR_CHECK(get_device(instance_, &gdi, &gpu_.physical), "xrGetVulkanGraphicsDevice2KHR");

    // --- Vulkan device
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_.physical, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_.physical, &nq, fams.data());
    gpu_.family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            gpu_.family = i;
            break;
        }
    if (gpu_.family == UINT32_MAX) {
        if (error) *error = "no graphics+compute queue family";
        return false;
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(gpu_.physical, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> dev_exts(ne);
    vkEnumerateDeviceExtensionProperties(gpu_.physical, nullptr, &ne, dev_exts.data());
    std::vector<const char*> enable_dev;
    for (const char* want : Gpu::wanted_device_extensions())
        for (auto& e : dev_exts)
            if (!strcmp(e.extensionName, want)) enable_dev.push_back(want);
    gpu_.external_memory_fd = !enable_dev.empty();

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gpu_.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(enable_dev.size());
    dci.ppEnabledExtensionNames = enable_dev.data();
    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = system_;
    xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = gpu_.physical;
    xdci.vulkanCreateInfo = &dci;
    XR_CHECK(create_device(instance_, &xdci, &gpu_.device, &vr), "xrCreateVulkanDeviceKHR");
    if (vr != VK_SUCCESS) {
        if (error) *error = "vkCreateDevice via OpenXR failed";
        return false;
    }
    vkGetDeviceQueue(gpu_.device, gpu_.family, 0, &gpu_.queue);
    gpu_.query_properties();

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(gpu_.physical, &pdp);
    VRL_LOG("xr", "runtime '%s', GPU '%s', cylinder layers %s", runtime_name_.c_str(), pdp.deviceName,
            cylinder_ ? "yes" : "no");

    // --- session + spaces
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = gpu_.instance;
    binding.physicalDevice = gpu_.physical;
    binding.device = gpu_.device;
    binding.queueFamilyIndex = gpu_.family;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO, &binding};
    sci.systemId = system_;
    XR_CHECK(xrCreateSession(instance_, &sci, &session_), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(session_, &rsci, &local_), "xrCreateReferenceSpace(LOCAL)");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(session_, &rsci, &view_), "xrCreateReferenceSpace(VIEW)");
    return true;
}

void XrContext::destroy() {
    if (gpu_.device) vkDeviceWaitIdle(gpu_.device);
    destroy_swapchain();
    if (view_) xrDestroySpace(view_);
    if (local_) xrDestroySpace(local_);
    if (session_) xrDestroySession(session_);
    if (gpu_.device) vkDestroyDevice(gpu_.device, nullptr);
    if (gpu_.instance) vkDestroyInstance(gpu_.instance, nullptr);
    if (instance_) xrDestroyInstance(instance_);
    view_ = local_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    gpu_ = Gpu{};
}

void XrContext::poll_events(bool* quit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                state_ = e->state;
                VRL_DBG("xr", "session state %d", int(state_));
                if (state_ == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) running_ = true;
                } else if (state_ == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    running_ = false;
                } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                    *quit = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: *quit = true; break;
            default: break;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void XrContext::request_exit() {
    if (session_ && running_) xrRequestExitSession(session_);
}

bool XrContext::wait_frame(XrFrameState* fs) {
    *fs = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    return XR_SUCCEEDED(xrWaitFrame(session_, &wi, fs));
}

bool XrContext::begin_frame() {
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    return XR_SUCCEEDED(xrBeginFrame(session_, &bi));
}

bool XrContext::end_frame(XrTime t, const std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = t;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = uint32_t(layers.size());
    ei.layers = layers.data();
    return XR_SUCCEEDED(xrEndFrame(session_, &ei));
}

bool XrContext::locate_head(XrTime t, XrPosef* pose) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(view_, local_, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    *pose = loc.pose;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) pose->position = {0, 0, 0};
    return true;
}

void XrContext::destroy_swapchain() {
    if (swapchain_) xrDestroySwapchain(swapchain_);
    swapchain_ = XR_NULL_HANDLE;
    images_.clear();
    sw_w_ = sw_h_ = 0;
}

bool XrContext::ensure_swapchain(uint32_t w, uint32_t h) {
    if (swapchain_ && w == sw_w_ && h == sw_h_) return true;
    vkQueueWaitIdle(gpu_.queue);
    destroy_swapchain();

    if (!sw_format_) {
        uint32_t n = 0;
        xrEnumerateSwapchainFormats(session_, 0, &n, nullptr);
        std::vector<int64_t> formats(n);
        xrEnumerateSwapchainFormats(session_, n, &n, formats.data());
        const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT};
        for (int64_t p : preferred) {
            for (int64_t f : formats)
                if (f == p) sw_format_ = f;
            if (sw_format_) break;
        }
        if (!sw_format_ && !formats.empty()) sw_format_ = formats[0];
        VRL_DBG("xr", "swapchain format %lld", static_cast<long long>(sw_format_));
    }

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = sw_format_;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session_, &ci, &swapchain_))) {
        VRL_LOG("xr", "xrCreateSwapchain %ux%u failed", w, h);
        swapchain_ = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
    sw_w_ = w;
    sw_h_ = h;
    return true;
}

VkImage XrContext::acquire_image() {
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return VK_NULL_HANDLE;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wi))) return VK_NULL_HANDLE;
    return images_[idx].image;
}

void XrContext::release_image() {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);
}

}  // namespace vrl
