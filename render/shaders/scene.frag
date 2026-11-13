#version 450

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_world_xz;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 params;
    vec4 light;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    if (pc.params.x > 0.5) {
        // Ground: a line every 2 metres. fwidth() says how much the
        // coordinate changes per pixel, which keeps lines one pixel wide at
        // any distance instead of shimmering.
        vec2 cell = v_world_xz / 2.0;
        vec2 distance_to_line = abs(fract(cell - 0.5) - 0.5) / fwidth(cell);
        float line = 1.0 - min(min(distance_to_line.x, distance_to_line.y), 1.0);
        out_color = vec4(pc.color.rgb + vec3(0.13) * line, 1.0);
    } else {
        // Box: flat shading from one directional light plus ambient.
        float diffuse = max(dot(normalize(v_normal), pc.light.xyz), 0.0);
        out_color = vec4(pc.color.rgb * (0.45 + 0.55 * diffuse), 1.0);
    }
}
