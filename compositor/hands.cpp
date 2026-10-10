#include "hands.h"

#include <cmath>

#include "log.h"

namespace vrl {

static float dist(const XrVector3f& a, const XrVector3f& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool HandTracking::init(XrInstance instance, XrSession session) {
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&create_))) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&destroy_))) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&locate_))))
        return false;
    for (int h = 0; h < 2; ++h) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (XR_FAILED(create_(session, &ci, &trackers_[h]))) {
            destroy();
            return false;
        }
    }
    VRL_LOG("hands", "hand tracking enabled");
    return true;
}

void HandTracking::destroy() {
    for (auto& t : trackers_)
        if (t && destroy_) destroy_(t);
    trackers_[0] = trackers_[1] = XR_NULL_HANDLE;
}

void HandTracking::update(XrSpace base, XrTime time) {
    for (int h = 0; h < 2; ++h) {
        Hand& hand = hands_[h];
        bool was_index = hand.index_pinch, was_middle = hand.middle_pinch;
        hand.valid = false;
        if (!trackers_[h]) continue;
        XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = base;
        li.time = time;
        XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locs.jointLocations = hand.joints;
        if (XR_FAILED(locate_(trackers_[h], &li, &locs)) || !locs.isActive) {
            hand.index_pinch = hand.middle_pinch = false;
            continue;
        }
        const auto& j = hand.joints;
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (!(j[XR_HAND_JOINT_WRIST_EXT].locationFlags & need) || !(j[XR_HAND_JOINT_INDEX_TIP_EXT].locationFlags & need))
            continue;
        hand.valid = true;
        const XrVector3f thumb = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
        // Hysteresis so pinches do not flicker.
        float di = dist(thumb, j[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position);
        float dm = dist(thumb, j[XR_HAND_JOINT_MIDDLE_TIP_EXT].pose.position);
        hand.index_pinch = was_index ? di < 0.035f : di < 0.02f;
        hand.middle_pinch = !hand.index_pinch && (was_middle ? dm < 0.035f : dm < 0.02f);
        // Pointing ray: from between the shoulder side of the palm towards the
        // index knuckle, which stays steady while pinching.
        const XrVector3f a = j[XR_HAND_JOINT_WRIST_EXT].pose.position;
        const XrVector3f b = j[XR_HAND_JOINT_INDEX_PROXIMAL_EXT].pose.position;
        XrVector3f d{b.x - a.x, b.y - a.y, b.z - a.z};
        float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (n < 1e-4f) {
            hand.valid = false;
            continue;
        }
        hand.ray_origin = b;
        hand.ray_dir = {d.x / n, d.y / n, d.z / n};
    }
}

}  // namespace vrl
