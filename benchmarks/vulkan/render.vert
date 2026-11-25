#version 450
// mode 0: 1 インスタンス = 四角形 1 枚（6 頂点）。xform = (x, y, w, h)。
// mode 1: grid x grid のメッシュ（1 セル 2 三角形）を頂点番号から生成し、xform の矩形に並べる。
layout(push_constant) uniform Push {
    vec4 color;
    vec4 xform;
    uint mode;
    uint iterations;
    uint grid;
    uint seed;
} pc;

layout(location = 0) out vec2 uv;

const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));

void main() {
    vec2 p;
    if (pc.mode == 0u) {
        p = corners[gl_VertexIndex % 6];
    } else {
        uint cell = uint(gl_VertexIndex) / 6u;
        uint cx = cell % pc.grid, cy = cell / pc.grid;
        p = (vec2(cx, cy) + corners[gl_VertexIndex % 6]) / float(pc.grid);
        // 頂点ごとに少し波打たせ、頂点シェーダーにも仕事をさせる
        p += 0.002 * sin(vec2(p.y, p.x) * 40.0 + float(pc.seed & 255u));
    }
    uv = p;
    vec2 pos = pc.xform.xy + p * pc.xform.zw;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
