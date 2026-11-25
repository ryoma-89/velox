#version 450
layout(std140, set = 0, binding = 0) uniform FrameUBO {
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
    vec4 params;
} ubo;

layout(std430, set = 0, binding = 1) readonly buffer Instances {
    vec4 data[];
} inst;

layout(push_constant) uniform DrawPush {
    uint baseInstance;
    uint mode;
    uint seed;
    uint iterations;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;

const vec3 kCubePos[36] = vec3[](
    // Front (+Z)
    vec3(-1,-1, 1), vec3( 1,-1, 1), vec3( 1, 1, 1), vec3(-1,-1, 1), vec3( 1, 1, 1), vec3(-1, 1, 1),
    // Back (-Z)
    vec3( 1,-1,-1), vec3(-1,-1,-1), vec3(-1, 1,-1), vec3( 1,-1,-1), vec3(-1, 1,-1), vec3( 1, 1,-1),
    // Top (+Y)
    vec3(-1, 1, 1), vec3( 1, 1, 1), vec3( 1, 1,-1), vec3(-1, 1, 1), vec3( 1, 1,-1), vec3(-1, 1,-1),
    // Bottom (-Y)
    vec3(-1,-1,-1), vec3( 1,-1,-1), vec3( 1,-1, 1), vec3(-1,-1,-1), vec3( 1,-1, 1), vec3(-1,-1, 1),
    // Right (+X)
    vec3( 1,-1, 1), vec3( 1,-1,-1), vec3( 1, 1,-1), vec3( 1,-1, 1), vec3( 1, 1,-1), vec3( 1, 1, 1),
    // Left (-X)
    vec3(-1,-1,-1), vec3(-1,-1, 1), vec3(-1, 1, 1), vec3(-1,-1,-1), vec3(-1, 1, 1), vec3(-1, 1,-1)
);

const vec3 kFaceNorm[6] = vec3[](
    vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, 1, 0),
    vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0)
);

const vec2 kQuadUv[6] = vec2[](
    vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

void main() {
    uint vid = uint(gl_VertexIndex);
    if (pc.mode == 2u) {
        vec2 uv = kQuadUv[vid % 6u];
        vUv = uv;
        vWorldPos = vec3(uv * 2.0 - 1.0, 0.05);
        vNormal = vec3(0.0, 0.0, 1.0);
        vColor = vec4(0.95, 0.92, 0.90, 0.12);
        gl_Position = vec4(uv * 2.0 - 1.0, 0.05, 1.0);
        return;
    }

    uint instIdx = (pc.baseInstance + uint(gl_InstanceIndex)) & 16383u;
    vec4 tr = inst.data[instIdx * 2u + 0u];
    vec4 col = inst.data[instIdx * 2u + 1u];

    uint v = vid % 36u;
    vec3 local = kCubePos[v] * tr.w;
    vec3 nrm = kFaceNorm[v / 6u];

    float c = cos(col.w);
    float s = sin(col.w);
    vec3 rotPos = vec3(local.x * c - local.z * s, local.y, local.x * s + local.z * c);
    vec3 rotNrm = vec3(nrm.x * c - nrm.z * s, nrm.y, nrm.x * s + nrm.z * c);

    vec3 world = tr.xyz + rotPos;
    vWorldPos = world;
    vNormal = normalize(rotNrm);
    vUv = kQuadUv[v % 6u] * 0.5 + vec2(float(instIdx & 1u) * 0.5, float((instIdx >> 1u) & 1u) * 0.5);
    vColor = vec4(col.rgb, pc.mode == 1u ? 0.65 : 1.0);

    float invZ = 1.0 / (1.0 + world.z * 0.65);
    gl_Position = vec4(world.x * invZ, world.y * invZ * (16.0 / 9.0), clamp(world.z, 0.01, 0.99), 1.0);
}
