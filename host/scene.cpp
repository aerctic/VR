#include "scene.h"

#include <cstring>

#include "scene.comp.h"

namespace vrl {

namespace {
struct GpuEye {
    float orientation[4];
    float position[4];
    float tan_lrud[4];
};
struct GpuPrim {
    float a[4];
    float b[4];
    float color[4];
};
struct GpuSceneHeader {
    GpuEye eyes[2];
    float params[4];
    float accent[4];
};
}  // namespace

bool SceneRenderer::init(Gpu* gpu, uint32_t eye_w, uint32_t eye_h) {
    gpu_ = gpu;
    eye_w_ = eye_w;
    eye_h_ = eye_h;
    VkDevice d = gpu->device;

    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 2;
    dci.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &dci, nullptr, &dsl_) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &plci, nullptr, &layout_) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kSceneCompSpv);
    smci.pCode = kSceneCompSpv;
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

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(d, &dpci, nullptr, &pool_) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {dsl_, dsl_};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(d, &dsai, sets_) != VK_SUCCESS) return false;

    const VkDeviceSize bytes = sizeof(GpuSceneHeader) + sizeof(GpuPrim) * kMaxPrims;
    for (int i = 0; i < 2; ++i) {
        if (!out_[i].create(*gpu, eye_w * 2, eye_h, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            return false;
        if (!buffers_[i].create(*gpu, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
        VkDescriptorImageInfo img{VK_NULL_HANDLE, out_[i].view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo buf{buffers_[i].buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2] = {};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &img, nullptr, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, &buf, nullptr};
        vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    }
    return true;
}

void SceneRenderer::destroy() {
    if (!gpu_) return;
    VkDevice d = gpu_->device;
    for (int i = 0; i < 2; ++i) {
        out_[i].destroy(*gpu_);
        buffers_[i].destroy(*gpu_);
    }
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(d, dsl_, nullptr);
    gpu_ = nullptr;
}

void SceneRenderer::record(VkCommandBuffer cmd, const SceneEye eyes[2], float floor_y, float time, bool overlay,
                           const std::vector<ScenePrim>& prims) {
    const int i = overlay ? 1 : 0;
    auto* hdr = static_cast<GpuSceneHeader*>(buffers_[i].mapped);
    for (int e = 0; e < 2; ++e) {
        memcpy(hdr->eyes[e].orientation, eyes[e].orientation, 16);
        memcpy(hdr->eyes[e].position, eyes[e].position, 12);
        hdr->eyes[e].position[3] = 1;
        hdr->eyes[e].tan_lrud[0] = eyes[e].tan_left;
        hdr->eyes[e].tan_lrud[1] = eyes[e].tan_right;
        hdr->eyes[e].tan_lrud[2] = eyes[e].tan_up;
        hdr->eyes[e].tan_lrud[3] = eyes[e].tan_down;
    }
    const size_t n = prims.size() < kMaxPrims ? prims.size() : kMaxPrims;
    hdr->params[0] = floor_y;
    hdr->params[1] = time;
    hdr->params[2] = overlay ? 1.0f : 0.0f;
    hdr->params[3] = float(n);
    hdr->accent[0] = 0.25f;
    hdr->accent[1] = 0.65f;
    hdr->accent[2] = 1.0f;
    hdr->accent[3] = 1.0f;
    auto* gp = reinterpret_cast<GpuPrim*>(hdr + 1);
    for (size_t k = 0; k < n; ++k) {
        const ScenePrim& p = prims[k];
        gp[k] = GpuPrim{{p.a[0], p.a[1], p.a[2], p.radius},
                        {p.b[0], p.b[1], p.b[2], float(p.type)},
                        {p.color[0], p.color[1], p.color[2], p.color[3]}};
    }

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = initialized_[i] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = out_[i].image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    initialized_[i] = true;

    const uint32_t size[2] = {eye_w_, eye_h_};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[i], 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, size);
    vkCmdDispatch(cmd, (eye_w_ + 7) / 8, (eye_h_ + 7) / 8, 2);

    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

}  // namespace vrl
