// GPU pass converting a captured frame (+ depth) into a stereo pair.
#pragma once

#include "frame_server.h"
#include "gpu.h"

namespace vrl {

struct StereoParams {
    bool stereo = true;
    bool reversed_depth = false;
    bool flip_y = false;
    bool show_depth = false;
    float separation = 0.015f;
    float convergence = 0.03f;
    float popout = 0.5f;
    float far_plane = 1000.0f;
    int search_steps = 24;
    // Exact depth from the detected projection: distance = depth_b / (depth - depth_a).
    bool exact_depth = false;
    float depth_a = 0, depth_b = 0;
    float convergence_distance = 3.0f;  // game units
};

class StereoRenderer {
public:
    bool init(Gpu* gpu);
    void destroy();

    // Records the pass. Output layout is GENERAL, ready for TRANSFER_SRC reads
    // (the pass ends with a barrier to TRANSFER_READ).
    bool record(VkCommandBuffer cmd, const FrameView& frame, const StereoParams& params);

    VkImage output() const { return out_.image; }
    uint32_t output_width() const { return out_.width; }
    uint32_t output_height() const { return out_.height; }
    // Per-eye size; when the last pass was stereo the output is 2x as wide.
    uint32_t eye_width() const { return eye_width_; }
    bool output_is_stereo() const { return out_stereo_; }

    // Reversed-Z vote from the last pass: >0 reversed, <0 standard, 0 unknown.
    // Call after the submission containing the pass has completed.
    int read_depth_vote();

private:
    bool ensure_output(uint32_t w, uint32_t h);

    Gpu* gpu_ = nullptr;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    GpuBuffer dummy_depth_;
    GpuBuffer stats_;
    GpuImage out_;
    bool out_initialized_ = false;
    bool out_stereo_ = false;
    uint32_t eye_width_ = 0;
};

// True if the format holds gamma-encoded data that the sampler will not decode.
bool format_needs_srgb_decode(VkFormat f);

}  // namespace vrl
