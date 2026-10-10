#include "frame_server.h"

#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

#include "ipc.h"
#include "log.h"

namespace vrl {

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct FrameServer::Client {
    int id = 0;
    int fd = -1;
    bool control = false;
    bool hello = false;
    ClientInfo info;

    bool have_slots = false;
    SlotsMsg slots{};
    struct Imported {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer depth = VK_NULL_HANDLE;
        VkDeviceMemory depth_mem = VK_NULL_HANDLE;
        void* shm = nullptr;
        size_t shm_size = 0;
    };
    std::vector<Imported> imported;
    // Cpu transport: frames are uploaded into these.
    GpuImage local;
    GpuBuffer local_depth;
    GpuBuffer staging;

    bool pending = false;
    FrameMsg pending_msg{};
    uint64_t last_frame_ns = 0;
};

FrameServer::FrameServer(Gpu* gpu) : gpu_(gpu) {}

FrameServer::~FrameServer() {
    if (gpu_->queue) vkQueueWaitIdle(gpu_->queue);
    while (!clients_.empty()) drop_client(clients_.size() - 1);
    if (listen_fd_ >= 0) close(listen_fd_);
}

bool FrameServer::listen(const std::string& name) {
    listen_fd_ = ipc_listen(name);
    return listen_fd_ >= 0;
}

FrameServer::Client* FrameServer::find(int id) {
    for (auto& c : clients_)
        if (c->id == id) return c.get();
    return nullptr;
}

const ClientInfo* FrameServer::active() const {
    for (auto& c : clients_)
        if (c->id == active_id_) return &c->info;
    return nullptr;
}

std::vector<ClientInfo> FrameServer::clients() const {
    std::vector<ClientInfo> out;
    for (auto& c : clients_)
        if (c->hello) out.push_back(c->info);
    return out;
}

bool FrameServer::has_new_frame() const {
    for (auto& c : clients_)
        if (c->id == active_id_) return c->pending;
    return false;
}

void FrameServer::accept_clients() {
    for (;;) {
        int fd = ipc_accept(listen_fd_);
        if (fd < 0) return;
        auto c = std::make_unique<Client>();
        c->id = next_id_++;
        c->fd = fd;
        c->info.id = c->id;
        clients_.push_back(std::move(c));
    }
}

void FrameServer::poll(int timeout_ms) {
    if (timeout_ms > 0) {
        std::vector<pollfd> pfds;
        if (listen_fd_ >= 0) pfds.push_back({listen_fd_, POLLIN, 0});
        for (auto& c : clients_) pfds.push_back({c->fd, POLLIN, 0});
        ::poll(pfds.data(), pfds.size(), timeout_ms);
    }
    if (listen_fd_ >= 0) accept_clients();

    alignas(8) char buf[kMaxMsgSize];
    for (size_t i = 0; i < clients_.size();) {
        Client& c = *clients_[i];
        bool alive = true;
        for (;;) {
            std::vector<int> fds;
            ssize_t n = ipc_recv(c.fd, buf, sizeof(buf), &fds);
            if (n == kIpcWouldBlock) break;
            if (n <= 0 || !handle_message(c, buf, size_t(n), fds)) {
                close_fds(fds);
                alive = false;
                break;
            }
            close_fds(fds);  // anything not consumed by the handler
        }
        if (alive) ++i;
        else drop_client(i);
    }
    update_active();
}

bool FrameServer::handle_message(Client& c, const char* buf, size_t n, std::vector<int>& fds) {
    if (n < sizeof(MsgHeader)) return false;
    auto* h = reinterpret_cast<const MsgHeader*>(buf);
    if (h->magic != kMagic) return false;

    switch (MsgType(h->type)) {
        case MsgType::Hello: {
            if (n < sizeof(HelloMsg)) return false;
            auto* m = reinterpret_cast<const HelloMsg*>(buf);
            if (m->version != kProtocolVersion) {
                VRL_LOG("server", "client speaks protocol %u, expected %u", m->version, kProtocolVersion);
                return false;
            }
            c.hello = true;
            c.info.pid = m->pid;
            c.info.appid = m->appid;
            c.info.exe.assign(m->exe, strnlen(m->exe, sizeof(m->exe)));

            bool same_gpu = m->has_uuid && !memcmp(m->device_uuid, gpu_->device_uuid, VK_UUID_SIZE) &&
                            !memcmp(m->driver_uuid, gpu_->driver_uuid, VK_UUID_SIZE);
            Transport t = (same_gpu && gpu_->external_memory_fd) ? Transport::Gpu : Transport::Cpu;
            if (m->requested_transport == uint32_t(Transport::Cpu)) t = Transport::Cpu;
            if (m->requested_transport == uint32_t(Transport::Gpu) && same_gpu && gpu_->external_memory_fd)
                t = Transport::Gpu;
            auto w = make_msg<WelcomeMsg>(MsgType::Welcome);
            w.transport = uint32_t(t);
            VRL_LOG("server", "game connected: %s (pid %d, appid %u), %s transport%s", c.info.exe.c_str(), c.info.pid,
                    c.info.appid, t == Transport::Gpu ? "gpu" : "cpu",
                    same_gpu || !m->has_uuid ? "" : " (game runs on a different GPU)");
            return ipc_send(c.fd, &w, sizeof(w));
        }
        case MsgType::Slots: {
            if (n < sizeof(SlotsMsg) || !c.hello) return false;
            auto* m = reinterpret_cast<const SlotsMsg*>(buf);
            if (gpu_->queue) vkQueueWaitIdle(gpu_->queue);
            free_imports(c);
            // Frames of the previous generation are gone.
            c.pending = false;
            for (size_t i = 0; i < in_flight_.size();)
                if (in_flight_[i].client_id == c.id) in_flight_.erase(in_flight_.begin() + i);
                else ++i;
            if (!import_slots(c, *m, fds)) {
                VRL_LOG("server", "failed to import frame slots from %s", c.info.exe.c_str());
                free_imports(c);
                return true;  // keep the connection, maybe the next generation works
            }
            return true;
        }
        case MsgType::Frame: {
            if (n < sizeof(FrameMsg) || !c.have_slots) return true;
            auto* m = reinterpret_cast<const FrameMsg*>(buf);
            if (m->generation != c.slots.generation || m->slot >= c.slots.slot_count) return true;
            if (c.pending) release(c, c.pending_msg.generation, c.pending_msg.slot);  // superseded
            c.pending = true;
            c.pending_msg = *m;
            c.info.frames++;
            c.last_frame_ns = now_ns();
            return true;
        }
        case MsgType::Control: {
            if (n < sizeof(ControlMsg)) return false;
            auto* m = reinterpret_cast<const ControlMsg*>(buf);
            c.control = true;
            std::string cmd(m->text, strnlen(m->text, sizeof(m->text)));
            auto reply = make_msg<ControlReplyMsg>(MsgType::ControlReply);
            std::string text = control_handler ? control_handler(cmd) : "no control handler";
            snprintf(reply.text, sizeof(reply.text), "%s", text.c_str());
            return ipc_send(c.fd, &reply, sizeof(reply));
        }
        default: return true;
    }
}

bool FrameServer::import_slots(Client& c, const SlotsMsg& m, std::vector<int>& fds) {
    const bool has_depth = DepthFormat(m.depth_format) != DepthFormat::None;
    const uint32_t per_slot = has_depth ? 2 : 1;
    if (m.slot_count == 0 || m.slot_count > kMaxSlots || fds.size() != m.slot_count * per_slot) return false;
    if (m.width == 0 || m.height == 0 || m.width > 16384 || m.height > 16384) return false;
    const VkDeviceSize depth_bytes =
        VkDeviceSize(m.depth_width) * m.depth_height * depth_bytes_per_texel(DepthFormat(m.depth_format));

    c.slots = m;
    c.info.transport = Transport(m.transport);
    c.info.width = m.width;
    c.info.height = m.height;
    c.info.depth = has_depth;
    c.imported.resize(m.slot_count);
    VkDevice d = gpu_->device;

    for (uint32_t i = 0; i < m.slot_count; ++i) {
        auto& im = c.imported[i];
        int color_fd = fds[i * per_slot];
        int depth_fd = has_depth ? fds[i * per_slot + 1] : -1;

        if (Transport(m.transport) == Transport::Cpu) {
            im.shm_size = size_t(m.color_alloc_size + m.depth_alloc_size);
            void* p = mmap(nullptr, im.shm_size, PROT_READ, MAP_SHARED, color_fd, 0);
            if (p == MAP_FAILED) return false;
            im.shm = p;
            continue;  // fds closed by the caller
        }

        if (!gpu_->external_memory_fd) return false;
        VkExternalMemoryImageCreateInfo ext_img{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext_img};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VkFormat(m.color_format);
        ici.extent = {m.width, m.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = kSlotImageUsage;
        if (vkCreateImage(d, &ici, nullptr, &im.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, im.image, &req);
        if (!(req.memoryTypeBits & (1u << m.color_memory_type)) || req.size > m.color_alloc_size) return false;

        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = im.image;
        VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        imp.fd = color_fd;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
        mai.allocationSize = m.color_alloc_size;
        mai.memoryTypeIndex = m.color_memory_type;
        if (vkAllocateMemory(d, &mai, nullptr, &im.mem) != VK_SUCCESS) return false;
        fds[i * per_slot] = -1;  // owned by the driver now
        if (vkBindImageMemory(d, im.image, im.mem, 0) != VK_SUCCESS) return false;

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = im.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(d, &vci, nullptr, &im.view) != VK_SUCCESS) return false;

        if (has_depth) {
            VkExternalMemoryBufferCreateInfo ext_buf{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            ext_buf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext_buf};
            bci.size = depth_bytes;
            bci.usage = kSlotDepthBufferUsage;
            if (vkCreateBuffer(d, &bci, nullptr, &im.depth) != VK_SUCCESS) return false;
            vkGetBufferMemoryRequirements(d, im.depth, &req);
            if (!(req.memoryTypeBits & (1u << m.depth_memory_type)) || req.size > m.depth_alloc_size) return false;
            VkMemoryDedicatedAllocateInfo bded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            bded.buffer = im.depth;
            VkImportMemoryFdInfoKHR bimp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &bded};
            bimp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            bimp.fd = depth_fd;
            VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &bimp};
            bmai.allocationSize = m.depth_alloc_size;
            bmai.memoryTypeIndex = m.depth_memory_type;
            if (vkAllocateMemory(d, &bmai, nullptr, &im.depth_mem) != VK_SUCCESS) return false;
            fds[i * per_slot + 1] = -1;
            if (vkBindBufferMemory(d, im.depth, im.depth_mem, 0) != VK_SUCCESS) return false;
        }
    }

    if (Transport(m.transport) == Transport::Cpu) {
        if (!c.local.create(*gpu_, m.width, m.height, VkFormat(m.color_format),
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
        if (has_depth && !c.local_depth.create(*gpu_, depth_bytes,
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false))
            return false;
        if (!c.staging.create(*gpu_, m.color_alloc_size + (has_depth ? depth_bytes : 0),
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
            return false;
    }

    c.have_slots = true;
    VRL_LOG("server", "%s: %ux%u format %d, depth %s, %u slots via %s", c.info.exe.c_str(), m.width, m.height,
            m.color_format, has_depth ? "yes" : "no", m.slot_count,
            Transport(m.transport) == Transport::Gpu ? "gpu" : "cpu");
    return true;
}

void FrameServer::free_imports(Client& c) {
    VkDevice d = gpu_->device;
    for (auto& im : c.imported) {
        if (im.view) vkDestroyImageView(d, im.view, nullptr);
        if (im.image) vkDestroyImage(d, im.image, nullptr);
        if (im.mem) vkFreeMemory(d, im.mem, nullptr);
        if (im.depth) vkDestroyBuffer(d, im.depth, nullptr);
        if (im.depth_mem) vkFreeMemory(d, im.depth_mem, nullptr);
        if (im.shm) munmap(im.shm, im.shm_size);
    }
    c.imported.clear();
    c.local.destroy(*gpu_);
    c.local_depth.destroy(*gpu_);
    c.staging.destroy(*gpu_);
    c.have_slots = false;
}

void FrameServer::drop_client(size_t index) {
    Client& c = *clients_[index];
    if (c.hello) VRL_LOG("server", "game disconnected: %s (pid %d)", c.info.exe.c_str(), c.info.pid);
    if (c.have_slots && gpu_->queue) vkQueueWaitIdle(gpu_->queue);
    for (size_t i = 0; i < in_flight_.size();)
        if (in_flight_[i].client_id == c.id) in_flight_.erase(in_flight_.begin() + i);
        else ++i;
    free_imports(c);
    close(c.fd);
    clients_.erase(clients_.begin() + index);
}

void FrameServer::release(Client& c, uint32_t generation, uint32_t slot) {
    auto r = make_msg<ReleaseMsg>(MsgType::Release);
    r.generation = generation;
    r.slot = slot;
    ipc_send(c.fd, &r, sizeof(r));
}

void FrameServer::update_active() {
    const uint64_t now = now_ns();
    Client* cur = find(active_id_);
    Client* best = nullptr;
    for (auto& c : clients_) {
        if (c->control || !c->have_slots) continue;
        if (!best || c->last_frame_ns > best->last_frame_ns) best = c.get();
    }
    int new_id = active_id_;
    if (!cur || !cur->have_slots) {
        new_id = best ? best->id : 0;
    } else if (best && best != cur && now - cur->last_frame_ns > 1500000000ull &&
               now - best->last_frame_ns < 500000000ull) {
        new_id = best->id;  // the active game stopped presenting, another one is live
    }
    if (new_id != active_id_) {
        active_id_ = new_id;
        Client* a = find(active_id_);
        if (a) VRL_LOG("server", "now showing %s (pid %d)", a->info.exe.c_str(), a->info.pid);
        if (on_active_changed) on_active_changed(a ? &a->info : nullptr);
    }
    // Frames from games that are not shown go straight back.
    for (auto& c : clients_) {
        if (c->id != active_id_ && c->pending) {
            release(*c, c->pending_msg.generation, c->pending_msg.slot);
            c->pending = false;
        }
    }
}

void FrameServer::cycle_active() {
    std::vector<Client*> games;
    for (auto& c : clients_)
        if (!c->control && c->have_slots) games.push_back(c.get());
    if (games.size() < 2) return;
    size_t i = 0;
    while (i < games.size() && games[i]->id != active_id_) ++i;
    Client* next = games[(i + 1) % games.size()];
    next->last_frame_ns = now_ns();
    active_id_ = next->id;
    if (on_active_changed) on_active_changed(&next->info);
}

bool FrameServer::begin_frame(VkCommandBuffer cmd, FrameView* out) {
    Client* c = find(active_id_);
    if (!c || !c->pending || !c->have_slots) return false;
    const FrameMsg msg = c->pending_msg;
    c->pending = false;
    const SlotsMsg& m = c->slots;
    auto& im = c->imported[msg.slot];
    const bool has_depth = msg.has_depth && DepthFormat(m.depth_format) != DepthFormat::None;
    const VkDeviceSize depth_bytes =
        VkDeviceSize(m.depth_width) * m.depth_height * depth_bytes_per_texel(DepthFormat(m.depth_format));

    *out = FrameView{};
    out->format = VkFormat(m.color_format);
    out->width = m.width;
    out->height = m.height;
    out->depth_format = DepthFormat(m.depth_format);
    out->depth_width = m.depth_width;
    out->depth_height = m.depth_height;
    out->depth_size = depth_bytes;
    out->has_depth = has_depth;
    out->frame_index = msg.frame_index;
    out->timestamp_ns = msg.timestamp_ns;

    if (Transport(m.transport) == Transport::Gpu) {
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        ib.dstQueueFamilyIndex = gpu_->family;
        ib.image = im.image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        bb.dstQueueFamilyIndex = gpu_->family;
        bb.buffer = im.depth;
        bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             has_depth ? 1 : 0, &bb, 1, &ib);
        out->image = im.image;
        out->view = im.view;
        out->depth = has_depth ? im.depth : VK_NULL_HANDLE;
        in_flight_.push_back({c->id, msg.generation, msg.slot});
        return true;
    }

    // Cpu transport: copy out of shared memory and hand the slot straight back.
    memcpy(c->staging.mapped, im.shm, size_t(m.color_alloc_size));
    if (has_depth)
        memcpy(static_cast<char*>(c->staging.mapped) + m.color_alloc_size,
               static_cast<const char*>(im.shm) + m.color_alloc_size, size_t(depth_bytes));
    release(*c, msg.generation, msg.slot);

    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = c->local.image;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m.width, m.height, 1};
    vkCmdCopyBufferToImage(cmd, c->staging.buffer, c->local.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (has_depth) {
        VkBufferCopy bc{m.color_alloc_size, 0, depth_bytes};
        vkCmdCopyBuffer(cmd, c->staging.buffer, c->local_depth.buffer, 1, &bc);
    }
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = c->local_depth.buffer;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         has_depth ? 1 : 0, &bb, 1, &ib);
    out->image = c->local.image;
    out->view = c->local.view;
    out->depth = has_depth ? c->local_depth.buffer : VK_NULL_HANDLE;
    return true;
}

void FrameServer::gpu_work_done() {
    for (const InFlight& f : in_flight_) {
        Client* c = find(f.client_id);
        if (c && c->have_slots && c->slots.generation == f.generation) release(*c, f.generation, f.slot);
    }
    in_flight_.clear();
}

}  // namespace vrl
