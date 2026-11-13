#pragma once

#include <cmath>
#include <cstddef>

namespace nebula_highlight {
inline constexpr std::size_t MaxLines = 512;
inline constexpr float MixStepAt60Hz = 0.14f;
inline constexpr float WeightStepAt60Hz = 0.18f;
inline constexpr float SettleEpsilon = 0.002f;
inline constexpr float Dim = 0.82f;
inline constexpr float Lift = 0.18f;
inline constexpr float LineAlpha = 0.85f;
inline constexpr float LineVisibleMix = 0.02f;
inline constexpr float FocusMin = 260.0f / 32;
inline constexpr float FocusMax = 3200.0f / 32;

inline bool Ease(float& value, float target, float stepAt60Hz, float seconds, bool reducedMotion) {
    if (reducedMotion || std::abs(target - value) <= SettleEpsilon) {
        value = target;
        return false;
    }
    value += (target - value) * (1 - std::pow(1 - stepAt60Hz, seconds * 60));
    return true;
}

inline float Alpha(float base, float selectionMix, float selection, float weight, float mix) {
    float selectedContext = 1 - Dim * selectionMix * (1 - selection) * (1 - weight);
    float highlightedContext = weight > 0.01f ? 0.6f + 0.4f * weight : 1 - Dim * mix;
    return base * selectedContext * (highlightedContext + (1 - highlightedContext) * selection);
}
}
