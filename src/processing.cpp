#include "processing.hpp"
#include <iostream>
#include <cmath>
#include <thread>
#include <chrono>

using namespace std;

static constexpr float PI = 3.14159265358979323846f;

ProcessingThread::ProcessingThread(CircularBuffer<complex<float>>& buf, AppState& state)
    : buf_(buf), state_(state) {}

ProcessingThread::~ProcessingThread() {
    if (plan_)     fftwf_destroy_plan(plan_);
    if (fftw_in_)  fftwf_free(fftw_in_);
    if (fftw_out_) fftwf_free(fftw_out_);
}

// public

void ProcessingThread::run() {
    cout << "[processing] started\n";

    while (state_.running.load()) {
        int requested_size = state_.fft_size.load();

        // rebuild plan whenever fft_size changes (user switched dropdown)
        if (requested_size != fft_size_)
            rebuild_plan(requested_size);

        // wait until a full batch is available
        if (buf_.available() < static_cast<size_t>(fft_size_)) {
            this_thread::sleep_for(chrono::microseconds(100));
            continue;
        }

        if (!buf_.pop_batch(batch_.data(), static_cast<size_t>(fft_size_)))
            continue;

        apply_hann_window();

        // copy windowed batch into fftw input buffer
        for (int k = 0; k < fft_size_; ++k) {
            fftw_in_[k][0] = batch_[k].real();
            fftw_in_[k][1] = batch_[k].imag();
        }
        //transform from fft in to fft out
        fftwf_execute(plan_);
        //put those new samples into the welch accumulate
        accumulate_frame();  // |X[k]|^2 into welch_accum_

        ++frame_count_;
            //augment frames until desired #
            //when reached, send to display thread
        if (frame_count_ >= WELCH_FRAMES) {
            flush_welch();   // average, convert to dBm, push to AppState
            update_peak();
            frame_count_ = 0;
        }
    }

    cout << "[processing] exiting\n";
}

//private 
void ProcessingThread::rebuild_plan(int new_fft_size) {
    // free old resources
    if (plan_)     fftwf_destroy_plan(plan_);
    if (fftw_in_)  fftwf_free(fftw_in_);
    if (fftw_out_) fftwf_free(fftw_out_);

    fft_size_ = new_fft_size;

    // fftw_malloc gives aligned memory — FFTW uses SIMD internally
    fftw_in_  = reinterpret_cast<fftwf_complex*>(
                    fftwf_malloc(sizeof(fftwf_complex) * fft_size_));
    fftw_out_ = reinterpret_cast<fftwf_complex*>(
                    fftwf_malloc(sizeof(fftwf_complex) * fft_size_));

    if (!fftw_in_ || !fftw_out_) {
        cerr << "[processing] fftwf_malloc failed\n";
        state_.running.store(false);
        return;
    }

    // MEASURE lets FFTW pick the fastest algorithm for this size
    plan_ = fftwf_plan_dft_1d(fft_size_, fftw_in_, fftw_out_,
                               FFTW_FORWARD, FFTW_MEASURE);
    if (!plan_) {
        cerr << "[processing] fftwf_plan_dft_1d failed\n";
        state_.running.store(false);
        return;
    }

    // resize working buffers
    batch_.resize(fft_size_);
    welch_accum_.assign(fft_size_, 0.0f);
    psd_result_.resize(fft_size_);
    frame_count_ = 0;

    // precompute Hann window coefficients for this fft_size
    // w[n] = 0.5 * (1 - cos(2π*n / (N-1)))
    hann_window_.resize(fft_size_);
    for (int n = 0; n < fft_size_; ++n)
        hann_window_[n] = 0.5f * (1.0f - cosf(2.0f * PI * n / (fft_size_ - 1)));

    cout << "[processing] plan rebuilt for fft_size=" << fft_size_ << "\n";
}

void ProcessingThread::apply_hann_window() {
    // multiply each complex sample by the real-valued window coefficient
    // this tapers the edges of the batch to zero, reducing spectral leakage
    for (int n = 0; n < fft_size_; ++n)
        batch_[n] *= hann_window_[n];
}

void ProcessingThread::accumulate_frame() {
    // add |X[k]|^2 from this FFT frame into the running Welch sum
    for (int k = 0; k < fft_size_; ++k) {
        float re = fftw_out_[k][0];
        float im = fftw_out_[k][1];
        welch_accum_[k] += re * re + im * im;
    }
}

void ProcessingThread::flush_welch() {
    // TODO phase 5 — average welch_accum_, convert to dBm, push to AppState
}

void ProcessingThread::update_peak() {
    // TODO phase 6
}