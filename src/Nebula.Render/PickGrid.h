#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

struct Hit {
    int id, tier;
    float x, y, halfX, halfY, depth;
    uint32_t order;
    float dot;
};

inline bool Contains(const Hit& hit, float x, float y) {
    float radius = std::min(hit.halfX, hit.halfY) * (.14f + .86f * hit.dot);
    float qx = std::abs(x - hit.x) - hit.halfX + radius;
    float qy = std::abs(y - hit.y) - hit.halfY + radius;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) +
           std::min(std::max(qx, qy), 0.0f) - radius <= 0;
}

class PickGrid {
    struct Level { int step, columns, rows, offset; };
    struct Node { std::uint32_t hit; int next; };
    std::array<Level, 6> levels_{};
    std::vector<int> heads_;
    std::vector<Node> nodes_;
    int width_ = 0, height_ = 0;
    std::size_t used_ = 0;
public:
    void Reserve(std::size_t items, int width, int height) {
        width_ = std::max(width, 1); height_ = std::max(height, 1);
        int offset = 0, step = 16;
        for (auto& level : levels_) {
            level = {step, (width_ + step - 1) / step, (height_ + step - 1) / step, offset};
            offset += level.columns * level.rows;
            step *= 4;
        }
        heads_.resize(offset);
        nodes_.resize(items * 4);
    }
    void Begin() {
        std::fill(heads_.begin(), heads_.end(), -1);
        used_ = 0;
    }
    void Add(const Hit& hit, std::size_t i) {
            if (hit.x + hit.halfX < 0 || hit.y + hit.halfY < 0 ||
                hit.x - hit.halfX >= width_ || hit.y - hit.halfY >= height_) return;
            std::size_t l = 0;
            float diameter = 2 * std::max(hit.halfX, hit.halfY);
            while (l + 1 < levels_.size() && diameter > levels_[l].step) ++l;
            const auto& level = levels_[l];
            int x0 = static_cast<int>(std::clamp(hit.x - hit.halfX, 0.0f, static_cast<float>(width_ - 1))) / level.step;
            int x1 = static_cast<int>(std::clamp(hit.x + hit.halfX, 0.0f, static_cast<float>(width_ - 1))) / level.step;
            int y0 = static_cast<int>(std::clamp(hit.y - hit.halfY, 0.0f, static_cast<float>(height_ - 1))) / level.step;
            int y1 = static_cast<int>(std::clamp(hit.y + hit.halfY, 0.0f, static_cast<float>(height_ - 1))) / level.step;
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                auto& head = heads_[level.offset + y * level.columns + x];
                nodes_[used_] = {static_cast<std::uint32_t>(i), head};
                head = static_cast<int>(used_++);
            }
    }
    void Build(const std::vector<Hit>& hits) {
        Begin();
        for (std::size_t i = 0; i < hits.size(); ++i) Add(hits[i], i);
    }
    template<class F> void Visit(float x, float y, F&& visitor) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        for (const auto& level : levels_) {
            int cell = level.offset + static_cast<int>(y) / level.step * level.columns + static_cast<int>(x) / level.step;
            for (int node = heads_[cell]; node >= 0; node = nodes_[node].next) visitor(nodes_[node].hit);
        }
    }
};
