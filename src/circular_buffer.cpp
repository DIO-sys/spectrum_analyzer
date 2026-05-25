#include "circular_buffer.hpp"
#include <cassert>

using namespace std;

template <typename T>
CircularBuffer<T>::CircularBuffer(size_t capacity)
    : buf_(capacity), capacity_(capacity), mask_(capacity - 1)
{
    assert(capacity > 0 && (capacity & mask_) == 0 && "capacity must be a power of two");
}

template <typename T>
void CircularBuffer<T>::push(const T& item) {
    size_t write = write_pos_.load(memory_order_relaxed);
    buf_[write & mask_] = item;

    // release: makes the write visible to the consumer before we advance the index
    write_pos_.store(write + 1, memory_order_release);
}

template <typename T>
bool CircularBuffer<T>::pop_batch(T* out, size_t count) {
    size_t read  = read_pos_.load(memory_order_relaxed);

    // acquire: pairs with the release in push — ensures we see the written data
    size_t write = write_pos_.load(memory_order_acquire);

    if (write - read < count)
        return false;

    for (size_t i = 0; i < count; ++i)
        out[i] = buf_[(read + i) & mask_];

    read_pos_.store(read + count, memory_order_release);
    return true;
}

template <typename T>
size_t CircularBuffer<T>::available() const {
    size_t read  = read_pos_.load(memory_order_relaxed);
    size_t write = write_pos_.load(memory_order_acquire);
    return write - read;
}

template class CircularBuffer<complex<float>>;