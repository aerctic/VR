// GPU renderer for the VR room (loading screen / theater surroundings) and
// for the hands / controllers / laser pointers overlay. Renders both eyes of
// a projection layer side by side.
#pragma once

#include <vector>

#include "gpu.h"

namespace vrl {

struct SceneEye {
    float orientation[4];  // xyzw
    float position[3];
    float tan_left, tan_right, tan_up, tan_down;
};

struct ScenePrim {
    enum Type { Sphere = 0, Capsule = 1, Glow = 2 };
    float a[3];
    float radius;
    float b[3];  // capsule end
    Type type;
    float color[4];  // linear rgb + opacity
};

class SceneRenderer {
public:
    bool init(Gpu* gpu, uint32_t eye_w, uint32_t eye_h);
    void destroy();

    // Records rendering of both eyes into an internal image (GENERAL layout,
    // readable as TRANSFER_SRC afterwards). overlay = false: environment.
    void record(VkCommandBuffer cmd, const SceneEye eyes[2], float floor_y, float time, bool overlay,
                const std::vector<ScenePrim>& prims);

    VkImage output(bool overlay) const { return out_[overlay ? 1 : 0].image; }
    uint32_t eye_width() const { return eye_w_; }
    uint32_t eye_height() const { return eye_h_; }

    static constexpr uint32_t kMaxPrims = 128;

private:
    Gpu* gpu_ = nullptr;
    uint32_t eye_w_ = 0, eye_h_ = 0;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2] = {};
    GpuImage out_[2];
    GpuBuffer buffers_[2];  // one per mode so both can be recorded in one frame
    bool initialized_[2] = {};
};

}  // namespace vrl
