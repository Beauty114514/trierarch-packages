#version 450
layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv;
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 corner[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0),
                            vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec2 p = corner[gl_VertexIndex];
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, p), 0.0, 1.0);
    v_uv = mix(pc.uv.xy, pc.uv.zw, p);
}
