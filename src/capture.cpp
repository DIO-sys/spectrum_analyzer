#include "capture.hpp"

#include <libbladeRF.h>
#include <iostream>
#include <fstream>
#include <ctime>
#include <filesystem>

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

    uint64_t current_freq = 0;
    int      current_gain = -1;

    cout << "[capture] streaming\n";

    while (state_.running.load()) {
        // only issue USB control commands when values actually change
        uint64_t new_freq = state_.center_freq_hz.load();
        int      new_gain = state_.gain_db.load();

        if (new_freq != current_freq) {
            status = bladerf_set_frequency(dev_, BLADERF_CHANNEL_RX(0), new_freq);
            if (status != 0) {
                log_error("set_frequency", status);
                state_.running.store(false);
                break;
            }
            current_freq = new_freq;
        }

        if (new_gain != current_gain) {
            status = bladerf_set_gain(dev_, BLADERF_CHANNEL_RX(0), new_gain);
            if (status != 0) {
                log_error("set_gain", status);
                state_.running.store(false);
                break;
            }
            current_gain = new_gain;
        }

        // blocking read — fills raw_buf_ with interleaved int16 I/Q pairs
        status = bladerf_sync_rx(dev_, raw_buf_.data(), TRANSFER_SAMPLES,
                                 nullptr, 5000);
        if (status != 0) {
            log_error("bladerf_sync_rx", status);
            state_.running.store(false);
            break;
        }

        scale_and_push(raw_buf_.data(), TRANSFER_SAMPLES);

        // check if UI requested a save
        if (state_.save_requested.load()) {
            save_iq_file();
            state_.save_requested.store(false);
        }
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
                                 32,               // num_buffers — more buffering
                                 TRANSFER_SAMPLES,
                                 4,                // num_transfers — was 8, reduce USB pressure
                                 5000);            // timeout ms — give more time
    if (status != 0) {
        log_error("sync_config", status);
        return false;
    }

    // hardware DC offset correction — zeros the I and Q DC bias on the mixer
    // this reduces but doesn't fully eliminate the center spike
    status = bladerf_set_correction(dev_, BLADERF_CHANNEL_RX(0), BLADERF_CORR_DCOFF_I, 0);
    if (status != 0) log_error("dcoff_i", status); // non-fatal, continue

    status = bladerf_set_correction(dev_, BLADERF_CHANNEL_RX(0), BLADERF_CORR_DCOFF_Q, 0);
    if (status != 0) log_error("dcoff_q", status); // non-fatal, continue

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

void CaptureThread::save_iq_file() {
    // make sure data/ directory exists
    filesystem::create_directories("../data");

    // timestamp filename: data/iq_YYYYMMDD_HHMMSS_<freq>MHz.bin
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));

    uint64_t freq_hz  = state_.center_freq_hz.load();
    uint64_t freq_mhz = freq_hz / 1'000'000;

    string path = string("../data/iq_") + ts + "_" +
                  to_string(freq_mhz) + "MHz.bin";

    ofstream f(path, ios::binary);
    if (!f) {
        cerr << "[capture] failed to open " << path << " for writing\n";
        return;
    }

    // capture 1 second = SAMPLE_RATE_HZ samples
    // stream directly to disk in TRANSFER_SAMPLES chunks
    unsigned int total   = SAMPLE_RATE_HZ;
    unsigned int written = 0;

    cout << "[capture] saving " << total << " samples to " << path << "\n";

    while (written < total && state_.running.load()) {
        int status = bladerf_sync_rx(dev_, raw_buf_.data(), TRANSFER_SAMPLES,
                                     nullptr, 5000);
        if (status != 0) {
            log_error("save bladerf_sync_rx", status);
            break;
        }
        // write raw int16 interleaved IQ — NumPy reads this as np.int16
        f.write(reinterpret_cast<const char*>(raw_buf_.data()),
                TRANSFER_SAMPLES * 2 * sizeof(int16_t));
        written += TRANSFER_SAMPLES;
    }

    f.close();
    cout << "[capture] saved " << written << " samples to " << path << "\n";
}