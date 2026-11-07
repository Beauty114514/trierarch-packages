#version 450
layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv;
    float rot;
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 corner[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0),
                            vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec2 p = corner[gl_VertexIndex];
    vec4 rect = pc.rect;
    vec2 uv = mix(pc.uv.xy, pc.uv.zw, p);
    if (pc.rot > 0.5 && pc.rot < 1.5) {
        uv = vec2(uv.y, 1.0 - uv.x);
    } else if (pc.rot > 1.5 && pc.rot < 2.5) {
        uv = vec2(1.0 - uv.x, 1.0 - uv.y);
    } else if (pc.rot > 2.5) {
        uv = vec2(1.0 - uv.y, uv.x);
    }
    gl_Position = vec4(mix(rect.xy, rect.zw, p), 0.0, 1.0);
    v_uv = uv;
}
