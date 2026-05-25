#pragma once

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include <complex>
#include <vector>
#include <fftw3.h>

class ProcessingThread {
public:
    ProcessingThread(CircularBuffer<std::complex<float>>& buf, AppState& state);
    ~ProcessingThread();

    void run();

private:
    void rebuild_plan(int new_fft_size);
    void apply_hann_window();
    void accumulate_frame();
    void flush_welch();
    void update_peak();

    CircularBuffer<std::complex<float>>& buf_;
    AppState&                            state_;

    int            fft_size_{ 0 };
    //time domain samples before fft
    fftwf_complex* fftw_in_ { nullptr };
    //frequency domain samples after fft
    fftwf_complex* fftw_out_{ nullptr };
    //optimized fft configuration
    fftwf_plan     plan_    { nullptr };

    // batch popped from the ring buffer each iteration
    std::vector<std::complex<float>> batch_;

    std::vector<float> hann_window_;
    std::vector<float> welch_accum_;
    std::vector<float> psd_result_;

    static constexpr int WELCH_FRAMES = 8;
    int frame_count_{ 0 };
};