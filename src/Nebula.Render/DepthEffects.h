#pragma once

namespace {
constexpr const char* DepthShader = R"hlsl(
cbuffer Lens : register(b0) {
    float4 lens; // focus distance, artistic lens scale px, max radius px, unused
    float4 frameInfo; // inverse full size xy, filter direction zw
};
Texture2D<float4> image : register(t0);
Texture2D<float> depths : register(t1);
Texture2D<float4> blurred : register(t2);
Texture2D<float> protection : register(t3);
SamplerState linearSampler : register(s0);
struct Fullscreen { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float coc(float depth) {
    return depth <= 0 ? lens.z : min(lens.z, lens.y * abs(depth - lens.x) / max(depth, 0.001));
}
float4 DownPS(Fullscreen input) : SV_TARGET {
    float4 result = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x)
        result += image.SampleLevel(linearSampler, input.uv + float2(x - 0.5, y - 0.5) * frameInfo.xy, 0);
    return result * 0.25;
}
float4 BlurPS(Fullscreen input) : SV_TARGET {
    float center = depths.SampleLevel(linearSampler, input.uv, 0);
    float radius = coc(center);
    float4 sum = 0;
    float total = 0;
    [unroll] for (int tap = -6; tap <= 6; ++tap) {
        float2 uv = input.uv + frameInfo.zw * frameInfo.xy * (tap / 6.0) * radius;
        float depth = depths.SampleLevel(linearSampler, uv, 0);
        float weight = exp(-0.5 * tap * tap / 6.25);
        if (depth > 0 && (center <= 0 || depth < center * 0.94)) weight *= 0.015;
        if (protection.SampleLevel(linearSampler, uv, 0) > 0.2) weight *= 0.015;
        sum += image.SampleLevel(linearSampler, uv, 0) * weight;
        total += weight;
    }
    return sum / max(total, 0.0001);
}
float4 ResolvePS(Fullscreen input) : SV_TARGET {
    float depth = depths.SampleLevel(linearSampler, input.uv, 0);
    float protectedPixel = protection.SampleLevel(linearSampler, input.uv, 0);
    float3 sharp = image.SampleLevel(linearSampler, input.uv, 0).rgb;
    if (protectedPixel >= 0.1 || coc(depth) <= 0.5) return float4(sharp, 1);
    float blend = smoothstep(0.5, 3.5, coc(depth));
    return float4(lerp(sharp, blurred.SampleLevel(linearSampler, input.uv, 0).rgb, blend), 1);
}
)hlsl";
}
