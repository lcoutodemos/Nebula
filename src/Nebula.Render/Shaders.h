#pragma once

namespace {
constexpr const char* PhotoShader = R"hlsl(
cbuffer Camera : register(b0) {
    row_major float4x4 view;
    row_major float4x4 projection;
    float4 viewport;
};
cbuffer SceneLens : register(b1) {
    float4 lens; // focus distance, original ready, DoF enabled, atmosphere enabled
    float4 originalRegion;
};
Texture2D<float4> focusedOriginal : register(t15);
Texture2DArray<float4> coarse0 : register(t0);
Texture2DArray<float4> coarse1 : register(t1);
Texture2DArray<float4> coarse2 : register(t2);
Texture2DArray<float4> coarse3 : register(t3);
Texture2DArray<float4> mip128 : register(t4);
Texture2DArray<float4> mip256 : register(t5);
Texture2DArray<float4> mip512 : register(t6);
ByteAddressBuffer hierarchy0 : register(t7);
ByteAddressBuffer hierarchy1 : register(t8);
ByteAddressBuffer hierarchy2 : register(t9);
ByteAddressBuffer hierarchy3 : register(t10);
ByteAddressBuffer artHierarchy : register(t11);
SamplerState photoSampler : register(s0);
SamplerState pointSampler : register(s1);
struct TileRecord {
    float3 position; float halfX;
    uint color; float halfY; float aspect; float alpha;
    float4 effects;
    uint4 pages;
    float4 state;
    float4 content;
};
StructuredBuffer<TileRecord> tileRecords : register(t20);
StructuredBuffer<uint> drawOrder : register(t21);
struct Varying {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation float4 bounds : TEXCOORD1;
    nointerpolation float4 color : COLOR0;
    nointerpolation float4 flags : TEXCOORD2;
    nointerpolation float2 dimensions : TEXCOORD3;
    nointerpolation float4 effects : TEXCOORD4;
    nointerpolation float4 appearance : TEXCOORD5;
    nointerpolation float aspect : TEXCOORD6;
    nointerpolation float highlight : TEXCOORD7;
    nointerpolation float4 representations : TEXCOORD8;
    nointerpolation float4 transition : TEXCOORD9;
    nointerpolation float4 previewUv : TEXCOORD10;
    nointerpolation float viewZ : TEXCOORD11;
    nointerpolation uint2 imagePages : TEXCOORD12;
    nointerpolation uint artReady : TEXCOORD13;
    nointerpolation float4 content : TEXCOORD14;
};
Varying VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    TileRecord input = tileRecords[drawOrder[instance]];
    const float2 vertices[6] = {
        float2(-1,1), float2(1,1), float2(-1,-1),
        float2(-1,-1), float2(1,1), float2(1,-1)
    };
    Varying result;
    float2 corner = vertices[vertex];
    float4 center = mul(float4(input.position, 1), view);
    result.viewZ = center.z;
    float2 projected = float2(input.halfX * projection[0][0] * viewport.x,
                              input.halfY * projection[1][1] * viewport.y) * 0.5 / max(center.z, 0.001);
    float2 halfSize = max(projected, 0.00001);
    // Rasterize beyond the SDF edge so subpixel growth does not clip its AA fringe.
    float2 rasterHalf = halfSize + 4;
    center.xy += corner * rasterHalf * 2 * center.z /
                 (float2(projection[0][0], projection[1][1]) * viewport.xy);
    result.position = mul(center, projection);
    result.position.z = lerp(result.position.z, 0.5 * result.position.w + result.position.z * 0.25, input.state.z);
    result.position.z = lerp(result.position.z, 0.99999 * result.position.w, input.effects.x);
    result.uv = 0.5 + corner * (rasterHalf / halfSize) * float2(0.5,-0.5);
    uint repA = input.pages.w & 7, repB = (input.pages.w >> 3) & 7;
    result.bounds = float4(0,0,1,1);
    result.color = float4(input.color & 255, (input.color >> 8) & 255, (input.color >> 16) & 255, 255) / 255;
    uint thumbnail = input.pages.w >> 16;
    float drawn = 2 * max(halfSize.x, halfSize.y);
    result.flags = float4(thumbnail == 65535 ? -1 : (float)thumbnail, input.effects.x,
                         1 - smoothstep(5, 6, drawn), input.state.w);
    result.dimensions = halfSize * 2;
    result.effects = float4(input.effects.y, 1, (input.pages.w >> 6) & 7, (input.pages.w & 512) ? 1 : 0);
    result.appearance = float4(input.alpha, 0, input.state.z, input.effects.w);
    result.aspect = input.aspect;
    result.highlight = input.effects.z;
    result.representations = float4(input.pages.x == 0xffffffff ? -1 : (float)input.pages.x, 0, 0, repA);
    result.transition = float4(repB, input.state.x, input.state.y, (input.pages.w & 1024) ? 1 : 0);
    result.previewUv = float4(0,0,1,1);
    result.imagePages = input.pages.yz;
    result.artReady = (input.pages.w & 2048) ? 1 : 0;
    result.content = input.content;
    return result;
}
float3 linearColor(float3 c) {
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}
float3 mosaicTexel(Varying input, int level) {
    if (input.representations.x < 0) return linearColor(input.color.rgb);
    if (level == 5 && input.artReady == 0) level = 4;
    uint offsets[6] = {0,256,320,336,340,341};
    uint edge = level < 5 ? 16 >> level : 16;
    uint2 grid = level < 5 ? uint2(edge,edge) : (input.aspect < 1 ? uint2(8,16) : uint2(16,8));
    uint2 cell = min((uint2)(saturate(input.uv) * grid), grid - 1);
    uint photo = (uint)input.representations.x;
    uint address = ((photo & 32767) * 341 + offsets[level] + cell.y * grid.x + cell.x) * 4;
    uint chunk = photo >> 15;
    uint packed;
    if (level == 5) packed = artHierarchy.Load((photo * 128 + cell.y * grid.x + cell.x) * 4);
    else packed = chunk == 0 ? hierarchy0.Load(address) : chunk == 1 ? hierarchy1.Load(address) :
                  chunk == 2 ? hierarchy2.Load(address) : hierarchy3.Load(address);
    return linearColor(float3((packed >> 16) & 255, (packed >> 8) & 255, packed & 255) / 255);
}
float3 samplePage(Varying input, uint token) {
        uint pool = token >> 16, slot = token & 65535;
        float edge = 64 << pool;
        float2 dimensions = max(1, floor(edge * float2(min(input.aspect,1), min(1/input.aspect,1)) + 0.5));
        float2 offset = floor((edge - dimensions) * 0.5);
        float2 uv = lerp((offset + 0.5) / edge, (offset + dimensions - 0.5) / edge, input.uv);
        float4 photo;
        if (pool == 0) {
            uint array = slot / 2048;
            float3 address = float3(uv, slot % 2048);
            photo = array == 0 ? coarse0.Sample(photoSampler, address) : array == 1 ? coarse1.Sample(photoSampler, address) :
                    array == 2 ? coarse2.Sample(photoSampler, address) : coarse3.Sample(photoSampler, address);
        } else if (pool == 1) photo = mip128.Sample(photoSampler, float3(uv, slot));
        else if (pool == 2) photo = mip256.Sample(photoSampler, float3(uv, slot));
        else photo = mip512.Sample(photoSampler, float3(uv, slot));
        return lerp(linearColor(input.color.rgb), photo.rgb, photo.a);
}
float3 meanColor(Varying input) {
    float luminance = dot(input.color.rgb, float3(0.299,0.587,0.114));
    return linearColor(saturate(luminance + (input.color.rgb - luminance) * 1.12));
}
float3 contentColor(Varying input) {
    float3 mean = meanColor(input);
    // This untextured branch is the dot path. It shares size, SDF and depth order
    // with tiles; no HDR accumulation or material switch can create a cap wall.
    if (input.content.x >= 4) return mean;
    float3 mosaic;
    if (input.effects.z > 0.5) {
        int levels[6] = {0,4,3,2,1,5};
        mosaic = mosaicTexel(input, levels[(int)input.effects.z]);
    } else {
        float level = input.content.x;
        int low = (int)floor(level);
        mosaic = lerp(mosaicTexel(input, low), mosaicTexel(input, min(low + 1, 4)), frac(level));
    }
    // At t0 use the supplied mean (with its saturation lift) exactly, even
    // when the newly decoded hierarchy's measured mean is slightly different.
    mosaic += (mean - mosaicTexel(input, 4)) * (input.content.x * 0.25);
    float3 color = lerp(mean, saturate(mosaic), input.content.w);
    if (input.effects.z > 0.5) return color;
    if (input.flags.x >= 0 && input.content.y > 0)
        color = lerp(color, samplePage(input, (uint)input.flags.x), input.content.y);
    if (input.content.z > 0) {
        float3 a = input.imagePages.x == 0xffffffff ? color : samplePage(input, input.imagePages.x);
        float3 b = input.imagePages.y == 0xffffffff ? color : samplePage(input, input.imagePages.y);
        color = lerp(color, lerp(b, a, smoothstep(0, 1, input.transition.y)), input.content.z);
    }
    return color;
}
struct PhotoOutput { float4 color : SV_TARGET0; float depth : SV_TARGET1; float protection : SV_TARGET2; };
void OpaqueDepthPS(Varying input) {
    clip(input.flags.w - 0.999);
    float2 halfSize = input.dimensions * 0.5;
    float radius = min(halfSize.x, halfSize.y) * lerp(0.14, 1, input.flags.z);
    float2 q = abs((input.uv * 2 - 1) * halfSize) - (halfSize - radius);
    float distance = length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
    clip(-distance - 1.5);
}
PhotoOutput PS(Varying input) {
    float2 halfSize = input.dimensions * 0.5;
    float radius = min(halfSize.x, halfSize.y) * lerp(0.14, 1, input.flags.z);
    float2 local = (input.uv * 2 - 1) * halfSize;
    float2 q = abs(local) - (halfSize - radius);
    float rounded = length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
    float d = rounded;
    float aa = max(fwidth(d), 0.65);
    float coverage = 1 - smoothstep(-0.5, 0.5, d);
    float highlightWidth = aa * (1.5 + 1.2 * input.highlight);
    float highlightStrength = input.highlight > 0 ? max(input.highlight, 0.35 * input.effects.x * (1 - input.flags.z)) : 0;
    float highlightRing = (1 - smoothstep(0, highlightWidth, abs(d))) * highlightStrength * input.appearance.w;
    clip(max(coverage, highlightRing) * input.flags.w - 0.003);

    float3 color = contentColor(input);
    if (lens.y > 0 && input.transition.w > 0.5 && input.effects.z < 0.5 &&
        all(input.uv >= originalRegion.xy) && all(input.uv <= originalRegion.zw)) {
        float2 uv = (input.uv - originalRegion.xy) / max(originalRegion.zw - originalRegion.xy, 0.000001);
        float4 original = focusedOriginal.Sample(photoSampler, uv);
        color = lerp(color, lerp(linearColor(input.color.rgb), original.rgb, original.a), lens.y);
    }
    float edge = 1 - smoothstep(-aa * 2.5, 0, d + aa * 2);
    color = lerp(color, color * 0.86, edge * 0.5 * (1 - input.flags.z) * (1 - max(input.flags.y, input.effects.x)));
    color *= 1 + 0.06 * input.effects.x;
    float3 ground = linearColor(input.effects.w > 0.5 ? float3(239,239,237) / 255 : float3(20,20,22) / 255);
    float protect = saturate(max(input.flags.y, max(input.highlight, input.effects.x)));
    if (lens.w > 0.5 && protect <= 0.01) {
        // Optional, default-off atmosphere follows the same continuous con curve.
        float luminance = dot(color, float3(0.2126,0.7152,0.0722));
        color = lerp(color, luminance.xxx, 0.25 * (1 - input.appearance.w));
    }
    // Distance contrast: alpha *= con, never dilute the .42 far endpoint.
    float alpha = saturate(input.appearance.x) * input.appearance.w;
    color = lerp(ground, color, alpha);
    float hover = input.effects.x;
    float ring = 1 - smoothstep(0.8, 2.0, abs(d + 0.6));
    float3 monochrome = input.effects.w > 0.5 ? linearColor(float3(0.15,0.15,0.15)) : float3(1,1,1);
    color = lerp(color, monochrome, input.highlight > 0 ? 0 : ring * hover * 0.48);
    color = lerp(color, monochrome, highlightRing);
    float3 selection = linearColor(float3(0.27, 0.57, 1.0));
    color = lerp(color, selection, ring * input.flags.y);
    PhotoOutput output;
    output.color = float4(color, max(coverage, highlightRing) * input.flags.w);
    output.depth = input.viewZ;
    output.protection = protect;
    return output;
}
)hlsl";

constexpr const char* LineShader = R"hlsl(
cbuffer Camera : register(b0) {
    row_major float4x4 view;
    row_major float4x4 projection;
    float4 viewport;
};
struct LineInput { float3 position : POSITION; float alpha : TEXCOORD0; };
struct LineOutput { float4 position : SV_POSITION; float alpha : TEXCOORD0; };
LineOutput LineVS(LineInput input) {
    LineOutput output;
    output.position = mul(mul(float4(input.position, 1), view), projection);
    output.alpha = input.alpha;
    return output;
}
float4 LinePS(LineOutput input) : SV_TARGET {
    float ink = viewport.z > 0.5 ? pow((0.15 + 0.055) / 1.055, 2.4) : 1;
    return float4(ink, ink, ink, input.alpha);
}
)hlsl";

constexpr const char* GlassShader = R"hlsl(
cbuffer Glass : register(b0) {
    float4 outputInfo; // drawable width, height, panel count, light theme
    float4 blurInfo;   // inverse source size xy, pass axis zw
    float4 bounds[8];
    float4 material[8]; // radius, tint, opacity, unused
    float4 displayScale; // composition scale xy, shadow radius/offset DIPs
};
Texture2D<float4> original : register(t0);
Texture2D<float4> blurred : register(t1);
SamplerState linearSampler : register(s0);
struct Fullscreen { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Fullscreen FullVS(uint vertex : SV_VertexID) {
    Fullscreen output;
    output.uv = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
float4 DownsamplePS(Fullscreen input) : SV_TARGET {
    // Sixteen bilinear taps prefilter the complete 8x8 source footprint.
    float2 texel = blurInfo.xy;
    float4 total = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            total += original.SampleLevel(linearSampler, input.uv + texel * float2(x * 2 - 3, y * 2 - 3), 0);
    return total / 16;
}
float4 BlurPS(Fullscreen input) : SV_TARGET {
    float2 stepSize = blurInfo.xy * blurInfo.zw;
    // Bilinear paired taps implement a normalized nine-tap Gaussian.
    float4 value = original.SampleLevel(linearSampler, input.uv, 0) * 0.2270270270;
    value += original.SampleLevel(linearSampler, input.uv + stepSize * 1.3846153846, 0) * 0.3162162162;
    value += original.SampleLevel(linearSampler, input.uv - stepSize * 1.3846153846, 0) * 0.3162162162;
    value += original.SampleLevel(linearSampler, input.uv + stepSize * 3.2307692308, 0) * 0.0702702703;
    value += original.SampleLevel(linearSampler, input.uv - stepSize * 3.2307692308, 0) * 0.0702702703;
    return value;
}
float3 linearColor(float3 c) {
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}
float roundedDistance(float2 local, float2 halfSize, float radius) {
    float2 q = abs(local) - (halfSize - radius);
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
}
// Cubic B-spline reconstruction of the eighth-resolution blur in four bilinear taps. A
// bilinear 8x upsample is only C0, so its texel grid shows as blocks over bright photos.
float3 smoothBackdrop(float2 uv) {
    float2 size;
    blurred.GetDimensions(size.x, size.y);
    float2 p = uv * size - 0.5, i = floor(p), f = p - i;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6, w1 = (4 - 6 * f2 + 3 * f3) / 6;
    float2 w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6, w3 = f3 / 6;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 c0 = (i - 0.5 + w1 / g0) / size, c1 = (i + 1.5 + w3 / g1) / size;
    return g0.y * (g0.x * blurred.SampleLevel(linearSampler, float2(c0.x, c0.y), 0).rgb +
                   g1.x * blurred.SampleLevel(linearSampler, float2(c1.x, c0.y), 0).rgb) +
           g1.y * (g0.x * blurred.SampleLevel(linearSampler, float2(c0.x, c1.y), 0).rgb +
                   g1.x * blurred.SampleLevel(linearSampler, float2(c1.x, c1.y), 0).rgb);
}
float4 CompositePS(Fullscreen input) : SV_TARGET {
    float3 color = original.SampleLevel(linearSampler, input.uv, 0).rgb;
    float2 pixelPosition = input.position.xy;
    for (int i = 0; i < (int)outputInfo.z; ++i) {
        if (material[i].z <= 0) continue;
        float2 halfSize = bounds[i].zw * 0.5;
        float2 local = pixelPosition - bounds[i].xy - halfSize;
        float radius = min(material[i].x, min(halfSize.x, halfSize.y));
        float distance = roundedDistance(local, halfSize, radius);
        float3 finished = color;
        if (distance > 0) {
            float shadowDistance = roundedDistance(local - float2(0, displayScale.w * displayScale.y), halfSize, radius);
            float shadowRadius = max(1, displayScale.z * max(displayScale.x, displayScale.y));
            // Ambient, not a drop: quadratic falloff so the edge has no visible band.
            float falloff = 1 - smoothstep(0, shadowRadius, max(shadowDistance, 0));
            float shadow = falloff * falloff * (outputInfo.w > 0.5 ? 0.05 : 0.11);
            finished *= 1 - shadow;
        }
        float coverage = 1 - smoothstep(-0.5, 0.5, distance);
        if (coverage > 0) {
            float3 backdrop = smoothBackdrop(input.uv);
            float luminance = dot(backdrop, float3(0.2126,0.7152,0.0722));
            backdrop = saturate(lerp(luminance.xxx, backdrop, 1.35));
            float3 tint = linearColor(outputInfo.w > 0.5 ? float3(245,245,243) / 255 : float3(28,28,31) / 255);
            float tintStrength = saturate(material[i].y * (outputInfo.w > 0.5 ? 1 : 0.60 / 0.55));
            float3 frosted = lerp(backdrop, tint, tintStrength);
            float grain = frac(52.9829189 * frac(dot(floor(pixelPosition), float2(0.06711056, 0.00583715)))) - 0.5;
            frosted += grain * (outputInfo.w > 0.5 ? 0.004 : 0.0015);
            float vertical = saturate((local.y + halfSize.y) / (halfSize.y * 2));
            float topLeft = smoothstep(0.6, 1, -local.x / halfSize.x) * smoothstep(0.6, 1, -local.y / halfSize.y);
            float highlight = outputInfo.w > 0.5 ? lerp(0.55, 0.20, vertical) + 0.04 * topLeft
                                                 : lerp(0.14, 0.035, vertical) + 0.025 * topLeft;
            float hairline = saturate(1 - abs(distance + 0.5));
            frosted = lerp(frosted, float3(1,1,1), hairline * highlight);
            finished = lerp(finished, saturate(frosted), coverage);
        }
        if (outputInfo.w > 0.5 && distance > 0)
            finished *= 1 - saturate(1 - abs(distance - 0.5)) * 0.06;
        color = lerp(color, finished, material[i].z);
    }
    return float4(color, 1);
}
)hlsl";
}
