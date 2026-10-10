// Minimal Vulkan "game" for end-to-end tests: renders into a real swapchain
// (VK_EXT_headless_surface) with a depth buffer, exactly like a game would,
// so the VRLayer layer can capture it.
//
// Scene: the left half of the screen is red at depth 0.25, the right half is
// blue at depth 0.6. Runs until SIGTERM.
//
// usage: vrlayer_test_game renderpass|dynamic

#include <vulkan/vulkan.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>

#include "fullscreen.frag.h"
#include "fullscreen.vert.h"

static volatile sig_atomic_t g_stop = 0;

#define CHECK(x)                                                         \
    do {                                                                 \
        VkResult r_ = (x);                                               \
        if (r_ != VK_SUCCESS) {                                          \
            fprintf(stderr, "test_game: %s failed: %d\n", #x, int(r_)); \
            return 1;                                                    \
        }                                                                \
    } while (0)

struct PC {
    float color[4];
    float z;
};

int main(int argc, char** argv) {
    signal(SIGTERM, [](int) { g_stop = 1; });
    const bool dynamic = argc > 1 && !strcmp(argv[1], "dynamic");
    const uint32_t W = 160, H = 120;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vrlayer-test-game";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));

    auto create_headless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
    VkHeadlessSurfaceCreateInfoEXT hsci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    CHECK(create_headless(instance, &hsci, nullptr, &surface));

    uint32_t n = 1;
    VkPhysicalDevice phys;
    vkEnumeratePhysicalDevices(instance, &n, &phys);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, dynamic ? &f13 : nullptr};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice device;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // Swapchain.
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = caps.minImageCount > 3 ? caps.minImageCount : 3;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = {W, H};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
    std::vector<VkImageView> views(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sci.imageFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &vci, nullptr, &views[i]));
    }

    // Depth buffer.
    VkImageCreateInfo dic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dic.imageType = VK_IMAGE_TYPE_2D;
    dic.format = VK_FORMAT_D32_SFLOAT;
    dic.extent = {W, H, 1};
    dic.mipLevels = 1;
    dic.arrayLayers = 1;
    dic.samples = VK_SAMPLE_COUNT_1_BIT;
    dic.tiling = VK_IMAGE_TILING_OPTIMAL;
    dic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage depth;
    CHECK(vkCreateImage(device, &dic, nullptr, &depth));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, depth, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (req.memoryTypeBits & (1u << i)) {
            mai.memoryTypeIndex = i;
            break;
        }
    VkDeviceMemory depth_mem;
    CHECK(vkAllocateMemory(device, &mai, nullptr, &depth_mem));
    CHECK(vkBindImageMemory(device, depth, depth_mem, 0));
    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image = depth;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format = dic.format;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depth_view;
    CHECK(vkCreateImageView(device, &dvci, nullptr, &depth_view));

    // Render pass + framebuffers (classic path).
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> fbs(n);
    if (!dynamic) {
        VkAttachmentDescription att[2] = {};
        att[0].format = sci.imageFormat;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att[1] = att[0];
        att[1].format = dic.format;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask = dep.srcStageMask;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 2;
        rpci.pAttachments = att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        CHECK(vkCreateRenderPass(device, &rpci, nullptr, &render_pass));
        for (uint32_t i = 0; i < n; ++i) {
            VkImageView fb_views[2] = {views[i], depth_view};
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = render_pass;
            fci.attachmentCount = 2;
            fci.pAttachments = fb_views;
            fci.width = W;
            fci.height = H;
            fci.layers = 1;
            CHECK(vkCreateFramebuffer(device, &fci, nullptr, &fbs[i]));
        }
    }

    // Pipeline.
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vs, fs;
    smci.codeSize = sizeof(kFullscreenVertSpv);
    smci.pCode = kFullscreenVertSpv;
    CHECK(vkCreateShaderModule(device, &smci, nullptr, &vs));
    smci.codeSize = sizeof(kFullscreenFragSpv);
    smci.pCode = kFullscreenFragSpv;
    CHECK(vkCreateShaderModule(device, &smci, nullptr, &fs));
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PC)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &sci.imageFormat;
    prci.depthAttachmentFormat = dic.format;
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic ? &prci : nullptr};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vin;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = layout;
    gpci.renderPass = render_pass;
    VkPipeline pipeline;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired;
    CHECK(vkCreateSemaphore(device, &semci, nullptr, &acquired));
    std::vector<VkSemaphore> rendered(n);
    for (auto& s : rendered) CHECK(vkCreateSemaphore(device, &semci, nullptr, &s));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    uint64_t frames = 0;
    while (!g_stop) {
        uint32_t idx;
        CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkClearValue clears[2] = {};
        clears[1].depthStencil = {1.0f, 0};
        if (dynamic) {
            VkImageMemoryBarrier b[2] = {};
            b[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[0].image = images[idx];
            b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b[1] = b[0];
            b[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            b[1].image = depth;
            b[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 0, 0, nullptr, 0, nullptr, 2, b);
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = views[idx];
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo da = ca;
            da.imageView = depth_view;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.clearValue = clears[1];
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {W, H}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
        } else {
            VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rbi.renderPass = render_pass;
            rbi.framebuffer = fbs[idx];
            rbi.renderArea = {{0, 0}, {W, H}};
            rbi.clearValueCount = 2;
            rbi.pClearValues = clears;
            vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0, 0, float(W), float(H), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        // Right half: blue, far.
        VkRect2D right{{int32_t(W / 2), 0}, {W / 2, H}};
        vkCmdSetScissor(cmd, 0, 1, &right);
        PC pc{{0, 0, 1, 1}, 0.6f};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        // Left half: red, near.
        VkRect2D left{{0, 0}, {W / 2, H}};
        vkCmdSetScissor(cmd, 0, 1, &left);
        pc = {{1, 0, 0, 1}, 0.25f};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        if (dynamic) {
            vkCmdEndRendering(cmd);
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[idx];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        } else {
            vkCmdEndRenderPass(cmd);
        }
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered[idx];
        CHECK(vkQueueSubmit(queue, 1, &si, fence));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &idx;
        CHECK(vkQueuePresentKHR(queue, &pi));

        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);
        ++frames;
    }

    vkDeviceWaitIdle(device);
    vkDestroyFence(device, fence, nullptr);
    for (auto s : rendered) vkDestroySemaphore(device, s, nullptr);
    vkDestroySemaphore(device, acquired, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    for (auto fb : fbs)
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
    vkDestroyImageView(device, depth_view, nullptr);
    vkDestroyImage(device, depth, nullptr);
    vkFreeMemory(device, depth_mem, nullptr);
    for (auto v : views) vkDestroyImageView(device, v, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    fprintf(stderr, "test_game: rendered %llu frames, clean exit\n", static_cast<unsigned long long>(frames));
    return 0;
}
