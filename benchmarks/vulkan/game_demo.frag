#version 450
layout(std140, set = 0, binding = 0) uniform FrameUBO {
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
    vec4 params;
} ubo;

layout(set = 0, binding = 2) uniform sampler2D texAtlas;

layout(push_constant) uniform DrawPush {
    uint baseInstance;
    uint mode;
    uint seed;
    uint iterations;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 albedo = texture(texAtlas, vUv).rgb;
    uint taps = max(pc.iterations, 1u);
    for (uint i = 1u; i < taps; ++i) {
        vec2 off = vec2(float(i) * 0.007, float(i) * 0.011);
        albedo += texture(texAtlas, vUv + off).rgb;
    }
    albedo /= float(taps);

    if (pc.mode == 2u) {
        float vig = 1.0 - 0.25 * dot(vUv - 0.5, vUv - 0.5);
        outColor = vec4(albedo * vColor.rgb * vig, vColor.a);
        return;
    }

    vec3 N = normalize(vNormal);
    vec3 L = normalize(ubo.lightDir.xyz);
    vec3 V = normalize(ubo.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    float roughness = clamp(ubo.params.x, 0.1, 0.95);
    float specPow = mix(64.0, 8.0, roughness);
    float spec = pow(NdotH, specPow);
    float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);

    vec3 L2 = normalize(vec3(-0.6, 0.5, 0.6));
    float fill = max(dot(N, L2), 0.0) * 0.35;

    vec3 lit = albedo * vColor.rgb * (0.18 + NdotL * ubo.lightColor.rgb + fill) +
               vec3(spec * (0.35 + fresnel)) * ubo.lightColor.rgb;
    if (pc.mode == 1u) {
        lit += 0.15 * vColor.rgb * (0.5 + 0.5 * sin(vUv.x * 24.0 + ubo.params.y));
    }
    outColor = vec4(clamp(lit, 0.0, 1.0), vColor.a);
}
