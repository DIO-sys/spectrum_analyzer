#pragma once

#include <atomic>
#include <vector>
#include <complex>
#include <cstddef>

// single-producer / single-consumer lock-free ring buffer
// capacity must be a power of two
//we don'te really need the template because we only use complex floats, but we could reuse it for another type if we wanted to. 
// i would change it for this project, but i want to keep it as a demonstration of how to write a reusable circular buffer class.

template <typename T>
class CircularBuffer {
public:
    //explicit makes it so that this is the only syntax to instantiate a buffer
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
    //this is the wrapping function how we quickly wrap around the buffer once we reach the end 
    const std::size_t mask_;

    // padded to separate cache lines so the two threads don't false-share
    alignas(64) std::atomic<std::size_t> write_pos_{ 0 };
    alignas(64) std::atomic<std::size_t> read_pos_ { 0 };
};

//extern template class is how we say that this will be instantiated later, because we want one shared instantiation
extern template class CircularBuffer<std::complex<float>>;