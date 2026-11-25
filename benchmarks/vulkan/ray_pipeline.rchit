#version 460
#extension GL_EXT_ray_tracing : require

struct Primary {
    uint value;
    float t;
};
layout(location = 0) rayPayloadInEXT Primary primary;

void main() {
    uint instance = uint(gl_InstanceCustomIndexEXT);
    uint primitive = uint(gl_PrimitiveID);
    // ray_bench.comp と同じ値（Ray Query 版と同じく、0 になる組み合わせは非命中と区別しない）
    primary.value = ((instance * 0x9E3779B1u) ^ (primitive + 1u)) & 0x7fffffffu;
    primary.t = gl_HitTEXT;
}
