#include "stereo.h"

#include <cstring>

#include "stereo.comp.h"

namespace vrl {

namespace {
struct PushConstants {
    uint32_t size[2];
    uint32_t depth_size[2];
    uint32_t depth_format;
    uint32_t flags;
    float separation;
    float convergence;
    float popout;
    float far_plane;
    int32_t steps;
    float depth_a;
    float depth_b;
    float conv_dist;
};
constexpr uint32_t kFlagStereo = 1;
constexpr uint32_t kFlagReversed = 2;
constexpr uint32_t kFlagFlipY = 4;
constexpr uint32_t kFlagSrgbDecode = 8;
constexpr uint32_t kFlagShowDepth = 16;
constexpr uint32_t kFlagHasDepth = 32;
constexpr uint32_t kFlagExactDepth = 64;
struct Stats {
    uint32_t zero_count, one_count, samples;
};
}  // namespace

bool format_needs_srgb_decode(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return true;
        default: return false;  // *_SRGB is decoded by the sampler, float formats are linear
    }
}

bool StereoRenderer::init(Gpu* gpu) {
    gpu_ = gpu;
    VkDevice d = gpu->device;

    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 4;
    dci.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &dci, nullptr, &dsl_) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &plci, nullptr, &layout_) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kStereoCompSpv);
    smci.pCode = kStereoCompSpv;
    VkShaderModule module;
    if (vkCreateShaderModule(d, &smci, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = layout_;
    VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_);
    vkDestroyShaderModule(d, module, nullptr);
    if (r != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(d, &dpci, nullptr, &pool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl_;
    if (vkAllocateDescriptorSets(d, &dsai, &set_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

    if (!dummy_depth_.create(*gpu, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    memset(dummy_depth_.mapped, 0, 16);
    if (!stats_.create(*gpu, sizeof(Stats), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    memset(stats_.mapped, 0, sizeof(Stats));
    return true;
}

void StereoRenderer::destroy() {
    if (!gpu_) return;
    VkDevice d = gpu_->device;
    out_.destroy(*gpu_);
    dummy_depth_.destroy(*gpu_);
    stats_.destroy(*gpu_);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(d, dsl_, nullptr);
    gpu_ = nullptr;
}

bool StereoRenderer::ensure_output(uint32_t w, uint32_t h) {
    if (out_.image && out_.width == w && out_.height == h) return true;
    if (out_.image) {
        vkQueueWaitIdle(gpu_->queue);
        out_.destroy(*gpu_);
    }
    out_initialized_ = false;
    return out_.create(*gpu_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
}

bool StereoRenderer::record(VkCommandBuffer cmd, const FrameView& f, const StereoParams& p) {
    const bool stereo = p.stereo && f.has_depth;
    if (!ensure_output(stereo ? f.width * 2 : f.width, f.height)) return false;
    out_stereo_ = stereo;
    eye_width_ = f.width;

    VkDescriptorImageInfo src{sampler_, f.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo depth{f.has_depth ? f.depth : dummy_depth_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo dst{VK_NULL_HANDLE, out_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo stats{stats_.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[4] = {};
    for (int i = 0; i < 4; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set_;
        w[i].dstBinding = uint32_t(i);
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &src;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &depth;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo = &dst;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[3].pBufferInfo = &stats;
    vkUpdateDescriptorSets(gpu_->device, 4, w, 0, nullptr);

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = out_initialized_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = out_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    out_initialized_ = true;

    PushConstants pc{};
    pc.size[0] = f.width;
    pc.size[1] = f.height;
    pc.depth_size[0] = f.has_depth ? f.depth_width : 1;
    pc.depth_size[1] = f.has_depth ? f.depth_height : 1;
    pc.depth_format = uint32_t(f.depth_format);
    pc.flags = (stereo ? kFlagStereo : 0u) | (p.reversed_depth ? kFlagReversed : 0u) | (p.flip_y ? kFlagFlipY : 0u) |
               (format_needs_srgb_decode(f.format) ? kFlagSrgbDecode : 0u) | (p.show_depth ? kFlagShowDepth : 0u) |
               (f.has_depth ? kFlagHasDepth : 0u) | (p.exact_depth ? kFlagExactDepth : 0u);
    pc.depth_a = p.depth_a;
    pc.depth_b = p.depth_b;
    pc.conv_dist = p.convergence_distance > 0 ? p.convergence_distance : 1.0f;
    pc.separation = p.separation;
    pc.convergence = p.convergence;
    pc.popout = p.popout;
    pc.far_plane = p.far_plane < 1.0f ? 1.0f : p.far_plane;
    pc.steps = p.search_steps < 4 ? 4 : (p.search_steps > 128 ? 128 : p.search_steps);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (f.width + 7) / 8, (f.height + 7) / 8, stereo ? 2 : 1);

    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, nullptr, 1, &b);
    return true;
}

int StereoRenderer::read_depth_vote() {
    auto* s = static_cast<Stats*>(stats_.mapped);
    int vote = 0;
    // Need a meaningful amount of "cleared" depth (sky / background) to vote.
    uint32_t threshold = s->samples / 50 + 1;
    if (s->zero_count > s->one_count && s->zero_count >= threshold) vote = 1;
    else if (s->one_count > s->zero_count && s->one_count >= threshold) vote = -1;
    memset(s, 0, sizeof(Stats));
    return vote;
}

}  // namespace vrl
