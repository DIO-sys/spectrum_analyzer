#pragma once

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include <complex>
#include <cstdint>
#include <vector>

struct bladerf;

class CaptureThread {
public:
    CaptureThread(CircularBuffer<std::complex<float>>& buf, AppState& state);
    ~CaptureThread();

    void run();

private:
    bool open_device();
    bool configure_device();
    void close_device();
    void scale_and_push(const int16_t* raw, unsigned int num_samples);
    void log_error(const char* context, int status);
    void save_iq_file();

    CircularBuffer<std::complex<float>>& buf_;
    AppState&                            state_;

    struct bladerf* dev_{ nullptr };

    // raw int16 staging buffer: 2 int16 per IQ pair (I then Q)
    std::vector<int16_t> raw_buf_;

    //these integers are so verbose for best practices.
    //they're defined in the header because they're needed in the .cpp file, but they don't need to be mutable or visible outside of this class, so they're private and static. 
    //static purely so that constexpr doesnt return an error, constexpr to allow compile-time optimization, and unsigned because they can't be negative.
    // ~0.1ms of samples per USB transfer
    static constexpr unsigned int TRANSFER_SAMPLES = 4096;

    // sample rate fixed at 40 MSPS — BladeRF max
    static constexpr unsigned int SAMPLE_RATE_HZ = 40'000'000;

    // 40 MHz bandwidth of recorded spectrum to match sample rate
    // we match sample rate and bandwidth from nyquist theorem 
    static constexpr unsigned int BANDWIDTH_HZ = 40'000'000;
};