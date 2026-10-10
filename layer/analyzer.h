// Finds the game's camera in GPU constant data, tracks it, and (optionally)
// re-aims it with the VR head pose.
//
// Search: a background thread scans every host-visible mapping of the game
// (16-byte stride, the alignment of float4x4 in HLSL cbuffers and std140)
// for perspective projection matrices whose aspect matches the swapchain.
// The most common one is the main camera. Around its copies we look for a
// rigid transform at a consistent offset (the view matrix), and for P*V and
// V^-1 (view-projection / camera matrices) so injection can keep them
// consistent. Push constants and vkCmdUpdateBuffer data are checked inline.
//
// Tracking: the projection never changes from frame to frame, so its exact
// bit pattern is an anchor: wherever it is written, the view matrix sits at
// the learned offset.
//
// Injection: at vkQueueSubmit, every fresh copy of the view matrix in the
// mapped "camera" regions is replaced by D^-1 * V (D = head pose in camera
// space) before the GPU reads it.
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "camera_math.h"
#include "protocol.h"

namespace vrl {

class Analyzer {
public:
    Analyzer();
    ~Analyzer();
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;

    // Memory tracking (called from the corresponding Vulkan hooks).
    void on_allocate(VkDeviceMemory mem, VkDeviceSize size);
    void on_free(VkDeviceMemory mem);
    void on_map(VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize size, void* ptr);
    void on_unmap(VkDeviceMemory mem);
    // Must be held around the real vkUnmapMemory / vkFreeMemory so the
    // scanner never touches memory that is going away.
    std::mutex& region_lock() { return regions_mtx_; }

    // Inline constant data (push constants, vkCmdUpdateBuffer). Returns true
    // if the data contained the camera and *patched holds a re-aimed copy.
    bool on_inline_data(const void* data, uint32_t size, std::vector<uint8_t>* patched);
    void on_viewport(const VkViewport* v, uint32_t n);

    // Rewrites fresh view matrices in mapped memory. Call before every submit.
    void before_submit();
    // Per frame. Returns true if the analysis changed and should be reported.
    bool on_present(VkExtent2D extent, bool depth_selected);

    void restart();
    void set_camera(const CameraMsg& m);

    AnalysisMsg report();
    // The game's own camera this frame (before injection).
    bool camera(float rot[9], float pos[3]);
    // Injection applied to the frame being presented.
    void frame_injection(uint32_t* seq, uint32_t* eye);

private:
    struct Region {
        uint64_t id = 0;
        VkDeviceSize alloc_size = 0;
        uint8_t* ptr = nullptr;  // null when unmapped
        VkDeviceSize size = 0;
    };
    struct Hit {
        uint64_t region;
        VkDeviceSize offset;
    };
    using Bits = std::array<uint32_t, 16>;

    void thread_main();
    void search_step();
    void finish_search_pass();
    void track();
    void scan_for_anchor(const Region& r, VkDeviceSize begin, VkDeviceSize end, std::vector<Hit>* hits) const;
    Mat4 current_delta() const;  // requires state_mtx_
    void patch_at(uint8_t* base, VkDeviceSize size, VkDeviceSize hit, uint64_t key, const Mat4& d);

    // Regions (guarded by regions_mtx_).
    std::mutex regions_mtx_;
    std::unordered_map<VkDeviceMemory, Region> regions_;
    uint64_t next_region_id_ = 1;

    // Analysis state (guarded by state_mtx_).
    mutable std::mutex state_mtx_;
    enum class State { Searching, Tracking } state_ = State::Searching;
    VkExtent2D extent_{};
    std::atomic<bool> viewport_flipped_{false};
    bool depth_selected_ = false;
    // Search pass progress.
    std::vector<uint64_t> pass_regions_;
    size_t pass_index_ = 0;
    VkDeviceSize pass_offset_ = 0;
    uint64_t pass_bytes_ = 0, pass_total_ = 0;  // scanner thread only
    uint64_t scanned_total_ = 0;
    uint32_t progress_ = 0;
    bool restart_requested_ = false;
    std::map<Bits, std::vector<Hit>> proj_candidates_;
    std::map<Bits, int> inline_candidates_;
    bool inline_have_view_ = false;
    int64_t inline_view_rel_ = 0;
    // Result.
    bool have_proj_ = false;
    Bits proj_bits_{};
    ProjectionInfo proj_{};
    bool proj_inline_ = false;
    int64_t view_rel_ = 0, vp_rel_ = 0, inv_rel_ = 0;  // byte offsets from the projection
    bool have_view_ = false, have_vp_ = false, have_inv_ = false;
    std::vector<uint64_t> hot_regions_;
    std::unordered_map<uint64_t, VkDeviceSize> cursor_;  // per hot region: last anchor seen
    uint32_t frames_without_hits_ = 0;
    uint32_t last_hits_ = 0;
    // Camera.
    bool camera_valid_ = false;
    float cam_rot_[9]{}, cam_pos_[3]{};
    std::unordered_map<uint64_t, Bits> last_view_seen_;
    // Injection.
    CameraMsg camera_msg_{};
    uint32_t eye_ = 0;
    uint32_t frame_seq_ = 0, frame_eye_ = 0;
    uint32_t presented_seq_ = 0, presented_eye_ = 0;
    uint32_t patched_this_frame_ = 0, patched_last_frame_ = 0;
    std::unordered_map<uint64_t, std::pair<Bits, Bits>> patched_;  // location -> (original, written)
    VkDeviceSize inject_window_ = 2 << 20;
    bool report_dirty_ = true;
    char status_[96] = "waiting for 3D rendering";

    std::thread thread_;
    std::condition_variable cv_;
    bool kick_ = false;
    bool stop_ = false;
};

}  // namespace vrl
