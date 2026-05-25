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

//public 

void CaptureThread::run() {
    cout << "[capture] starting\n";

    if (!open_device() || !configure_device()) {
        state_.running.store(false);
        return;
    }

    int status = bladerf_start_rx(dev_, BLADERF_MODULE_RX, nullptr, nullptr);
    if (status != 0) {
        log_error("start_rx", status);
        state_.running.store(false);
        return;
    }

    cout << "[capture] streaming\n";

    while (state_.running.load()) {
        // check if center freq or gain changed since last iteration
        uint64_t freq = state_.center_freq_hz.load();
        int      gain = state_.gain_db.load();

        status = bladerf_set_frequency(dev_, BLADERF_MODULE_RX, freq);
        if (status != 0) {
            log_error("set_frequency", status);
            state_.running.store(false);
            break;
        }

        status = bladerf_set_gain(dev_, BLADERF_MODULE_RX, gain);
        if (status != 0) {
            log_error("set_gain", status);
            state_.running.store(false);
            break;
        }

        // blocking read — fills raw_buf_ with interleaved int16 I/Q pairs
        status = bladerf_rx(dev_, BLADERF_FORMAT_SC16_Q11,
                            raw_buf_.data(), TRANSFER_SAMPLES, nullptr);
        if (status < 0) {
            log_error("bladerf_rx", status);
            state_.running.store(false);
            break;
        }

        scale_and_push(raw_buf_.data(), TRANSFER_SAMPLES);
    }

    bladerf_stop_rx(dev_, BLADERF_MODULE_RX, nullptr);
    cout << "[capture] exiting\n";
}

//private 

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

    // sample rate
    uint32_t actual_rate = 0;
    status = bladerf_set_sample_rate(dev_, BLADERF_MODULE_RX,
                                     SAMPLE_RATE_HZ, &actual_rate);
    if (status != 0) {
        log_error("set_sample_rate", status);
        return false;
    }
    cout << "[capture] sample rate: " << actual_rate << " sps\n";

    // bandwidth
    uint32_t actual_bw = 0;
    status = bladerf_set_bandwidth(dev_, BLADERF_MODULE_RX,
                                   BANDWIDTH_HZ, &actual_bw);
    if (status != 0) {
        log_error("set_bandwidth", status);
        return false;
    }

    // initial center frequency from AppState
    status = bladerf_set_frequency(dev_, BLADERF_MODULE_RX,
                                   state_.center_freq_hz.load());
    if (status != 0) {
        log_error("set_frequency (init)", status);
        return false;
    }

    // initial gain
    status = bladerf_set_gain(dev_, BLADERF_MODULE_RX, state_.gain_db.load());
    if (status != 0) {
        log_error("set_gain (init)", status);
        return false;
    }

    // sync config — tells the driver our transfer size and buffer count
    // 16 buffers × 4096 samples gives ~1.6ms of USB-level buffering
    status = bladerf_sync_config(dev_, BLADERF_MODULE_RX,
                                 BLADERF_FORMAT_SC16_Q11,
                                 16,              // num_buffers
                                 TRANSFER_SAMPLES,
                                 8,               // num_transfers
                                 3500);           // timeout ms
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
    // BladeRF SC16_Q11 format: interleaved int16 pairs, range [-2048, 2047]
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