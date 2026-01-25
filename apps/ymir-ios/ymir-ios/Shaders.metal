#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut ymir_vertex(uint vertex_id [[vertex_id]]) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };
    const float2 texCoords[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };
    VertexOut out;
    out.position = float4(positions[vertex_id], 0.0, 1.0);
    out.texCoord = texCoords[vertex_id];
    return out;
}

fragment float4 ymir_fragment(VertexOut in [[stage_in]], texture2d<float> colorTex [[texture(0)]]) {
    constexpr sampler s(address::clamp_to_edge, filter::nearest);
    float2 uv = float2(in.texCoord.x, 1.0 - in.texCoord.y);
    return colorTex.sample(s, uv);
}
