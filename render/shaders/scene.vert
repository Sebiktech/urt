#version 450

// One shader pair draws everything for now: boxes and the ground grid.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

// Push constants: a small block of data sent with each draw call. Must match
// PushConstants in renderer.cpp field for field.
layout(push_constant) uniform Push {
    mat4 mvp;     // object space -> clip space
    vec4 color;   // rgb
    vec4 params;  // x: 1 = ground, 0 = box; y: ground size; zw: ground centre (world x, z)
    vec4 light;   // direction towards the light, in object space
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_world_xz;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    v_normal = in_normal;
    // Only meaningful for the ground: where this point is in the world.
    v_world_xz = in_position.xz * pc.params.y + pc.params.zw;
}
