#version 460
#extension GL_EXT_ray_tracing : require

struct Primary {
    uint value;
    float t;
};
layout(location = 0) rayPayloadInEXT Primary primary;

void main() {
    primary.value = 0u;
    primary.t = -1.0;
}
