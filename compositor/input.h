// VR controllers -> virtual Xbox pad, plus a few compositor shortcuts.
//
// Mapping (Touch / Index / WMR / Vive):
//   sticks, triggers         -> sticks, triggers
//   grips                    -> LB / RB
//   A B (right), X Y (left)  -> A B X Y   (Index: left A/B act as X/Y)
//   stick clicks             -> L3 / R3
//   left menu                -> Start      (Index: left trackpad press)
//   left menu + left grip    -> Back       (Index: right trackpad press, Vive: right menu)
//   hold Start 1 s           -> recenter the screen
//   hold both stick clicks 1.5 s -> toggle theater / immersive
#pragma once

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

#include "uinput.h"

namespace vrl {

class XrInput {
public:
    bool init(XrInstance instance, XrSession session);
    void destroy();

    struct Result {
        GamepadState pad;
        bool recenter = false;
        bool toggle_mode = false;
    };
    // dt in seconds since the previous call.
    Result sync(XrSession session, float dt);

private:
    XrActionSet set_ = XR_NULL_HANDLE;
    XrPath hands_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrAction stick_ = XR_NULL_HANDLE, stick_click_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE,
             squeeze_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE, back_ = XR_NULL_HANDLE, a_ = XR_NULL_HANDLE,
             b_ = XR_NULL_HANDLE, x_ = XR_NULL_HANDLE, y_ = XR_NULL_HANDLE;
    bool attached_ = false;

    float start_held_ = 0;
    bool start_was_down_ = false;
    bool start_consumed_ = false;
    float start_pulse_ = 0;  // keep Start pressed for a moment after a short tap
    float sticks_held_ = 0;
    bool sticks_consumed_ = false;
};

}  // namespace vrl
