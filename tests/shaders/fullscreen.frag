#version 450
layout(push_constant) uniform PC { vec4 color; float z; float use_ubo; } pc;
// Camera block as a game would upload it (std140, column-major).
layout(set = 0, binding = 0) uniform Camera { mat4 world; mat4 view; mat4 proj; mat4 view_proj; } cam;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = pc.color;
    // Green encodes the camera yaw the GPU actually sees: 0.5 + 0.5 * cos(yaw).
    if (pc.use_ubo > 0.5) out_color.g = 0.5 + 0.5 * cam.view[0][0];
}
