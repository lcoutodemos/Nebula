#pragma once
#include <array>
#include <cstddef>
#include <utility>

template<class T, std::size_t Capacity> class BoundedQueue {
    std::array<T, Capacity> entries_{};
    std::size_t first_ = 0, count_ = 0;
public:
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == Capacity; }
    std::size_t size() const { return count_; }
    T& front() { return entries_[first_]; }
    void push_back(T value) {
        entries_[(first_ + count_) % Capacity] = std::move(value);
        ++count_;
    }
    void push_front(T value) {
        first_ = (first_ + Capacity - 1) % Capacity;
        entries_[first_] = std::move(value);
        ++count_;
    }
    void pop_front() {
        entries_[first_] = {};
        first_ = (first_ + 1) % Capacity;
        --count_;
    }
    void clear() { while (!empty()) pop_front(); }
};
