// GPU work for one compositor frame and the OpenXR composition layers.

#include <cmath>
#include <cstring>

#include "app.h"

namespace vrl {

static Quat to_quat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
static XrQuaternionf to_xr(const Quat& q) { return {q.x, q.y, q.z, q.w}; }
static Vec3 to_vec(const XrVector3f& v) { return {v.x, v.y, v.z}; }
static XrVector3f to_xr(const Vec3& v) { return {v.x, v.y, v.z}; }

static const std::vector<int64_t> kColorFormats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT};

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

App::Quad App::panel_quad() const {
    // Below eye level, tilted up towards the viewer like a dashboard.
    Quad q;
    q.rot = normalize(mul(rest_rot_, from_axis_angle({1, 0, 0}, rad(-25))));
    q.pos = add(rest_pos_, rotate(rest_rot_, {0, -0.32f, -0.85f}));
    q.w = 0.9f;
    q.h = q.w * float(kPanelHeight) / float(kPanelWidth);
    return q;
}

App::Quad App::screen_quad() const {
    const float aspect = frame_h_ ? float(frame_w_) / float(frame_h_) : 16.0f / 9.0f;
    Quad q;
    q.rot = rest_rot_;
    if (phase_ == Phase::Loading) {
        // Preview above the panel so game menus stay usable while loading.
        q.w = 3.2f;
        q.pos = add(rest_pos_, rotate(rest_rot_, {0, 0.55f, -3.2f}));
    } else {
        const float dist = std::fmax(settings_.screen_distance, 0.3f);
        q.w = std::fmax(settings_.screen_width, 0.1f);
        q.pos = add(add(rest_pos_, {0, settings_.screen_height, 0}), rotate(rest_rot_, {0, 0, -dist}));
    }
    q.h = q.w / aspect;
    return q;
}

bool App::screen_is_cylinder() const {
    return phase_ == Phase::Playing && settings_.curved && xr_.cylinder_supported();
}

StereoParams App::stereo_params(bool force_mono) const {
    StereoParams p;
    p.stereo = settings_.stereo && !force_mono;
    p.reversed_depth = settings_.depth_reversed == "auto" ? reversed_score_ > 0 : settings_.depth_reversed == "true";
    p.flip_y = settings_.depth_flip_y;
    p.show_depth = settings_.show_depth;
    p.separation = settings_.separation;
    p.convergence = settings_.convergence;
    p.popout = settings_.popout;
    p.far_plane = settings_.depth_far_plane;
    p.search_steps = settings_.search_steps;
    const AnalysisMsg* a = analysis();
    if (a && (a->flags & kAnalysisProjection) && settings_.depth_reversed == "auto") {
        // The projection tells us exactly how depth maps to distance.
        p.exact_depth = true;
        p.depth_a = a->depth_a;
        p.depth_b = a->depth_b;
        p.convergence_distance = settings_.convergence_near * a->near_d;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Overlay primitives: hands, controllers, pointer
// ---------------------------------------------------------------------------

void App::collect_prims() {
    prims_.clear();
    auto sphere = [&](Vec3 c, float r, float cr, float cg, float cb, float a) {
        ScenePrim p{};
        p.a[0] = c.x;
        p.a[1] = c.y;
        p.a[2] = c.z;
        p.radius = r;
        p.type = ScenePrim::Sphere;
        p.color[0] = cr;
        p.color[1] = cg;
        p.color[2] = cb;
        p.color[3] = a;
        prims_.push_back(p);
    };
    auto capsule = [&](Vec3 a, Vec3 b, float r, ScenePrim::Type type, float cr, float cg, float cb, float alpha) {
        ScenePrim p{};
        p.a[0] = a.x;
        p.a[1] = a.y;
        p.a[2] = a.z;
        p.radius = r;
        p.b[0] = b.x;
        p.b[1] = b.y;
        p.b[2] = b.z;
        p.type = type;
        p.color[0] = cr;
        p.color[1] = cg;
        p.color[2] = cb;
        p.color[3] = alpha;
        prims_.push_back(p);
    };

    // Hands: a sphere per joint (palm skipped), pinching fingertips light up.
    for (int h = 0; h < 2 && settings_.hand_tracking; ++h) {
        const HandTracking::Hand& hand = hands_.hand(h);
        if (!hand.valid) continue;
        for (int j = 1; j < XR_HAND_JOINT_COUNT_EXT; ++j) {
            const XrHandJointLocationEXT& jl = hand.joints[j];
            if (!(jl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) continue;
            bool lit = (hand.index_pinch && (j == XR_HAND_JOINT_INDEX_TIP_EXT || j == XR_HAND_JOINT_THUMB_TIP_EXT)) ||
                       (hand.middle_pinch && (j == XR_HAND_JOINT_MIDDLE_TIP_EXT || j == XR_HAND_JOINT_THUMB_TIP_EXT));
            float r = std::fmin(std::fmax(jl.radius, 0.005f), 0.02f);
            if (lit) sphere(to_vec(jl.pose.position), r * 1.2f, 0.35f, 0.75f, 1.0f, 1.0f);
            else sphere(to_vec(jl.pose.position), r, 0.82f, 0.86f, 0.92f, 0.85f);
        }
    }
    // Controllers.
    for (int h = 0; h < 2; ++h) {
        if (!aim_valid_[h]) continue;
        Vec3 p = to_vec(aim_[h].position);
        Vec3 back = add(p, rotate(to_quat(aim_[h].orientation), {0, -0.02f, 0.1f}));
        capsule(p, back, 0.018f, ScenePrim::Capsule, 0.16f, 0.18f, 0.22f, 1.0f);
        sphere(p, 0.012f, 0.35f, 0.7f, 1.0f, 1.0f);
    }
    // Pointer.
    if (pointer_active_) {
        capsule(pointer_from_, pointer_to_, pointer_hit_ ? 0.003f : 0.0015f, ScenePrim::Glow, 0.3f, 0.7f, 1.0f,
                pointer_hit_ ? 0.9f : 0.35f);
        if (pointer_hit_) sphere(pointer_to_, pointer_down_ ? 0.016f : 0.012f, 0.6f, 0.85f, 1.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void App::blit(VkCommandBuffer cmd, VkImage src, int32_t sx, int32_t sy, uint32_t w, uint32_t h, VkImage dst,
               int32_t dx, int32_t dy, uint32_t dw, uint32_t dh) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {sx, sy, 0};
    region.srcOffsets[1] = {sx + int32_t(w), sy + int32_t(h), 1};
    region.dstSubresource = region.srcSubresource;
    region.dstOffsets[0] = {dx, dy, 0};
    region.dstOffsets[1] = {dx + int32_t(dw), dy + int32_t(dh), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   (w == dw && h == dh) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}

void App::render(double now) {
    Gpu& gpu = xr_.gpu();
    if (fence_pending_) {
        vkWaitForFences(gpu.device, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(gpu.device, 1, &fence_);
        fence_pending_ = false;
        server_->gpu_work_done();
        reversed_score_ += stereo_.read_depth_vote();
        reversed_score_ = reversed_score_ > 30 ? 30 : (reversed_score_ < -30 ? -30 : reversed_score_);
    }
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    const ViewMode mode = effective_mode();
    const bool vr = phase_ == Phase::Playing && mode == ViewMode::Vr;
    const bool aer = vr && settings_.vr_camera == "alternate";
    std::vector<XrImageSwapchain*> acquired;
    auto acquire = [&](XrImageSwapchain& sc, uint32_t w, uint32_t h) -> VkImage {
        if (!sc.ensure(xr_.session(), xr_.formats(), kColorFormats, w, h, gpu.queue)) return VK_NULL_HANDLE;
        VkImage img = sc.acquire();
        if (img) acquired.push_back(&sc);
        return img;
    };

    // --- game frame
    new_output_ = false;
    FrameView f;
    if (server_->has_new_frame() && server_->begin_frame(cmd_, &f)) {
        on_game_frame(now, f);
        if (stereo_.record(cmd_, f, stereo_params(aer))) {
            have_output_ = new_output_ = true;
            frame_w_ = f.width;
            frame_h_ = f.height;
            frame_stereo_ = stereo_.output_is_stereo();
            ++fps_window_frames_;
            if (aer && f.eye != 0) {
                if (aer_canvas_.width != 2 * f.width || aer_canvas_.height != f.height) {
                    vkQueueWaitIdle(gpu.queue);
                    aer_canvas_.destroy(gpu);
                    aer_canvas_.create(gpu, 2 * f.width, f.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                    image_barrier(cmd_, aer_canvas_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT);
                }
                VkImageCopy c{};
                c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.dstSubresource = c.srcSubresource;
                c.dstOffset = {int32_t((f.eye - 1) * f.width), 0, 0};
                c.extent = {f.width, f.height, 1};
                vkCmdCopyImage(cmd_, stereo_.output(), VK_IMAGE_LAYOUT_GENERAL, aer_canvas_.image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                image_barrier(cmd_, aer_canvas_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            }
        }
    }
    const uint64_t ns = uint64_t(now * 1e9);
    if (!fps_window_start_) fps_window_start_ = ns;
    if (ns - fps_window_start_ > 1000000000ull) {
        game_fps_ = float(fps_window_frames_) / float(double(ns - fps_window_start_) * 1e-9);
        fps_window_start_ = ns;
        fps_window_frames_ = 0;
    }

    if (new_output_) {
        if (vr) {
            const bool use_canvas = aer && aer_canvas_.image;
            const uint32_t w = use_canvas ? aer_canvas_.width : stereo_.output_width();
            if (VkImage dst = acquire(vr_sc_, 2 * frame_w_, frame_h_)) {
                blit(cmd_, use_canvas ? aer_canvas_.image : stereo_.output(), 0, 0, w, frame_h_, dst, 0, 0, w, frame_h_);
                vr_has_image_ = true;
            }
        } else if (VkImage dst = acquire(screen_sc_, stereo_.output_width(), stereo_.output_height())) {
            blit(cmd_, stereo_.output(), 0, 0, stereo_.output_width(), stereo_.output_height(), dst, 0, 0,
                 stereo_.output_width(), stereo_.output_height());
            screen_has_image_ = true;
        }
    }

    // --- room and overlay (both eyes, rendered for this frame's views)
    SceneEye eyes[2];
    for (int e = 0; e < 2 && views_ok_; ++e) {
        const XrView& v = views_[e];
        eyes[e] = SceneEye{{v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w},
                           {v.pose.position.x, v.pose.position.y, v.pose.position.z},
                           std::tan(v.fov.angleLeft),
                           std::tan(v.fov.angleRight),
                           std::tan(v.fov.angleUp),
                           std::tan(v.fov.angleDown)};
    }
    const uint32_t sw = scene_.eye_width(), sh = scene_.eye_height();
    env_this_frame_ = false;
    if (views_ok_ && (phase_ == Phase::Loading || settings_.environment)) {
        scene_.record(cmd_, eyes, floor_y_, float(now), false, {});
        if (VkImage dst = acquire(env_sc_, 2 * sw, sh)) {
            blit(cmd_, scene_.output(false), 0, 0, 2 * sw, sh, dst, 0, 0, 2 * sw, sh);
            env_this_frame_ = true;
        }
    }
    collect_prims();
    overlay_this_frame_ = false;
    if (views_ok_ && !prims_.empty()) {
        scene_.record(cmd_, eyes, floor_y_, float(now), true, prims_);
        if (VkImage dst = acquire(overlay_sc_, 2 * sw, sh)) {
            blit(cmd_, scene_.output(true), 0, 0, 2 * sw, sh, dst, 0, 0, 2 * sw, sh);
            overlay_this_frame_ = true;
        }
    }

    // --- loading panel (only when its content changed)
    if (phase_ == Phase::Loading && panel_dirty_) {
        draw_loading_panel(panel_canvas_, loading_view_, hovered_button_, pressed_button_);
        memcpy(panel_staging_.mapped, panel_canvas_.data(), panel_canvas_.size_bytes());
        image_barrier(cmd_, panel_image_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kPanelWidth, kPanelHeight, 1};
        vkCmdCopyBufferToImage(cmd_, panel_staging_.buffer, panel_image_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        image_barrier(cmd_, panel_image_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        if (VkImage dst = acquire(panel_sc_, kPanelWidth, kPanelHeight)) {
            blit(cmd_, panel_image_.image, 0, 0, kPanelWidth, kPanelHeight, dst, 0, 0, kPanelWidth, kPanelHeight);
            panel_has_image_ = true;
        }
        panel_dirty_ = false;
        panel_drawn_at_ = now;
    }

    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(gpu.queue, 1, &si, fence_);
    fence_pending_ = true;
    for (XrImageSwapchain* sc : acquired) sc->release();
}

// ---------------------------------------------------------------------------
// Layers: room, game, panel, overlay (back to front)
// ---------------------------------------------------------------------------

void App::build_layers() {
    layers_.clear();
    auto projection = [&](XrCompositionLayerProjection& layer, XrCompositionLayerProjectionView* views,
                          const XrImageSwapchain& sc, uint32_t eye_w, uint32_t eye_h, bool alpha) {
        for (int e = 0; e < 2; ++e) {
            views[e] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[e].pose = views_[e].pose;
            views[e].fov = views_[e].fov;
            views[e].subImage.swapchain = sc.handle();
            views[e].subImage.imageRect.offset = {int32_t(e * eye_w), 0};
            views[e].subImage.imageRect.extent = {int32_t(eye_w), int32_t(eye_h)};
        }
        layer = XrCompositionLayerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = xr_.local_space();
        layer.layerFlags = alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        layer.viewCount = 2;
        layer.views = views;
        layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
    };

    if (env_this_frame_) projection(env_layer_, env_views_, env_sc_, scene_.eye_width(), scene_.eye_height(), false);

    const ViewMode mode = effective_mode();
    const bool vr = phase_ == Phase::Playing && mode == ViewMode::Vr;
    const float aspect = frame_h_ ? float(frame_w_) / float(frame_h_) : 1.0f;
    const bool stereo = frame_stereo_;
    const XrEyeVisibility vis[2] = {stereo ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH, XR_EYE_VISIBILITY_RIGHT};
    const int eyes = stereo ? 2 : 1;
    auto sub_image = [&](const XrImageSwapchain& sc, int e) {
        XrSwapchainSubImage s{};
        s.swapchain = sc.handle();
        s.imageRect.offset = {int32_t(e * frame_w_), 0};
        s.imageRect.extent = {int32_t(frame_w_), int32_t(frame_h_)};
        return s;
    };

    if (have_output_ && vr && vr_has_image_ && (vr_eye_valid_[0] || vr_eye_valid_[1])) {
        // 6DOF: each eye shows the image rendered from the pose it was rendered with.
        const AnalysisMsg* a = analysis();
        const float th = std::tan(rad(a ? a->hfov_deg : settings_.game_fov) * 0.5f);
        const float tv = a && a->vfov_deg > 0 ? std::tan(rad(a->vfov_deg) * 0.5f) : th / aspect;
        const bool aer = settings_.vr_camera == "alternate";
        for (int e = 0; e < 2; ++e) {
            int src = (aer || stereo) ? e : 0;
            if (aer && !vr_eye_valid_[src]) src = 1 - src;  // first frames: one eye only so far
            XrCompositionLayerProjectionView& v = vr_views_[e];
            v = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            v.pose = vr_eye_pose_[vr_eye_valid_[e] ? e : 1 - e];
            v.fov = {-std::atan(th), std::atan(th), std::atan(tv), -std::atan(tv)};
            v.subImage = sub_image(vr_sc_, src);
        }
        vr_layer_ = XrCompositionLayerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        vr_layer_.space = xr_.local_space();
        vr_layer_.viewCount = 2;
        vr_layer_.views = vr_views_;
        layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vr_layer_));
    } else if (have_output_ && !vr && screen_has_image_) {
        if (phase_ == Phase::Loading || mode == ViewMode::Theater) {
            const Quad q = screen_quad();
            if (screen_is_cylinder()) {
                const float dist = std::fmax(settings_.screen_distance, 0.3f);
                for (int e = 0; e < eyes; ++e) {
                    XrCompositionLayerCylinderKHR& c = cylinders_[e];
                    c = XrCompositionLayerCylinderKHR{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                    c.space = xr_.local_space();
                    c.eyeVisibility = vis[e];
                    c.subImage = sub_image(screen_sc_, e);
                    c.pose.orientation = to_xr(rest_rot_);
                    c.pose.position = to_xr(add(rest_pos_, {0, settings_.screen_height, 0}));
                    c.radius = dist;
                    c.centralAngle = std::fmin(q.w / dist, 2.0f * kPi);
                    c.aspectRatio = aspect;
                    layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&c));
                }
            } else {
                for (int e = 0; e < eyes; ++e) {
                    XrCompositionLayerQuad& l = quads_[e];
                    l = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                    l.space = xr_.local_space();
                    l.eyeVisibility = vis[e];
                    l.subImage = sub_image(screen_sc_, e);
                    l.pose.orientation = to_xr(q.rot);
                    l.pose.position = to_xr(q.pos);
                    l.size = {q.w, q.h};
                    layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l));
                }
            }
        } else {
            // Immersive: a quad covering exactly the game's FOV, oriented like
            // the game camera that rendered the frame.
            const float d = std::fmax(settings_.immersive_distance, 0.3f);
            const float fov = std::fmin(std::fmax(settings_.game_fov, 30.0f), 170.0f);
            const float width = 2.0f * d * std::tan(rad(fov) * 0.5f);
            for (int e = 0; e < eyes; ++e) {
                XrCompositionLayerQuad& l = quads_[e];
                l = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                l.space = xr_.local_space();
                l.eyeVisibility = vis[e];
                l.subImage = sub_image(screen_sc_, e);
                l.pose.orientation = to_xr(immersive_rot_);
                l.pose.position = to_xr(add(to_vec(head_.position), rotate(immersive_rot_, {0, 0, -d})));
                l.size = {width, width / aspect};
                layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l));
            }
        }
    }

    if (phase_ == Phase::Loading && panel_has_image_) {
        const Quad q = panel_quad();
        XrCompositionLayerQuad& l = quads_[2];
        l = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        l.space = xr_.local_space();
        l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        l.subImage.swapchain = panel_sc_.handle();
        l.subImage.imageRect.extent = {kPanelWidth, kPanelHeight};
        l.pose.orientation = to_xr(q.rot);
        l.pose.position = to_xr(q.pos);
        l.size = {q.w, q.h};
        layers_.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l));
    }

    if (overlay_this_frame_)
        projection(overlay_layer_, overlay_views_, overlay_sc_, scene_.eye_width(), scene_.eye_height(), true);
}

}  // namespace vrl
