// Streams presented frames (and the selected depth buffer) to the compositor.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "layer.h"

namespace vrl {

class Capture {
public:
    explicit Capture(DeviceData* dev);
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Called from vkQueuePresentKHR. Returns true when a copy was submitted;
    // the present must then wait on *wait instead of its own semaphores.
    bool on_present(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* wait);

    // Blocks until no copy is in flight.
    void wait_idle();

private:
    enum class SlotState { Free, Copying, Sent };
    enum class Conn { Disconnected, AwaitWelcome, Ready };

    struct Slot {
        SlotState state = SlotState::Free;
        // Gpu transport: exported image + depth buffer.
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_mem = VK_NULL_HANDLE;
        VkBuffer depth = VK_NULL_HANDLE;
        VkDeviceMemory depth_mem = VK_NULL_HANDLE;
        // Cpu transport: host readback buffer + shared memfd.
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readback_mem = VK_NULL_HANDLE;
        void* readback_ptr = nullptr;
        bool readback_coherent = true;
        int shm_fd = -1;
        void* shm = nullptr;
        // Submission.
        VkCommandPool pool = VK_NULL_HANDLE;
        uint32_t pool_family = UINT32_MAX;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t frame_index = 0;
        bool has_depth = false;
    };

    struct Job {
        uint32_t slot;
        uint32_t generation;
    };

    void poll_connection();
    void disconnect();
    bool create_slots(VkFormat format, VkExtent2D extent, const DepthStaging& depth);
    bool create_gpu_slot(Slot& s, VkFormat format, VkExtent2D extent, VkDeviceSize depth_size);
    bool create_cpu_slot(Slot& s, VkDeviceSize color_size, VkDeviceSize depth_size);
    void destroy_slots();
    bool send_slots();
    bool record_and_submit(Slot& s, VkQueue queue, uint32_t family, VkImage sc_image, const DepthStaging* depth,
                           const VkPresentInfoKHR* info, VkSemaphore signal);
    void worker_main();

    DeviceData* dev_;

    // Connection (present thread only, fd_ also used by the worker under io_mtx_).
    std::mutex io_mtx_;
    int fd_ = -1;
    Conn conn_ = Conn::Disconnected;
    Transport transport_ = Transport::Gpu;
    uint64_t last_attempt_ns_ = 0;
    std::atomic<bool> broken_{false};
    bool warned_format_ = false;

    // Current slot configuration.
    uint32_t generation_ = 0;
    Transport slot_transport_ = Transport::Gpu;
    uint32_t slot_count_ = 0;
    uint64_t sc_serial_ = 0;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    DepthFormat depth_format_ = DepthFormat::None;
    VkExtent2D depth_extent_{};
    VkDeviceSize color_size_ = 0;  // Cpu: packed bytes; Gpu: allocation size
    VkDeviceSize depth_size_ = 0;  // packed bytes
    VkDeviceSize depth_alloc_size_ = 0;
    uint32_t color_mem_type_ = 0;
    uint32_t depth_mem_type_ = 0;
    uint64_t frame_counter_ = 0;
    Slot slots_[kMaxSlots];
    std::mutex slot_mtx_;  // guards Slot::state

    // Worker thread waiting for copies to finish.
    std::thread worker_;
    std::mutex job_mtx_;
    std::condition_variable job_cv_;
    std::condition_variable idle_cv_;
    std::deque<Job> jobs_;
    bool worker_busy_ = false;
    bool stop_ = false;
};

// Bytes per pixel of a swapchain format we can stream over the Cpu transport
// (0 if unsupported).
uint32_t color_bytes_per_pixel(VkFormat f);

}  // namespace vrl
