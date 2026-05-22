#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

// shared state passed by reference to all three threads
struct AppState {

    // config — UI writes, capture + processing read
    std::atomic<uint64_t> center_freq_hz  { 915'000'000ULL };
    std::atomic<int>      fft_size        { 1024 };
    std::atomic<int>      gain_db         { 30 };
    std::atomic<float>    cal_offset_db   { 0.0f };
    std::atomic<bool>     peak_hold_on    { false };
    std::atomic<bool>     peak_hold_reset { false }; // one-shot, cleared by processing

    std::atomic<bool>     running         { true };

    // psd output — processing writes, display reads
    std::mutex            psd_mutex;
    std::vector<float>    psd_dbm;
    std::vector<float>    peak_dbm;

    // flat row-major waterfall: waterfall[row * fft_size + col], row 0 = newest
    static constexpr int  WATERFALL_ROWS = 512;
    std::vector<float>    waterfall;

    AppState()                           = default;
    AppState(const AppState&)            = delete;
    AppState& operator=(const AppState&) = delete;
};