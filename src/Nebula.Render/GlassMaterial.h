#pragma once

namespace nebula_glass {
inline constexpr unsigned Downsample = 8;
inline constexpr int Iterations = 3;
inline constexpr float RadiusDip = 32.0f;
// Three Gaussian pairs at eighth resolution, including downsample and cubic B-spline
// reconstruction, approximate a 32-DIP three-sigma radius. Taps stay under one texel apart
// (0.40 x scale; 0.9 at 2.25x): wider spacing undersamples the kernel and, with a bilinear
// eighth-resolution upsample, made the frost look blocky over bright photos. The cubic
// reconstruction itself adds width, so the step is a little under one texel.
inline constexpr float KernelStepPerScale = 0.40f;
inline constexpr float ShadowRadiusDip = 18.0f;
inline constexpr float ShadowOffsetDip = 2.0f;
}
