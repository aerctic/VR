#version 450
layout(push_constant) uniform PC { vec4 color; float z; } pc;
layout(location = 0) out vec4 out_color;
void main() { out_color = pc.color; }
