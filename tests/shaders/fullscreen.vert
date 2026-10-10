#version 450
// Full-screen triangle at a constant depth (test game).
layout(push_constant) uniform PC { vec4 color; float z; } pc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, pc.z, 1.0);
}
