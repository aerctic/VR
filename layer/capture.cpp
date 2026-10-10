#include "capture.h"

#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "ipc.h"
#include "log.h"

namespace vrl {

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

uint32_t color_bytes_per_pixel(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        default: return 0;
    }
}

static std::string exe_name() {
    // argv[0] works for both native games and Wine (which shows the .exe path).
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return "unknown";
    char buf[512] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    std::string s(buf);
    size_t cut = s.find_last_of("/\\");
    return cut == std::string::npos ? s : s.substr(cut + 1);
}

static uint32_t steam_appid() {
    for (const char* var : {"SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID"}) {
        const char* v = getenv(var);
        if (v && *v) {
            unsigned long id = strtoul(v, nullptr, 10);
            if (id && id < 0xFFFFFFFFul) return static_cast<uint32_t>(id);
        }
    }
    return 0;
}

Capture::Capture(DeviceData* dev) : dev_(dev) {
    worker_ = std::thread(&Capture::worker_main, this);
}

Capture::~Capture() {
    wait_idle();
    {
        std::lock_guard<std::mutex> l(job_mtx_);
        stop_ = true;
    }
    job_cv_.notify_all();
    worker_.join();
    destroy_slots();
    std::lock_guard<std::mutex> l(io_mtx_);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void Capture::disconnect() {
    wait_idle();
    {
        std::lock_guard<std::mutex> l(io_mtx_);
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
    if (conn_ == Conn::Ready) VRL_LOG("layer", "compositor disconnected");
    conn_ = Conn::Disconnected;
    broken_ = false;
    destroy_slots();
}

void Capture::poll_connection() {
    if (broken_) disconnect();

    if (conn_ == Conn::Disconnected) {
        uint64_t now = now_ns();
        if (now - last_attempt_ns_ < 1000000000ull) return;
        last_attempt_ns_ = now;
        int fd = ipc_connect(socket_name());
        if (fd < 0) return;

        auto hello = make_msg<HelloMsg>(MsgType::Hello);
        hello.version = kProtocolVersion;
        hello.pid = getpid();
        hello.appid = steam_appid();
        hello.has_uuid = dev_->has_uuid;
        memcpy(hello.device_uuid, dev_->device_uuid, VK_UUID_SIZE);
        memcpy(hello.driver_uuid, dev_->driver_uuid, VK_UUID_SIZE);
        bool gpu_ok = dev_->external_memory_fd && dev_->has_uuid;
        const char* t = getenv("VRLAYER_TRANSPORT");
        if (!gpu_ok || (t && !strcmp(t, "cpu"))) hello.requested_transport = uint32_t(Transport::Cpu);
        else if (t && !strcmp(t, "gpu")) hello.requested_transport = uint32_t(Transport::Gpu);
        snprintf(hello.exe, sizeof(hello.exe), "%s", exe_name().c_str());
        if (!ipc_send(fd, &hello, sizeof(hello))) {
            close(fd);
            return;
        }
        std::lock_guard<std::mutex> l(io_mtx_);
        fd_ = fd;
        conn_ = Conn::AwaitWelcome;
    }

    alignas(8) char buf[kMaxMsgSize];
    for (;;) {
        ssize_t n = ipc_recv(fd_, buf, sizeof(buf), nullptr);
        if (n == kIpcWouldBlock) break;
        if (n <= 0) {
            disconnect();
            return;
        }
        auto* h = reinterpret_cast<MsgHeader*>(buf);
        if (size_t(n) < sizeof(MsgHeader) || h->magic != kMagic) continue;
        switch (MsgType(h->type)) {
            case MsgType::Welcome: {
                if (size_t(n) < sizeof(WelcomeMsg)) break;
                auto* w = reinterpret_cast<WelcomeMsg*>(buf);
                transport_ = Transport(w->transport);
                if (transport_ == Transport::Gpu && !dev_->external_memory_fd) transport_ = Transport::Cpu;
                conn_ = Conn::Ready;
                slot_count_ = 0;  // force (re)creation + Slots message
                ++connection_generation_;  // a new compositor may accept what the old one did not
                VRL_LOG("layer", "connected to compositor (%s transport)",
                        transport_ == Transport::Gpu ? "gpu" : "cpu");
                break;
            }
            case MsgType::Release: {
                if (size_t(n) < sizeof(ReleaseMsg)) break;
                auto* r = reinterpret_cast<ReleaseMsg*>(buf);
                std::lock_guard<std::mutex> l(slot_mtx_);
                if (r->generation == generation_ && r->slot < slot_count_ && slots_[r->slot].state == SlotState::Sent)
                    slots_[r->slot].state = SlotState::Free;
                break;
            }
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

bool Capture::create_gpu_slot(Slot& s, VkFormat format, VkExtent2D extent, VkDeviceSize depth_size) {
    VkDevice d = dev_->device;
    VkExternalMemoryImageCreateInfo ext_img{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext_img};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = kSlotImageUsage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dev_->d.CreateImage(d, &ici, nullptr, &s.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    dev_->d.GetImageMemoryRequirements(d, s.image, &req);
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = s.image;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded};
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(dev_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (dev_->d.AllocateMemory(d, &mai, nullptr, &s.image_mem) != VK_SUCCESS) return false;
    if (dev_->d.BindImageMemory(d, s.image, s.image_mem, 0) != VK_SUCCESS) return false;
    color_size_ = req.size;
    color_mem_type_ = mai.memoryTypeIndex;

    if (depth_size) {
        VkExternalMemoryBufferCreateInfo ext_buf{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext_buf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext_buf};
        bci.size = depth_size;
        bci.usage = kSlotDepthBufferUsage;
        if (dev_->d.CreateBuffer(d, &bci, nullptr, &s.depth) != VK_SUCCESS) return false;
        dev_->d.GetBufferMemoryRequirements(d, s.depth, &req);
        VkMemoryDedicatedAllocateInfo bded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        bded.buffer = s.depth;
        VkExportMemoryAllocateInfo bexp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &bded};
        bexp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &bexp};
        bmai.allocationSize = req.size;
        bmai.memoryTypeIndex = find_memory_type(dev_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (bmai.memoryTypeIndex == UINT32_MAX) return false;
        if (dev_->d.AllocateMemory(d, &bmai, nullptr, &s.depth_mem) != VK_SUCCESS) return false;
        if (dev_->d.BindBufferMemory(d, s.depth, s.depth_mem, 0) != VK_SUCCESS) return false;
        depth_alloc_size_ = req.size;
        depth_mem_type_ = bmai.memoryTypeIndex;
    }
    return true;
}

bool Capture::create_cpu_slot(Slot& s, VkDeviceSize color_size, VkDeviceSize depth_size) {
    VkDevice d = dev_->device;
    VkDeviceSize total = color_size + depth_size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (dev_->d.CreateBuffer(d, &bci, nullptr, &s.readback) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    dev_->d.GetBufferMemoryRequirements(d, s.readback, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(
        dev_, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = find_memory_type(dev_, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (dev_->d.AllocateMemory(d, &mai, nullptr, &s.readback_mem) != VK_SUCCESS) return false;
    if (dev_->d.BindBufferMemory(d, s.readback, s.readback_mem, 0) != VK_SUCCESS) return false;
    if (dev_->d.MapMemory(d, s.readback_mem, 0, VK_WHOLE_SIZE, 0, &s.readback_ptr) != VK_SUCCESS) return false;
    s.readback_coherent =
        dev_->mem_props.memoryTypes[mai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    s.shm_fd = memfd_create("vrlayer-frame", MFD_CLOEXEC);
    if (s.shm_fd < 0 || ftruncate(s.shm_fd, off_t(total)) != 0) return false;
    s.shm = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, s.shm_fd, 0);
    if (s.shm == MAP_FAILED) {
        s.shm = nullptr;
        return false;
    }
    return true;
}

void Capture::destroy_slots() {
    VkDevice d = dev_->device;
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        Slot& s = slots_[i];
        if (s.fence) dev_->d.WaitForFences(d, 1, &s.fence, VK_TRUE, UINT64_MAX);
        if (s.image) dev_->d.DestroyImage(d, s.image, nullptr);
        if (s.image_mem) dev_->d.FreeMemory(d, s.image_mem, nullptr);
        if (s.depth) dev_->d.DestroyBuffer(d, s.depth, nullptr);
        if (s.depth_mem) dev_->d.FreeMemory(d, s.depth_mem, nullptr);
        if (s.readback) dev_->d.DestroyBuffer(d, s.readback, nullptr);
        if (s.readback_mem) dev_->d.FreeMemory(d, s.readback_mem, nullptr);
        if (s.shm) munmap(s.shm, size_t(color_size_ + depth_size_));
        if (s.shm_fd >= 0) close(s.shm_fd);
        if (s.pool) dev_->d.DestroyCommandPool(d, s.pool, nullptr);
        if (s.fence) dev_->d.DestroyFence(d, s.fence, nullptr);
        s = Slot{};
    }
    slot_count_ = 0;
}

bool Capture::create_slots(VkFormat format, VkExtent2D extent, const DepthStaging& depth) {
    wait_idle();
    destroy_slots();
    ++generation_;

    format_ = format;
    extent_ = extent;
    depth_format_ = depth.buffer ? depth.format : DepthFormat::None;
    depth_extent_ = depth.buffer ? depth.extent : VkExtent2D{0, 0};
    depth_size_ = depth.buffer ? depth.size : 0;
    depth_alloc_size_ = 0;

    slot_transport_ = transport_;
    if (slot_transport_ == Transport::Gpu && dev_->instance->d.GetPhysicalDeviceImageFormatProperties2) {
        // Check the image can be exported as an opaque fd.
        VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext};
        fi.format = format;
        fi.type = VK_IMAGE_TYPE_2D;
        fi.tiling = VK_IMAGE_TILING_OPTIMAL;
        fi.usage = kSlotImageUsage;
        VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ext_props};
        VkResult r = dev_->instance->d.GetPhysicalDeviceImageFormatProperties2(dev_->physical, &fi, &props);
        if (r != VK_SUCCESS || !(ext_props.externalMemoryProperties.externalMemoryFeatures &
                                 VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
            VRL_LOG("layer", "format %d not exportable, falling back to cpu transport", format);
            slot_transport_ = Transport::Cpu;
        }
    } else if (slot_transport_ == Transport::Gpu) {
        slot_transport_ = Transport::Cpu;
    }

    uint32_t bpp = color_bytes_per_pixel(format);
    if (slot_transport_ == Transport::Cpu && bpp == 0) {
        if (!warned_format_) VRL_LOG("layer", "swapchain format %d unsupported for cpu transport", format);
        warned_format_ = true;
        return false;
    }

    if (slot_transport_ == Transport::Cpu) color_size_ = VkDeviceSize(extent.width) * extent.height * bpp;
    uint32_t count = kMaxSlots;
    for (uint32_t i = 0; i < count; ++i) {
        Slot& s = slots_[i];
        bool ok = slot_transport_ == Transport::Gpu
                      ? create_gpu_slot(s, format, extent, depth_size_)
                      : create_cpu_slot(s, VkDeviceSize(extent.width) * extent.height * bpp, depth_size_);
        if (ok) {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            ok = dev_->d.CreateFence(dev_->device, &fci, nullptr, &s.fence) == VK_SUCCESS;
        }
        if (!ok) {
            VRL_LOG("layer", "failed to create frame slot %u", i);
            slot_count_ = kMaxSlots;
            destroy_slots();
            return false;
        }
    }
    slot_count_ = count;
    if (!send_slots()) {
        broken_ = true;
        return false;
    }
    VRL_LOG("layer", "streaming %ux%u (format %d) depth %s via %s", extent.width, extent.height, format,
            depth_format_ == DepthFormat::None ? "none" : "yes", slot_transport_ == Transport::Gpu ? "gpu" : "cpu");
    return true;
}

bool Capture::send_slots() {
    auto m = make_msg<SlotsMsg>(MsgType::Slots);
    m.generation = generation_;
    m.transport = uint32_t(slot_transport_);
    m.slot_count = slot_count_;
    m.width = extent_.width;
    m.height = extent_.height;
    m.color_format = uint32_t(format_);
    m.color_memory_type = color_mem_type_;
    m.color_alloc_size = color_size_;
    m.depth_format = uint32_t(depth_format_);
    m.depth_width = depth_extent_.width;
    m.depth_height = depth_extent_.height;
    m.depth_memory_type = depth_mem_type_;
    m.depth_alloc_size = slot_transport_ == Transport::Gpu ? depth_alloc_size_ : depth_size_;

    int fds[2 * kMaxSlots];
    int nfds = 0;
    bool ok = true;
    for (uint32_t i = 0; i < slot_count_ && ok; ++i) {
        Slot& s = slots_[i];
        if (slot_transport_ == Transport::Gpu) {
            VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
            gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            gi.memory = s.image_mem;
            ok = dev_->d.GetMemoryFdKHR(dev_->device, &gi, &fds[nfds]) == VK_SUCCESS;
            if (ok) ++nfds;
            if (ok && depth_format_ != DepthFormat::None) {
                gi.memory = s.depth_mem;
                ok = dev_->d.GetMemoryFdKHR(dev_->device, &gi, &fds[nfds]) == VK_SUCCESS;
                if (ok) ++nfds;
            }
        } else {
            // One memfd holds color followed by depth; send it twice so both
            // transports share the same fd layout.
            fds[nfds++] = dup(s.shm_fd);
            if (depth_format_ != DepthFormat::None) fds[nfds++] = dup(s.shm_fd);
        }
    }
    if (ok) {
        std::lock_guard<std::mutex> l(io_mtx_);
        ok = fd_ >= 0 && ipc_send(fd_, &m, sizeof(m), fds, nfds);
    }
    for (int i = 0; i < nfds; ++i) close(fds[i]);
    return ok;
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------

bool Capture::on_present(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* wait) {
    if (info->swapchainCount == 0) return false;
    poll_connection();
    if (conn_ != Conn::Ready) return false;

    VkImage sc_image;
    VkExtent2D extent;
    VkFormat format;
    uint64_t serial;
    VkSemaphore signal;
    uint32_t family;
    DepthStaging depth{};
    bool depth_valid = false;
    {
        std::lock_guard<std::mutex> l(dev_->mtx);
        auto it = dev_->swapchains.find(info->pSwapchains[0]);
        if (it == dev_->swapchains.end()) return false;
        SwapchainData& sc = it->second;
        uint32_t idx = info->pImageIndices[0];
        if (idx >= sc.images.size()) return false;
        auto qf = dev_->queue_family.find(queue);
        if (qf == dev_->queue_family.end()) return false;
        family = qf->second;
        if (!(dev_->families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)))
            return false;
        if (!sc.copy_done[idx]) {
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (dev_->d.CreateSemaphore(dev_->device, &sci, nullptr, &sc.copy_done[idx]) != VK_SUCCESS) return false;
        }
        sc_image = sc.images[idx];
        extent = sc.extent;
        format = sc.format;
        serial = sc.serial;
        signal = sc.copy_done[idx];
        if (dev_->depth_capture && dev_->depth_staging.buffer) {
            depth = dev_->depth_staging;
            depth_valid = dev_->frames_since_depth_copy <= 2;
        }
    }

    bool depth_changed = (depth.buffer ? depth.format : DepthFormat::None) != depth_format_ ||
                         (depth.buffer && (depth.extent.width != depth_extent_.width ||
                                           depth.extent.height != depth_extent_.height));
    if (slot_count_ == 0 || serial != sc_serial_ || format != format_ || extent.width != extent_.width ||
        extent.height != extent_.height || depth_changed) {
        const DepthFormat want_depth = depth.buffer ? depth.format : DepthFormat::None;
        if (have_failed_ && failed_.serial == serial && failed_.format == format &&
            failed_.extent.width == extent.width && failed_.extent.height == extent.height &&
            failed_.depth == want_depth && failed_.generation == connection_generation_)
            return false;  // already failed for exactly this setup
        sc_serial_ = serial;
        if (!create_slots(format, extent, depth)) {
            have_failed_ = true;
            failed_ = {serial, format, extent, want_depth, connection_generation_};
            return false;
        }
        have_failed_ = false;
    }

    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> l(slot_mtx_);
        for (uint32_t i = 0; i < slot_count_; ++i) {
            if (slots_[i].state == SlotState::Free) {
                slot = &slots_[i];
                slot->state = SlotState::Copying;
                break;
            }
        }
    }
    if (!slot) return false;  // compositor still holds every slot: skip this frame

    slot->has_depth = depth_valid && depth_format_ != DepthFormat::None;
    slot->frame_index = ++frame_counter_;
    if (!record_and_submit(*slot, queue, family, sc_image, slot->has_depth ? &depth : nullptr, info, signal)) {
        std::lock_guard<std::mutex> l(slot_mtx_);
        slot->state = SlotState::Free;
        return false;
    }

    {
        std::lock_guard<std::mutex> l(job_mtx_);
        jobs_.push_back(Job{uint32_t(slot - slots_), generation_});
    }
    job_cv_.notify_one();
    *wait = signal;
    return true;
}

bool Capture::record_and_submit(Slot& s, VkQueue queue, uint32_t family, VkImage sc_image, const DepthStaging* depth,
                                const VkPresentInfoKHR* info, VkSemaphore signal) {
    VkDevice d = dev_->device;
    if (s.pool_family != family) {
        if (s.pool) dev_->d.DestroyCommandPool(d, s.pool, nullptr);
        s.pool = VK_NULL_HANDLE;
        s.cmd = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        if (dev_->d.CreateCommandPool(d, &pci, nullptr, &s.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (dev_->d.AllocateCommandBuffers(d, &ai, &s.cmd) != VK_SUCCESS) return false;
        if (dev_->set_loader_data) dev_->set_loader_data(d, s.cmd);
        s.pool_family = family;
    }

    dev_->d.ResetFences(d, 1, &s.fence);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_->d.BeginCommandBuffer(s.cmd, &bi);

    const bool gpu = slot_transport_ == Transport::Gpu;
    const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // --- acquire: swapchain image -> TRANSFER_SRC, slot image -> TRANSFER_DST
    VkImageMemoryBarrier pre[2] = {};
    uint32_t npre = 0;
    pre[npre] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre[npre].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    pre[npre].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre[npre].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pre[npre].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[npre].srcQueueFamilyIndex = pre[npre].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[npre].image = sc_image;
    pre[npre].subresourceRange = color_range;
    ++npre;
    if (gpu) {
        pre[npre] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        pre[npre].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pre[npre].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pre[npre].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pre[npre].srcQueueFamilyIndex = pre[npre].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre[npre].image = s.image;
        pre[npre].subresourceRange = color_range;
        ++npre;
    }
    VkBufferMemoryBarrier depth_pre{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    if (depth) {
        depth_pre.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        depth_pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        depth_pre.srcQueueFamilyIndex = depth_pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_pre.buffer = depth->buffer;
        depth_pre.size = VK_WHOLE_SIZE;
    }
    dev_->d.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                               depth ? 1 : 0, &depth_pre, npre, pre);

    // --- copies
    if (gpu) {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {extent_.width, extent_.height, 1};
        dev_->d.CmdCopyImage(s.cmd, sc_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (depth) {
            VkBufferCopy bc{0, 0, depth_size_};
            dev_->d.CmdCopyBuffer(s.cmd, depth->buffer, s.depth, 1, &bc);
        }
    } else {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        dev_->d.CmdCopyImageToBuffer(s.cmd, sc_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback, 1, &region);
        if (depth) {
            VkBufferCopy bc{0, color_size_, depth_size_};
            dev_->d.CmdCopyBuffer(s.cmd, depth->buffer, s.readback, 1, &bc);
        }
    }

    // --- release: swapchain image back to PRESENT_SRC, slot resources to the compositor
    VkImageMemoryBarrier post[2] = {};
    uint32_t npost = 0;
    post[npost] = pre[0];
    post[npost].srcAccessMask = 0;
    post[npost].dstAccessMask = 0;
    post[npost].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post[npost].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ++npost;
    VkBufferMemoryBarrier buf_post{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    uint32_t nbuf_post = 0;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (gpu) {
        post[npost] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        post[npost].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        post[npost].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post[npost].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        post[npost].srcQueueFamilyIndex = family;
        post[npost].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        post[npost].image = s.image;
        post[npost].subresourceRange = color_range;
        ++npost;
        if (depth) {
            buf_post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            buf_post.srcQueueFamilyIndex = family;
            buf_post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            buf_post.buffer = s.depth;
            buf_post.size = VK_WHOLE_SIZE;
            nbuf_post = 1;
        }
    } else {
        buf_post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buf_post.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buf_post.srcQueueFamilyIndex = buf_post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf_post.buffer = s.readback;
        buf_post.size = VK_WHOLE_SIZE;
        nbuf_post = 1;
        dst_stage |= VK_PIPELINE_STAGE_HOST_BIT;
    }
    dev_->d.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0, 0, nullptr, nbuf_post, &buf_post,
                               npost, post);
    if (dev_->d.EndCommandBuffer(s.cmd) != VK_SUCCESS) return false;

    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signal;
    if (dev_->d.QueueSubmit(queue, 1, &si, s.fence) != VK_SUCCESS) {
        // The fence was reset; signal it again so wait_idle() never hangs.
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        dev_->d.QueueSubmit(queue, 1, &empty, s.fence);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Worker: waits for the copy to land, then announces the frame
// ---------------------------------------------------------------------------

void Capture::wait_idle() {
    std::unique_lock<std::mutex> l(job_mtx_);
    idle_cv_.wait(l, [&] { return jobs_.empty() && !worker_busy_; });
}

void Capture::worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(job_mtx_);
            job_cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;  // stop requested and drained
            job = jobs_.front();
            jobs_.pop_front();
            worker_busy_ = true;
        }

        Slot& s = slots_[job.slot];
        VkResult r = dev_->d.WaitForFences(dev_->device, 1, &s.fence, VK_TRUE, 5000000000ull);
        bool ok = r == VK_SUCCESS;

        if (ok && slot_transport_ == Transport::Cpu) {
            if (!s.readback_coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = s.readback_mem;
                range.size = VK_WHOLE_SIZE;
                dev_->d.InvalidateMappedMemoryRanges(dev_->device, 1, &range);
            }
            memcpy(s.shm, s.readback_ptr, size_t(color_size_ + (s.has_depth ? depth_size_ : 0)));
        }

        if (ok) {
            auto m = make_msg<FrameMsg>(MsgType::Frame);
            m.generation = job.generation;
            m.slot = job.slot;
            m.frame_index = s.frame_index;
            m.timestamp_ns = now_ns();
            m.has_depth = s.has_depth;
            std::lock_guard<std::mutex> l(io_mtx_);
            ok = fd_ >= 0 && ipc_send(fd_, &m, sizeof(m));
            if (!ok) broken_ = true;
        }

        {
            std::lock_guard<std::mutex> l(slot_mtx_);
            s.state = ok ? SlotState::Sent : SlotState::Free;
        }
        {
            std::lock_guard<std::mutex> l(job_mtx_);
            worker_busy_ = false;
        }
        idle_cv_.notify_all();
    }
}

}  // namespace vrl
