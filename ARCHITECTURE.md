# Architecture — Spectrum Analyzer

**Author:** Timo Kamgang  
**Hardware:** Nuand BladeRF 2.0 Micro xA4  
**Stack:** C++17 · libbladeRF · FFTW3 · Dear ImGui · ImPlot · OpenGL 3.3

---

## Overview

A real-time RF spectrum analyzer streaming 40 million IQ samples per second from a software-defined radio into a live power spectral density display. The system is built around a three-thread producer/consumer pipeline with a lock-free circular buffer at its core, a Welch-averaged FFTW3 DSP chain optimized for a clean noise floor, and an immediate-mode GPU-rendered display via Dear ImGui and ImPlot.

The primary design goal was interference detection — specifically characterizing signal activity in the 5.9 GHz C-V2X band. Every architectural decision downstream of IQ normalization is oriented toward that goal: maximizing noise floor stability over frequency resolution precision.

---

## The Inflection Point

Everything before normalization is hardware driver work. Everything after it is architecture.

```
BladeRF → raw int16 SC16_Q11 → ÷ 2048.0f → complex<float>
                                              ↑
                              architecture decisions begin here
```

The BladeRF delivers signed 16-bit interleaved IQ pairs in SC16_Q11 format — range −2048 to +2047. Dividing by `2048.0f` produces normalized complex floats in (−1.0, +1.0). Once samples are in `complex<float>`, they are mathematically first-class signal data. From this point forward every decision like the windowing function, averaging method, FFT size, buffer geometry all is a DSP and systems design choice with measurable engineering tradeoffs.

---

## Threading Model

Three threads, single ownership, no shared mutable state except through two well-defined interfaces.

```
main()
├── capture_thread    → CircularBuffer (producer side)
├── processing_thread → CircularBuffer (consumer side) + AppState (writer)
└── display thread    → AppState (reader) — runs on main thread (OpenGL affinity)
```

**Capture thread** owns the BladeRF device handle. It reads raw IQ from hardware, normalizes, and pushes to the ring buffer. It has no knowledge of the FFT or display.

**Processing thread** pulls batches from the ring buffer, runs the DSP chain, and writes dBm values to `AppState`. It has no knowledge of the hardware or rendering.

**Display thread** runs on `main()` because OpenGL contexts are main-thread-affine on Linux and macOS. It reads from `AppState` under a mutex, copies to local buffers, then renders.

**Shutdown** is coordinated by a single `atomic<bool> running` in `AppState`. When the display window closes, `running` is set to false. Both background threads exit cleanly on the next iteration. `main()` joins them before returning.

---

## Lock-Free Circular Buffer

The ring buffer is the highest-frequency interface in the system — the capture thread pushes at 40 MSPS, roughly 9,765 times per second at 4096 samples per transfer. A mutex here would stall the capture loop and cause USB transfer timeouts. The buffer is implemented as a single-producer/single-consumer (SPSC) lock-free ring using `std::atomic` with explicit memory ordering.

```cpp
alignas(64) std::atomic<size_t> write_pos_{ 0 };
alignas(64) std::atomic<size_t> read_pos_ { 0 };
```

**Cache line padding.** Both atomic indices are `alignas(64)` — placed on separate 64-byte cache lines. Without padding, the two indices share a cache line. When the capture thread writes `write_pos_` and the processing thread writes `read_pos_` simultaneously, the CPU must bounce that cache line between cores (false sharing), adding ~100ns of latency per operation.

**Memory ordering.** `push()` uses `memory_order_release` on the write index store. `pop_batch()` uses `memory_order_acquire` on the read of the write index. The acquire/release pair guarantees that all sample data written before the index advance is visible to the consumer before it reads any samples.

**Power-of-two capacity.** The buffer capacity is fixed at 2¹⁸ = 262,144 samples (~6.5ms at 40 MSPS). Power-of-two enables wrap-around via bitmask (`pos & mask_`) instead of modulo — a single AND instruction versus a division, critical in a loop executing 40M times per second.

**Overwrite behavior.** When the buffer is full, `push()` overwrites the oldest slot silently. For a real-time spectrum display this is correct: stale samples are less useful than fresh ones.

---

## DSP Pipeline

### Hann windowing — choosing noise floor over resolution

The FFT assumes its input is periodic — it wraps the end of each batch back to the beginning. Real-world signal batches do not start and end at the same amplitude. The discontinuity at the wrap point is interpreted as high-frequency energy — spectral leakage — which raises the noise floor across the entire spectrum. The Hann window tapers both ends of each batch to zero, eliminating the discontinuity.

```
w[n] = 0.5 * (1 − cos(2πn / (N−1)))
```

**The tradeoff.** Windowing slightly widens the main lobe of any signal peak — a sharp tone appears as a narrow hill rather than a single bin spike. This trades frequency precision for noise floor stability. For interference detection that tradeoff is correct: a cleaner floor makes weak interference events visible above background.

**Alternatives considered:**

| Window | Side lobe | Verdict |
|--------|-----------|---------|
| Rectangular | −13 dB | Maximum leakage — ruled out |
| Hann | −32 dB | Chosen — good rejection, standard |
| Blackman | −58 dB | Better rejection, wider lobe — overkill |

### FFTW3 — plan-based SIMD optimization

`fftwf_plan_dft_1d()` with `FFTW_MEASURE` benchmarks multiple FFT algorithms at startup and selects the fastest for the current CPU and transform size. On a machine with AVX2 the selected plan uses vectorized butterfly operations. The plan is cached and reused — `fftwf_execute()` runs in constant time after the first call. `fftwf_malloc()` returns SIMD-aligned memory so FFTW can fully vectorize. The plan is rebuilt whenever the user changes FFT size.

### Welch's method — 8-frame averaging

A single FFT frame is noisy. Thermal noise causes each frequency bin to fluctuate independently between frames. Welch's method averages magnitude-squared output across 8 frames:

```
welch_accum[k] += |X[k]|²     (×8 frames)
avg[k] = welch_accum[k] / 8
```

Averaging 8 frames reduces noise variance by √8 ≈ 2.8×. The noise floor becomes flat and stable. Weak interference events that would be masked by single-frame noise spikes become consistently visible above the floor.

**The tradeoff.** The display updates once per 8 frames. At 40 MSPS with FFT size 1024 the update rate is ~4,900 per second — imperceptible as lag. At 4096 it drops to ~1,200 per second, still well above human perception.

**Alternatives considered:**

| Method | Noise reduction | Verdict |
|--------|----------------|---------|
| Single FFT | None | Too noisy — ruled out |
| Bartlett | Similar | No windowing — leakage remains |
| Welch | √N reduction | Chosen |
| Exponential averaging | Continuous | Viable alternative — more responsive, tunable α |

### dBm conversion

```
power[k] = avg[k] / N²
psd_dbm[k] = 10 * log10(power[k]) + cal_offset
```

Division by N² normalizes output so readings are independent of FFT size. `fftshift` swaps the two halves of the output so DC lands at the center of the display. The DC bin is replaced by the average of its neighbors to suppress the hardware offset artifact inherent to direct conversion receivers.

---

## Display and User Controls

The display thread copies `psd_dbm` and `waterfall` from `AppState` at the start of each frame, then renders entirely from local buffers. The mutex is never held during draw calls.

All user controls write directly to `AppState` atomics and take effect on the next processing or capture loop iteration — typically within 0.1ms.

| Control | What it does |
|---------|-------------|
| Center frequency slider + input | Tunes 50–6000 MHz. Updates `center_freq_hz` atomic. Capture thread picks up on next iteration. Peak hold auto-clears on tune. |
| Gain slider | Sets BladeRF amplification 0–60 dB. Issued as a USB control command only when the value changes — not every loop. |
| FFT size dropdown | 1024 / 2048 / 4096. Triggers `rebuild_plan()` in the processing thread — reallocates FFTW buffers and recomputes Hann coefficients. |
| Calibration offset | Shifts all dBm readings uniformly. Applied in the dBm formula each Welch flush. |
| Peak hold toggle | Enables an orange overlay trace that retains the maximum dBm seen at each bin. |
| Peak hold reset | One-shot atomic flag cleared by the processing thread after wiping the peak buffer. |
| Cursor readout | Hover on PSD plot shows frequency and dBm at the cursor. Click locks the readout to that frequency. |
| Save IQ | Captures 1 second of raw int16 IQ to a timestamped `.bin` file in `data/`. Used as input to the Python analysis scripts. |

The waterfall is a flat row-major `vector<float>` of `WATERFALL_ROWS × fft_size`. Row 0 is always the newest frame. `ImPlot::PlotHeatmap()` accepts this layout directly.

---

## Hardware Integration

**Frequency range.** The AD9361 RFIC covers 70 MHz to 6 GHz. Reliable operation was observed from ~50 MHz to 6000 MHz in testing.

**USB timeout fix.** Initial configuration with 8 simultaneous transfers caused `bladerf_sync_rx` timeouts — the USB controller was saturated by transfer traffic competing with frequency/gain control commands. Resolved by reducing `num_transfers` from 8 to 4 and increasing `num_buffers` from 16 to 32. Frequency and gain updates are also gated — issued only when the value actually changes, not every capture loop iteration.

---

## Python Scripts

The Python analysis scripts mirror the C++ DSP chain exactly — same Hann window, same Welch averaging, same dBm normalization — applied offline to `.bin` capture files. `psd_plot.py` produces a static spectrum plot with noise floor overlay. `v2x_analysis.py` produces a time-frequency spectrogram with detected interference events annotated and exported to CSV.

---

## Key Design Decisions

| Decision | Alternative | Reason chosen |
|----------|-------------|---------------|
| Lock-free SPSC buffer | Mutex queue | 40 MSPS push rate — mutex causes USB timeouts |
| Hann window | Rectangular | Leakage raised noise floor unacceptably |
| Welch 8-frame average | Exponential averaging | Standard PSD estimator, matches Python reference |
| FFTW_MEASURE plan | FFTW_ESTIMATE | One-time startup cost, faster runtime |
| Non-overlapping batches | 50% overlap | Simpler, sufficient update rate |
| fftshift post-FFT | DC-centered tuning | AD9361 is direct conversion — DC spike unavoidable |
| `complex<float>` throughout | Separate I/Q arrays | Matches FFTW3 native format |
