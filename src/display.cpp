#include "display.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>
#include <iostream>
#include <algorithm>
#include <cmath>

using namespace std;

static constexpr int   WINDOW_W    = 1400;
static constexpr int   WINDOW_H    = 900;
static constexpr float SAMPLE_RATE = 40.0e6f;
static constexpr float DBM_MIN     = -120.0f;
static constexpr float DBM_MAX     =   10.0f;

DisplayThread::DisplayThread(AppState& state)
    : state_(state) {}

DisplayThread::~DisplayThread() {
    shutdown_glfw_and_imgui();
}

void DisplayThread::run() {
    if (!init_glfw_and_imgui()) {
        cerr << "[display] init failed\n";
        state_.running.store(false);
        return;
    }

    cout << "[display] render loop started\n";

    while (!glfwWindowShouldClose(window_) && state_.running.load()) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        copy_state();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove       |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        render_psd_plot();
        ImGui::Separator();
        render_waterfall();
        ImGui::Separator();
        render_controls();

        ImGui::End();

        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(window_, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }

    state_.running.store(false);
    cout << "[display] render loop exiting\n";
}

bool DisplayThread::init_glfw_and_imgui() {
    if (!glfwInit()) {
        cerr << "[display] glfwInit failed\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = glfwCreateWindow(WINDOW_W, WINDOW_H, "Spectrum Analyzer", nullptr, nullptr);
    if (!window_) {
        cerr << "[display] glfwCreateWindow failed\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().FrameRounding  = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    cout << "[display] GLFW + ImGui + ImPlot initialized\n";
    return true;
}

void DisplayThread::shutdown_glfw_and_imgui() {
    if (!window_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
}

void DisplayThread::copy_state() {
    lock_guard<mutex> lock(state_.psd_mutex);
    local_fft_size_  = static_cast<int>(state_.psd_dbm.size());
    local_psd_       = state_.psd_dbm;
    local_peak_      = state_.peak_dbm;
    local_waterfall_ = state_.waterfall;
}

void DisplayThread::render_psd_plot() {
    if (local_psd_.empty()) return;

    int    n          = local_fft_size_;
    double center     = static_cast<double>(state_.center_freq_hz.load()) / 1e6;
    double span       = SAMPLE_RATE / 1e6;
    double freq_start = center - span / 2.0;
    double freq_stop  = center + span / 2.0;

    // rebuild freq axis whenever center freq or fft size changes
    uint64_t current_center = state_.center_freq_hz.load();
    if (static_cast<int>(freq_axis_.size()) != n || current_center != last_center_freq_) {
        last_center_freq_ = current_center;
        freq_axis_.resize(n);
        for (int k = 0; k < n; ++k)
            freq_axis_[k] = static_cast<float>(freq_start + k * (span / n));
    }

    ImVec2 plot_size = { ImGui::GetContentRegionAvail().x,
                         ImGui::GetContentRegionAvail().y * 0.42f };

    ImPlot::SetNextAxesLimits(freq_start, freq_stop, DBM_MIN, DBM_MAX, ImPlotCond_Always);

    if (ImPlot::BeginPlot("PSD", plot_size, ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes("Frequency (MHz)", "Power (dBm)");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_start, freq_stop, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, DBM_MIN, DBM_MAX, ImPlotCond_Always);

        ImPlotSpec psd_spec;
        psd_spec.LineColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        ImPlot::PlotLine("PSD", freq_axis_.data(), local_psd_.data(), n, psd_spec);

        if (state_.peak_hold_on.load() && !local_peak_.empty()) {
            ImPlotSpec peak_spec;
            peak_spec.LineColor = ImVec4(1.0f, 0.4f, 0.1f, 1.0f);
            ImPlot::PlotLine("Peak", freq_axis_.data(), local_peak_.data(), n, peak_spec);
        }

        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();

            if (!cursor_locked_) {
                cursor_freq_hz_ = static_cast<float>(mouse.x * 1e6);
                int bin = static_cast<int>((mouse.x - freq_start) / span * n);
                bin = max(0, min(n - 1, bin));
                cursor_dbm_ = local_psd_[bin];
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                cursor_locked_ = !cursor_locked_;

            ImGui::BeginTooltip();
            ImGui::Text("%.4f MHz", cursor_freq_hz_ / 1e6f);
            ImGui::Text("%.1f dBm", cursor_dbm_);
            if (cursor_locked_) ImGui::Text("[locked]");
            ImGui::EndTooltip();

            ImPlotSpec cur_spec;
            cur_spec.LineColor = ImVec4(1.0f, 1.0f, 0.0f, 0.8f);
            float cx    = cursor_freq_hz_ / 1e6f;
            float ys[2] = { DBM_MIN, DBM_MAX };
            float xs[2] = { cx, cx };
            ImPlot::PlotLine("##cursor", xs, ys, 2, cur_spec);
        }

        ImPlot::EndPlot();
    }
}

void DisplayThread::render_waterfall() {
    if (local_waterfall_.empty() || local_fft_size_ == 0) return;

    double center     = static_cast<double>(state_.center_freq_hz.load()) / 1e6;
    double span       = SAMPLE_RATE / 1e6;
    double freq_start = center - span / 2.0;
    double freq_stop  = center + span / 2.0;

    ImVec2 plot_size = { ImGui::GetContentRegionAvail().x, 400.0f };

    // force X axis to track center freq, hide Y axis ticks (time is implicit)
    ImPlot::SetNextAxesLimits(freq_start, freq_stop, 0, AppState::WATERFALL_ROWS, ImPlotCond_Always);

    if (ImPlot::BeginPlot("Waterfall", plot_size,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes("Frequency (MHz)", nullptr,
                          ImPlotAxisFlags_None,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_start, freq_stop, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, AppState::WATERFALL_ROWS, ImPlotCond_Always);

        ImPlot::PushColormap(ImPlotColormap_Jet);
        ImPlot::PlotHeatmap("##wf",
                            local_waterfall_.data(),
                            AppState::WATERFALL_ROWS,
                            local_fft_size_,
                            DBM_MIN, DBM_MAX,
                            nullptr,
                            ImPlotPoint(freq_start, 0),
                            ImPlotPoint(freq_stop, static_cast<double>(AppState::WATERFALL_ROWS)));
        ImPlot::PopColormap();

        ImPlot::EndPlot();
    }
}

void DisplayThread::render_controls() {
    // row 1: freq slider + direct input, gain, fft size
    ImGui::SetNextItemWidth(260.0f);
    float freq_mhz = static_cast<float>(state_.center_freq_hz.load()) / 1e6f;
    if (ImGui::SliderFloat("##freqslider", &freq_mhz, 0.0f, 3800.0f, "%.1f MHz")) {
        state_.center_freq_hz.store(static_cast<uint64_t>(freq_mhz * 1e6f));
        freq_axis_.clear();
        state_.peak_hold_reset.store(true);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    // direct entry field — type exact MHz value and press Enter
    static float freq_input = freq_mhz;
    if (ImGui::InputFloat("MHz##input", &freq_input, 0.0f, 0.0f, "%.3f",
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
        freq_input = max(0.0f, min(3800.0f, freq_input)); // clamp to valid range
        state_.center_freq_hz.store(static_cast<uint64_t>(freq_input * 1e6f));
        freq_axis_.clear();
        state_.peak_hold_reset.store(true);
    }
    // keep input field in sync when slider moves
    if (ImGui::IsItemDeactivated())
        freq_input = static_cast<float>(state_.center_freq_hz.load()) / 1e6f;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    int gain = state_.gain_db.load();
    if (ImGui::SliderInt("Gain (dB)", &gain, 0, 60))
        state_.gain_db.store(gain);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    static const char* fft_options[] = { "1024", "2048", "4096" };
    static const int   fft_values[]  = { 1024, 2048, 4096 };
    int current_fft = state_.fft_size.load();
    int current_idx = 0;
    for (int i = 0; i < 3; ++i)
        if (fft_values[i] == current_fft) current_idx = i;
    if (ImGui::Combo("FFT Size", &current_idx, fft_options, 3))
        state_.fft_size.store(fft_values[current_idx]);

    // row 2: cal offset, peak hold, cursor readout
    ImGui::SetNextItemWidth(150.0f);
    float cal = state_.cal_offset_db.load();
    if (ImGui::InputFloat("Cal Offset (dB)", &cal, 0.5f, 1.0f, "%.1f"))
        state_.cal_offset_db.store(cal);

    ImGui::SameLine();
    bool peak_on = state_.peak_hold_on.load();
    if (ImGui::Checkbox("Peak Hold", &peak_on))
        state_.peak_hold_on.store(peak_on);

    ImGui::SameLine();
    if (ImGui::Button("Reset Peak"))
        state_.peak_hold_reset.store(true);

    ImGui::SameLine();
    if (cursor_locked_) {
        ImGui::TextColored({1.0f, 1.0f, 0.0f, 1.0f},
                           "| Cursor: %.4f MHz  %.1f dBm [locked]",
                           cursor_freq_hz_ / 1e6f, cursor_dbm_);
        ImGui::SameLine();
        if (ImGui::Button("Unlock"))
            cursor_locked_ = false;
    } else {
        ImGui::TextDisabled("| Click PSD to lock cursor");
    }

    // status bar
    ImGui::Separator();
    ImGui::TextDisabled("Status: %s  |  %.0f MSPS  |  Buffer: %dk  |  Frames: %d",
                        state_.running.load() ? "Running" : "Stopped",
                        SAMPLE_RATE / 1e6f,
                        static_cast<int>(1 << 18) / 1024,
                        state_.total_frames.load());
}