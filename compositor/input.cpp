#include "input.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "log.h"

namespace vrl {

static XrPath path(XrInstance inst, const std::string& s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s.c_str(), &p);
    return p;
}

static XrAction make_action(XrActionSet set, const char* name, const char* label, XrActionType type,
                            const XrPath* subpaths, uint32_t nsub) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    snprintf(ci.actionName, sizeof(ci.actionName), "%s", name);
    snprintf(ci.localizedActionName, sizeof(ci.localizedActionName), "%s", label);
    ci.actionType = type;
    ci.countSubactionPaths = nsub;
    ci.subactionPaths = subpaths;
    XrAction a = XR_NULL_HANDLE;
    xrCreateAction(set, &ci, &a);
    return a;
}

bool XrInput::init(XrInstance inst, XrSession session) {
    XrActionSetCreateInfo sci{XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(sci.actionSetName, sizeof(sci.actionSetName), "gamepad");
    snprintf(sci.localizedActionSetName, sizeof(sci.localizedActionSetName), "Gamepad emulation");
    if (XR_FAILED(xrCreateActionSet(inst, &sci, &set_))) return false;

    hands_[0] = path(inst, "/user/hand/left");
    hands_[1] = path(inst, "/user/hand/right");
    stick_ = make_action(set_, "stick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT, hands_, 2);
    stick_click_ = make_action(set_, "stick_click", "Stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, hands_, 2);
    trigger_ = make_action(set_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, hands_, 2);
    squeeze_ = make_action(set_, "squeeze", "Grip (bumper)", XR_ACTION_TYPE_FLOAT_INPUT, hands_, 2);
    menu_ = make_action(set_, "start", "Start / recenter", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    back_ = make_action(set_, "back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    a_ = make_action(set_, "a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    b_ = make_action(set_, "b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    x_ = make_action(set_, "x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    y_ = make_action(set_, "y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    aim_ = make_action(set_, "aim", "Pointer", XR_ACTION_TYPE_POSE_INPUT, hands_, 2);

    using Binding = std::pair<XrAction, std::string>;
    auto suggest = [&](const char* profile, const std::vector<Binding>& bindings) {
        std::vector<XrActionSuggestedBinding> b;
        for (auto& [action, p] : bindings) b.push_back({action, path(inst, p)});
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path(inst, profile);
        sb.countSuggestedBindings = uint32_t(b.size());
        sb.suggestedBindings = b.data();
        if (XR_FAILED(xrSuggestInteractionProfileBindings(inst, &sb)))
            VRL_DBG("input", "runtime rejected bindings for %s", profile);
    };
    const char* L = "/user/hand/left/input/";
    const char* R = "/user/hand/right/input/";
    auto P = [](const char* hand, const char* s) { return std::string(hand) + s; };

    suggest("/interaction_profiles/oculus/touch_controller",
            {{aim_, P(L, "aim/pose")}, {aim_, P(R, "aim/pose")},
             {stick_, P(L, "thumbstick")}, {stick_, P(R, "thumbstick")},
             {stick_click_, P(L, "thumbstick/click")}, {stick_click_, P(R, "thumbstick/click")},
             {trigger_, P(L, "trigger/value")}, {trigger_, P(R, "trigger/value")},
             {squeeze_, P(L, "squeeze/value")}, {squeeze_, P(R, "squeeze/value")},
             {menu_, P(L, "menu/click")},
             {a_, P(R, "a/click")}, {b_, P(R, "b/click")}, {x_, P(L, "x/click")}, {y_, P(L, "y/click")}});
    suggest("/interaction_profiles/valve/index_controller",
            {{aim_, P(L, "aim/pose")}, {aim_, P(R, "aim/pose")},
             {stick_, P(L, "thumbstick")}, {stick_, P(R, "thumbstick")},
             {stick_click_, P(L, "thumbstick/click")}, {stick_click_, P(R, "thumbstick/click")},
             {trigger_, P(L, "trigger/value")}, {trigger_, P(R, "trigger/value")},
             {squeeze_, P(L, "squeeze/value")}, {squeeze_, P(R, "squeeze/value")},
             {menu_, P(L, "trackpad/force")}, {back_, P(R, "trackpad/force")},
             {a_, P(R, "a/click")}, {b_, P(R, "b/click")}, {x_, P(L, "a/click")}, {y_, P(L, "b/click")}});
    suggest("/interaction_profiles/htc/vive_controller",
            {{aim_, P(L, "aim/pose")}, {aim_, P(R, "aim/pose")},
             {stick_, P(L, "trackpad")}, {stick_, P(R, "trackpad")},
             {stick_click_, P(L, "trackpad/click")}, {stick_click_, P(R, "trackpad/click")},
             {trigger_, P(L, "trigger/value")}, {trigger_, P(R, "trigger/value")},
             {squeeze_, P(L, "squeeze/click")}, {squeeze_, P(R, "squeeze/click")},
             {menu_, P(L, "menu/click")}, {back_, P(R, "menu/click")}});
    suggest("/interaction_profiles/microsoft/motion_controller",
            {{aim_, P(L, "aim/pose")}, {aim_, P(R, "aim/pose")},
             {stick_, P(L, "thumbstick")}, {stick_, P(R, "thumbstick")},
             {stick_click_, P(L, "thumbstick/click")}, {stick_click_, P(R, "thumbstick/click")},
             {trigger_, P(L, "trigger/value")}, {trigger_, P(R, "trigger/value")},
             {squeeze_, P(L, "squeeze/click")}, {squeeze_, P(R, "squeeze/click")},
             {menu_, P(L, "menu/click")}, {back_, P(R, "menu/click")},
             {a_, P(R, "trackpad/click")}, {x_, P(L, "trackpad/click")}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{aim_, P(L, "aim/pose")}, {aim_, P(R, "aim/pose")},
             {a_, P(R, "select/click")}, {b_, P(L, "select/click")}, {menu_, P(L, "menu/click")},
             {back_, P(R, "menu/click")}});

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    attached_ = XR_SUCCEEDED(xrAttachSessionActionSets(session, &ai));
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci.action = aim_;
        asci.subactionPath = hands_[h];
        asci.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(session, &asci, &aim_space_[h]))) aim_space_[h] = XR_NULL_HANDLE;
    }
    return attached_;
}

void XrInput::destroy() {
    for (auto& sp : aim_space_)
        if (sp) xrDestroySpace(sp);
    aim_space_[0] = aim_space_[1] = XR_NULL_HANDLE;
    if (set_) xrDestroyActionSet(set_);
    set_ = XR_NULL_HANDLE;
    attached_ = false;
}

static bool get_bool(XrSession s, XrAction a, XrPath sub = XR_NULL_PATH) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}

static float get_float(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return (XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive) ? st.currentState : 0.0f;
}

static XrVector2f get_vec2(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive) return st.currentState;
    return {0, 0};
}

XrInput::Result XrInput::sync(XrSession session, float dt, XrSpace base, XrTime time) {
    Result r;
    if (!attached_) return r;
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &si))) return r;

    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = aim_;
        gi.subactionPath = hands_[h];
        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        r.active[h] = XR_SUCCEEDED(xrGetActionStatePose(session, &gi, &ps)) && ps.isActive;
        if (!r.active[h] || !aim_space_[h]) continue;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(aim_space_[h], base, time, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            r.aim_valid[h] = true;
            r.aim[h] = loc.pose;
        }
    }

    GamepadState& p = r.pad;
    XrVector2f ls = get_vec2(session, stick_, hands_[0]);
    XrVector2f rs = get_vec2(session, stick_, hands_[1]);
    p.lx = ls.x;
    p.ly = ls.y;
    p.rx = rs.x;
    p.ry = rs.y;
    p.lstick = get_bool(session, stick_click_, hands_[0]);
    p.rstick = get_bool(session, stick_click_, hands_[1]);
    p.lt = get_float(session, trigger_, hands_[0]);
    p.rt = get_float(session, trigger_, hands_[1]);
    float lgrip = get_float(session, squeeze_, hands_[0]);
    p.lb = lgrip > 0.5f;
    p.rb = get_float(session, squeeze_, hands_[1]) > 0.5f;
    p.a = get_bool(session, a_);
    p.b = get_bool(session, b_);
    p.x = get_bool(session, x_);
    p.y = get_bool(session, y_);
    p.back = get_bool(session, back_);

    // Start: short tap = Start, hold = recenter, with left grip = Back.
    bool start_down = get_bool(session, menu_);
    if (start_down) {
        if (!start_was_down_) {
            start_held_ = 0;
            start_consumed_ = false;
            loading_fired_ = false;
        }
        start_held_ += dt;
        if (lgrip > 0.5f) {
            p.back = true;
            p.lb = false;  // the grip is part of the chord, not a bumper press
            start_consumed_ = true;
        } else if (start_held_ >= 1.0f && !start_consumed_) {
            r.recenter = true;
            start_consumed_ = true;
        } else if (start_held_ >= 3.0f && !loading_fired_) {
            r.open_loading = true;
            loading_fired_ = true;
        }
    } else if (start_was_down_ && !start_consumed_) {
        start_pulse_ = 0.12f;
    }
    start_was_down_ = start_down;
    if (start_pulse_ > 0) {
        p.start = true;
        start_pulse_ -= dt;
    }

    if (p.lstick && p.rstick) {
        sticks_held_ += dt;
        if (sticks_held_ >= 1.5f && !sticks_consumed_) {
            r.toggle_mode = true;
            sticks_consumed_ = true;
        }
    } else {
        sticks_held_ = 0;
        sticks_consumed_ = false;
    }
    return r;
}

}  // namespace vrl
