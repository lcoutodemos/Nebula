#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace nebula_lod {
using Histogram = std::array<uint32_t, 64>;
using Caps = std::array<float, 3>;
inline constexpr Caps Floors{10, 32, 110};
inline constexpr std::array<uint32_t, 3> Admissions{3000, 3800, 480};
inline float Smooth(float value) { return value * value * (3 - 2 * value); }
inline float Amount(float at60Hz, float seconds) { return 1 - std::pow(1 - at60Hz, seconds * 60); }
inline float Band(float start, float end, float value) {
    return Smooth(std::clamp((value - start) / (end - start), 0.0f, 1.0f));
}
struct Content {
    float level = 4, thumbnail = 0, preview = 0;
};
// Bounded point size; the revised design uses d as the drawn edge/diameter.
inline float PointSize(float edge) { return std::clamp(.5f * edge, 1.4f, 5.0f); }
inline float DrawSize(float edge, float center) {
    float point = PointSize(edge);
    if (edge <= center) return point;
    if (edge >= 2 * center) return edge;
    return point + (edge - point) * Band(center, 2 * center, edge);
}
// Complete the circle-to-rounded-tile morph before any primitive exceeds6px.
inline float TileShape(float drawn) { return Band(5, 6, drawn); }
inline Content Appearance(float edge, const Caps& caps) {
    if (edge <= caps[0]) return {};
    float progress = edge >= caps[1] ? 1 : std::clamp(std::log2(edge / caps[0]) / std::log2(caps[1] / caps[0]), 0.0f, 1.0f);
    return {4 * (1 - progress), Band(caps[1], caps[1] * 1.3f, edge), Band(caps[2], caps[2] * 1.3f, edge)};
}
inline int AppearanceTier(float edge, const Caps& caps) {
    return edge <= caps[0] ? 0 : edge <= caps[1] ? 1 : edge <= caps[2] ? 2 : 3;
}
// Attention: the 3px floor is screen-space, not an animated world size.
inline float Footprint(float raw, float attention) { return 3 + attention * (std::max(3.0f, raw) - 3); }
// Nothing is faded for being in front of the orbit target. An earlier
// orbit-relative band (.12D..35D) made a third of a zoomed-out cloud see-through. Only
// two things fade now, both tied to the photo itself: one about to slice the camera's near plane
// (absolute tile widths) and, below, one about to fill the window.
inline constexpr float NearGuardStart = .1f, NearGuardEnd = .3f;
inline constexpr float OversizeStart = .6f, OversizeEnd = .9f; // fractions of the window height
inline float NearVisibility(float depth, bool selected, bool lit, bool gathered) {
    if (depth <= 0) return 0;
    return selected || lit || gathered ? 1 : Band(NearGuardStart, NearGuardEnd, depth);
}
inline float Visibility(float depth, float drawn, float height, bool selected, bool lit, bool gathered) {
    return NearVisibility(depth, selected, lit, gathered) *
           (selected ? 1 : 1 - Band(OversizeStart * height, OversizeEnd * height, drawn));
}
inline std::array<float, 2> HalfExtents(float edge, float aspect, float shape) {
    return {edge * .5f * (1 + (std::min(aspect, 1.0f) - 1) * shape),
            edge * .5f * (1 + (std::min(1 / aspect, 1.0f) - 1) * shape)};
}
inline bool Residency(float& value, bool resident, float seconds, bool reducedMotion) {
    if (!resident) { value = 0; return false; }
    if (value >= 1) return false;
    value = reducedMotion ? 1 : std::min(1.0f, value + seconds / .18f);
    return value < 1;
}

// Wheel zoom: each notch's log-distance step is released over ~70 ms into the camera target,
// which itself eases at 95 ms. Two cascaded lags start the motion at zero speed and never
// overshoot, so a notch glides instead of kicking, and consecutive notches blend together.
inline constexpr float ZoomReleaseSeconds = .07f;
inline float TakeZoom(float& pending, float seconds, bool reducedMotion) {
    float taken = reducedMotion ? pending : pending * (1 - std::exp(-seconds / ZoomReleaseSeconds));
    pending -= taken;
    if (std::abs(pending) < 1e-4f) { taken += pending; pending = 0; }
    return taken;
}

// Include effective edges from 3px, then ease three caps.
inline int Bin(float edge) { return std::clamp(static_cast<int>(std::log2(std::max(edge, 1.0f)) * 8), 0, 63); }
inline float CapEdge(const Histogram& histogram, uint32_t admitted, float floor) {
    uint32_t count = 0;
    for (int bin = 63; bin >= 0; --bin) {
        count += histogram[bin];
        if (count > admitted) return std::max(floor, std::exp2((bin + 1) / 8.0f));
    }
    return floor;
}
// Exact same-frame rank avoids both cap easing and 1/8-octave histogram steps.
// Zero entries represent photos excluded from the histogram. Strictly more
// than this edge can admit at most 3000 photos, including tied populations.
inline float SizeCap(std::span<float> edges) {
    if (edges.size() <= Admissions[0]) return Floors[0];
    auto cutoff = edges.begin() + Admissions[0];
    std::nth_element(edges.begin(), cutoff, edges.end(), [](float a, float b) { return a > b; });
    return std::max(Floors[0], *cutoff);
}
inline bool UpdateCaps(Caps& caps, const Histogram& histogram, float seconds, bool reducedMotion) {
    bool moving = false;
    float amount = reducedMotion ? 1 : Amount(0.1f, seconds);
    Caps targets{};
    for (size_t i = 0; i < caps.size(); ++i) {
        targets[i] = CapEdge(histogram, Admissions[i], Floors[i]);
        // Histogram quotas can collide/invert. Keep log bands nonzero and leave
        // the same30% span used by the image crossfades, rather than creating a wall.
        if (i) targets[i] = std::max(targets[i], targets[i-1] * 1.3f);
    }
    for (size_t i = 0; i < caps.size(); ++i) {
        float target = targets[i];
        if (reducedMotion || std::abs(target - caps[i]) <= std::max(0.001f, target * 0.00001f)) caps[i] = target;
        else { caps[i] += (target - caps[i]) * amount; moving = true; }
    }
    return moving;
}

// Tiers: +/-15% hysteresis, twelve presented-frame dwell.
inline int WantedTier(float edge, int current, const Caps& caps) {
    int tier = current;
    while (tier < 3 && edge > caps[tier] * 1.15f) ++tier;
    while (tier > 0 && edge < caps[tier - 1] * 0.85f) --tier;
    return tier;
}
inline bool UpdateTier(int& tier, uint32_t& since, float edge, const Caps& caps, uint32_t frame,
                       bool selected, bool receded, bool reducedMotion) {
    int wanted = WantedTier(edge, tier, caps);
    if (selected) wanted = 3;
    // Keep the established query contract: an absent selected result is context.
    if (receded) wanted = 0;
    if (wanted == tier) return false;
    if (!reducedMotion && !selected && !receded && frame - since < 12) return true;
    tier = wanted; since = frame;
    return false;
}

// Only tier 3 may refine beyond 64px; these bands choose storage within that tier.
inline int Quality(int tier, float drawnEdge, int previous, bool original) {
    if (tier < 2) return -1;
    if (tier == 2) return 0;
    int maximum = original ? 3 : 2;
    int quality = std::clamp(previous, 1, maximum);
    while (quality < maximum && drawnEdge > (64 << quality) * 1.15f) ++quality;
    while (quality > 1 && drawnEdge < (64 << (quality - 1)) * 0.85f) --quality;
    return quality;
}

// One radial camera-distance curve, in native tile-width units.
inline float Contrast(float squaredDistance, bool exempt) {
    if (exempt || squaredDistance <= 225) return 1;
    if (squaredDistance >= 5625) return 0.42f;
    float t = std::clamp((0.5f * std::log2(squaredDistance) - std::log2(15.0f)) /
                         (std::log2(75.0f) - std::log2(15.0f)), 0.0f, 1.0f);
    return 0.42f + 0.58f * (1 - Smooth(t));
}

// Ease the dimensionless A/P/T factor, never projected pixels. At fixed raw this is
// a .18 interpolation of att=edge/raw. Recompute its 3/raw floor term at
// today's depth: easing that term as well would make distant photos rebound.
inline bool Attention(float& value, float target, float amount, bool reducedMotion) {
    float difference = target - value;
    if (reducedMotion || std::abs(difference) <= 0.003f) { value = target; return false; }
    float next = value + difference * amount;
    if (next == value) { value = target; return false; } // Float precision must not keep idle rendering alive.
    value = next;
    return true;
}

// Representation crossfade: .09/60Hz frame, smoothstep in the fragment shader.
inline bool Represent(int best, int& current, int& previous, float& mix, float seconds, bool reducedMotion) {
    if (reducedMotion) { previous = current; current = best; mix = 1; return false; }
    if (best != current) {
        if (best == previous && mix < 1) {
            std::swap(current, previous); mix = 1 - mix;
        } else if (mix >= 0.99f) {
            previous = current; current = best; mix = 0;
        }
    }
    bool moving = mix < 1 || best != current;
    mix = reducedMotion ? 1 : std::min(1.0f, mix + 0.09f * seconds * 60);
    return moving;
}
}
