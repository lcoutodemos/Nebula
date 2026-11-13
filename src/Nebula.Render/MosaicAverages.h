#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

inline std::vector<unsigned char> BuildMosaicAverages(const std::vector<unsigned char>& pixels,
    unsigned stridePixels, unsigned x, unsigned y, unsigned width, unsigned height, float aspect) {
    static const auto linear = [] {
        std::array<float, 256> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            float encoded = static_cast<float>(i) / 255;
            values[i] = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    std::vector<unsigned char> result(1876);
    size_t offset = 0;
    auto grid = [&](unsigned cols, unsigned rows, bool transpose) {
        for (unsigned row = 0; row < rows; ++row) for (unsigned col = 0; col < cols; ++col) {
            unsigned x0 = col * width / cols, x1 = std::max(x0 + 1, (col + 1) * width / cols);
            unsigned y0 = row * height / rows, y1 = std::max(y0 + 1, (row + 1) * height / rows);
            float sum[3]{}, alpha = 0;
            for (unsigned py = y0; py < std::min(y1, height); ++py)
                for (unsigned px = x0; px < std::min(x1, width); ++px) {
                    const auto* source = pixels.data() + ((y + py) * stridePixels + x + px) * 4;
                    float a = source[3] / 255.0f;
                    for (int channel = 0; channel < 3; ++channel) sum[channel] += linear[source[channel]] * a;
                    alpha += a;
                }
            unsigned address = transpose ? col * rows + row : row * cols + col;
            auto* pixel = result.data() + offset + address * 4;
            for (int channel = 0; channel < 3; ++channel) {
                float value = alpha > 0 ? sum[channel] / alpha : 0;
                float encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
                pixel[channel] = static_cast<unsigned char>(std::clamp(std::round(encoded * 255), 0.0f, 255.0f));
            }
            pixel[3] = static_cast<unsigned char>(std::clamp(std::round(alpha / ((x1 - x0) * (y1 - y0)) * 255), 0.0f, 255.0f));
        }
    };
    for (unsigned edge = 16; edge >= 1; edge /= 2) {
        grid(edge, edge, false);
        offset += edge * edge * 4;
    }
    // Canonical hierarchy: 16x8 landscape / 8x16 portrait, both row-major.
    if (aspect < 1) grid(8, 16, false);
    else grid(16, 8, false);
    return result;
}
