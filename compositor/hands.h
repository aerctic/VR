// Optical hand tracking (XR_EXT_hand_tracking): joint positions for drawing
// the hands, pinch gestures as buttons, and a pointing ray.
//
// Gestures (used when no controller is held in that hand):
//   index pinch  -> trigger (and click when pointing at the screen / panel)
//   middle pinch -> right hand: A, left hand: B
#pragma once

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

namespace vrl {

class HandTracking {
public:
    bool init(XrInstance instance, XrSession session);
    void destroy();
    bool available() const { return trackers_[0] != XR_NULL_HANDLE; }

    struct Hand {
        bool valid = false;
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
        bool index_pinch = false;
        bool middle_pinch = false;
        XrVector3f ray_origin{}, ray_dir{};
    };
    void update(XrSpace base, XrTime time);
    const Hand& hand(int i) const { return hands_[i]; }

private:
    PFN_xrCreateHandTrackerEXT create_ = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy_ = nullptr;
    PFN_xrLocateHandJointsEXT locate_ = nullptr;
    XrHandTrackerEXT trackers_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    Hand hands_[2];
};

}  // namespace vrl
