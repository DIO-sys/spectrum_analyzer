#include "capture.hpp"
#include <libbladeRF.h>
#include <iostream>

using namespace std;

CaptureThread::CaptureThread(CircularBuffer<complex<float>>& buf, AppState& state)
    : buf_(buf)
    , state_(state)
    , raw_buf_(TRANSFER_SAMPLES * 2) // 2 int16 per sample (I + Q)
{}

CaptureThread::~CaptureThread() {
    close_device();
}

// public

void CaptureThread::run() {
    cout << "[capture] starting\n";

    if (!open_device() || !configure_device()) {
        state_.running.store(false);
        return;
    }

    // enable RX front end after sync_config, before sync_rx
    int status = bladerf_enable_module(dev_, BLADERF_CHANNEL_RX(0), true);
    if (status != 0) {
        log_error("enable_module RX", status);
        state_.running.store(false);
        return;
    }

    cout << "[capture] streaming\n";

    while (state_.running.load()) {
        // pick up freq/gain changes from UI
        uint64_t freq = state_.center_freq_hz.load();
        int      gain = state_.gain_db.load();

        status = bladerf_set_frequency(dev_, BLADERF_CHANNEL_RX(0), freq);
        if (status != 0) {
            log_error("set_frequency", status);
            state_.running.store(false);
            break;
        }

        status = bladerf_set_gain(dev_, BLADERF_CHANNEL_RX(0), gain);
        if (status != 0) {
            log_error("set_gain", status);
            state_.running.store(false);
            break;
        }

        // blocking read — fills raw_buf_ with interleaved int16 I/Q pairs
        status = bladerf_sync_rx(dev_, raw_buf_.data(), TRANSFER_SAMPLES,
                                 nullptr, 3500);
        if (status != 0) {
            log_error("bladerf_sync_rx", status);
            state_.running.store(false);
            break;
        }

        scale_and_push(raw_buf_.data(), TRANSFER_SAMPLES);
    }

    bladerf_enable_module(dev_, BLADERF_CHANNEL_RX(0), false);
    cout << "[capture] exiting\n";
}

// private

bool CaptureThread::open_device() {
    int status = bladerf_open(&dev_, nullptr); // nullptr = first device found
    if (status != 0) {
        log_error("bladerf_open", status);
        return false;
    }
    cout << "[capture] BladeRF opened\n";
    return true;
}

bool CaptureThread::configure_device() {
    int status;

    uint32_t actual_rate = 0;
    status = bladerf_set_sample_rate(dev_, BLADERF_CHANNEL_RX(0),
                                     SAMPLE_RATE_HZ, &actual_rate);
    if (status != 0) {
        log_error("set_sample_rate", status);
        return false;
    }
    cout << "[capture] sample rate: " << actual_rate << " sps\n";

    uint32_t actual_bw = 0;
    status = bladerf_set_bandwidth(dev_, BLADERF_CHANNEL_RX(0),
                                   BANDWIDTH_HZ, &actual_bw);
    if (status != 0) {
        log_error("set_bandwidth", status);
        return false;
    }

    status = bladerf_set_frequency(dev_, BLADERF_CHANNEL_RX(0),
                                   state_.center_freq_hz.load());
    if (status != 0) {
        log_error("set_frequency (init)", status);
        return false;
    }

    status = bladerf_set_gain(dev_, BLADERF_CHANNEL_RX(0), state_.gain_db.load());
    if (status != 0) {
        log_error("set_gain (init)", status);
        return false;
    }

    // BLADERF_RX_X1 = single channel layout (SISO)
    // must be called before bladerf_enable_module
    status = bladerf_sync_config(dev_, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11,
                                 16,               // num_buffers
                                 TRANSFER_SAMPLES,
                                 8,                // num_transfers
                                 3500);            // timeout ms
    if (status != 0) {
        log_error("sync_config", status);
        return false;
    }

    cout << "[capture] device configured\n";
    return true;
}

void CaptureThread::close_device() {
    if (dev_) {
        bladerf_close(dev_);
        dev_ = nullptr;
        cout << "[capture] device closed\n";
    }
}

void CaptureThread::scale_and_push(const int16_t* raw, unsigned int num_samples) {
    // SC16_Q11: interleaved int16 pairs, range [-2048, 2047]
    // divide by 2048.0f to normalize to (-1.0, +1.0)
    for (unsigned int i = 0; i < num_samples; ++i) {
        float i_val = raw[i * 2]     / 2048.0f;
        float q_val = raw[i * 2 + 1] / 2048.0f;
        buf_.push(complex<float>(i_val, q_val));
    }
}

void CaptureThread::log_error(const char* context, int status) {
    cerr << "[capture] " << context
         << " failed: " << bladerf_strerror(status) << "\n";
}