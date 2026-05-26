#pragma once

#include "app_state.hpp"
#include <vector>

// forward declare so we don't pull GLFW into every file that includes this
struct GLFWwindow;

class DisplayThread {
public:
    explicit DisplayThread(AppState& state);
    ~DisplayThread();

    // runs on main thread — OpenGL contexts are main-thread-affine
    void run();

private:
    bool init_glfw_and_imgui();
    void shutdown_glfw_and_imgui();

    void copy_state();        // lock psd_mutex, snapshot vectors, unlock
    void render_psd_plot();   // ImPlot line graph + peak hold overlay
    void render_waterfall();  // ImPlot heatmap scrolling display
    void render_controls();   // sliders, dropdowns, buttons — Phase 7

    AppState& state_;

    // local copies so psd_mutex isn't held during rendering
    std::vector<float> local_psd_;
    std::vector<float> local_peak_;
    std::vector<float> local_waterfall_;
    int                local_fft_size_{ 0 };

    GLFWwindow* window_{ nullptr };

    // cursor state for dBm readout
    bool  cursor_locked_{ false };
    float cursor_freq_hz_{ 0.0f };
    float cursor_dbm_{ 0.0f };

    // x-axis frequency array recomputed when fft_size or center_freq changes
    std::vector<float> freq_axis_;
    uint64_t           last_center_freq_{ 0 }; // detect freq changes for axis rebuild
};