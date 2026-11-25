#version 450
layout(push_constant) uniform Push {
    vec4 color;
    vec4 xform;
    uint mode;
    uint iterations;
    uint grid;
    uint seed;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 sum = texture(tex, uv);
    // テクスチャの読み取り回数で、フラグメント負荷（帯域とサンプラー）を調整する
    for (uint i = 1u; i < pc.iterations; ++i) {
        sum += texture(tex, uv * (1.0 + float(i) * 0.37) + vec2(float(i) * 0.011, 0.0));
    }
    outColor = vec4(sum.rgb / float(max(pc.iterations, 1u)), 1.0) * pc.color;
}
