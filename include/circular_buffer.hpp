#pragma once

#include <atomic>
#include <vector>
#include <complex>
#include <cstddef>

// single-producer / single-consumer lock-free ring buffer
// capacity must be a power of two
template <typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(std::size_t capacity);

    // never blocks — overwrites oldest data when full
    void push(const T& item);

    // fills out[0..count-1], returns false if not enough samples available
    bool pop_batch(T* out, std::size_t count);

    std::size_t available() const;
    std::size_t capacity() const { return capacity_; }

private:
    std::vector<T>    buf_;
    const std::size_t capacity_;
    const std::size_t mask_;

    // padded to separate cache lines so the two threads don't false-share
    alignas(64) std::atomic<std::size_t> write_pos_{ 0 };
    alignas(64) std::atomic<std::size_t> read_pos_ { 0 };
};

extern template class CircularBuffer<std::complex<float>>;