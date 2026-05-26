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

        // transform from fft_in to fft_out
        fftwf_execute(plan_);

        // put those new samples into the welch accumulator
        accumulate_frame();

        ++frame_count_;
        // augment frames until desired #
        // when reached, send to display thread
        if (frame_count_ >= WELCH_FRAMES) {
            average_welch();
            convert_to_dbm();
            publish_psd();
            update_peak();
            frame_count_ = 0;
        }
    }

    cout << "[processing] exiting\n";
}

// private

void ProcessingThread::rebuild_plan(int new_fft_size) {
    if (plan_)     fftwf_destroy_plan(plan_);
    if (fftw_in_)  fftwf_free(fftw_in_);
    if (fftw_out_) fftwf_free(fftw_out_);

    fft_size_ = new_fft_size;

    // fftwf_malloc gives aligned memory — FFTW uses SIMD internally
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
    // tapers edges of batch to zero, reducing spectral leakage
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

void ProcessingThread::average_welch() {
    float frames = static_cast<float>(WELCH_FRAMES);
    for (int k = 0; k < fft_size_; ++k)
        welch_accum_[k] /= frames;
}

void ProcessingThread::convert_to_dbm() {
    float cal = state_.cal_offset_db.load();
    float n2  = static_cast<float>(fft_size_) * static_cast<float>(fft_size_);

    for (int k = 0; k < fft_size_; ++k) {
        // power_dBm = 10 * log10(|X[k]|^2 / N^2) + calibration_offset
        // clamp to avoid log10(0) = -inf
        float power = welch_accum_[k] / n2;
        psd_result_[k] = 10.0f * log10f(power > 1e-20f ? power : 1e-20f) + cal;
    }

    // fftshift: swap halves so DC is centered, negative freqs on the left
    int half = fft_size_ / 2;
    vector<float> shifted(fft_size_);
    for (int k = 0; k < half; ++k) {
        shifted[k]        = psd_result_[k + half];
        shifted[k + half] = psd_result_[k];
    }
    psd_result_ = shifted;

    fill(welch_accum_.begin(), welch_accum_.end(), 0.0f);
}

void ProcessingThread::publish_psd() {
    lock_guard<mutex> lock(state_.psd_mutex);

    state_.psd_dbm = psd_result_;

    // resize waterfall if fft_size changed
    int total = AppState::WATERFALL_ROWS * fft_size_;
    if (static_cast<int>(state_.waterfall.size()) != total)
        state_.waterfall.assign(total, -120.0f);

    // shift rows down, insert new row at top (row 0 = newest)
    move_backward(state_.waterfall.begin(),
                  state_.waterfall.end() - fft_size_,
                  state_.waterfall.end());
    copy(psd_result_.begin(), psd_result_.end(), state_.waterfall.begin());
}

void ProcessingThread::update_peak() {
    if (!state_.peak_hold_on.load()) return;

    // one-shot reset
    // basically on reset, for each frequency bin, set it to -200 dBm which is below the noise floor.
    if (state_.peak_hold_reset.load()) {
        lock_guard<mutex> lock(state_.psd_mutex);
        fill(state_.peak_dbm.begin(), state_.peak_dbm.end(), -200.0f);
        // turn off the reset flag so that it doesn't keep resetting every update.
        state_.peak_hold_reset.store(false);
        return;
    }

    lock_guard<mutex> lock(state_.psd_mutex);
    // if the peak hold array size doesn't match the current PSD size (because user changed FFT size)
    // resize it and fill with -200 dBm (no peaks).
    if (state_.peak_dbm.size() != state_.psd_dbm.size())
        state_.peak_dbm.assign(state_.psd_dbm.size(), -200.0f);

    // go through each frequency and compare current and "highest"
    for (size_t k = 0; k < state_.psd_dbm.size(); ++k) {
        if (state_.psd_dbm[k] > state_.peak_dbm[k])
            state_.peak_dbm[k] = state_.psd_dbm[k];
    }
}