"""
v2x_analysis.py

Analyzes a raw IQ capture from the 5.9 GHz V2X/DSRC band.
Detects interference events by power level and duration,
plots a spectrogram with events highlighted, and saves a CSV event log.

The 5.9 GHz DSRC band (5.850–5.925 GHz) is used by C-V2X and DSRC
for vehicle-to-vehicle and vehicle-to-infrastructure communication.
Interference on this band is a safety concern — it can prevent vehicles
from receiving position/speed broadcasts from nearby vehicles.

File format: raw interleaved int16 I/Q pairs (SC16_Q11)
as written by save_iq_file() in capture.cpp.

Usage:
    # generate test data first
    python3 v2x_generate.py --out test_v2x.bin

    # then analyze
    python3 v2x_analysis.py test_v2x.bin --freq 5900 --rate 40
"""

import argparse
import csv
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from pathlib import Path
from dataclasses import dataclass


# ── constants ─────────────────────────────────────────────────────────────────

# DSRC/C-V2X channel map (MHz offset from 5900 MHz center)
V2X_CHANNELS = {
    172: "SCH1",
    174: "SCH2",
    176: "CCH",   # control channel — most safety-critical
    178: "SCH3",
    180: "SCH4",
    182: "SCH5",
    184: "SCH6",
}

DETECTION_THRESHOLD_DB = 12.0
MIN_EVENT_DURATION_MS  = 0.5


# ── data structures ───────────────────────────────────────────────────────────

@dataclass
class InterferenceEvent:
    start_ms:    float
    duration_ms: float
    peak_dbm:    float
    center_mhz:  float
    channel:     str


# ── IQ loading ────────────────────────────────────────────────────────────────

def load_iq(path: str) -> np.ndarray:
    """Load raw int16 IQ file, return normalized complex64 array."""
    raw = np.fromfile(path, dtype=np.int16)
    if len(raw) % 2 != 0:
        raw = raw[:-1]
    samples = raw.astype(np.float32) / 2048.0
    return samples[0::2] + 1j * samples[1::2]


# ── spectrogram ───────────────────────────────────────────────────────────────

def compute_spectrogram(samples: np.ndarray, fft_size: int,
                        sample_rate_hz: float, cal_db: float) -> np.ndarray:
    """
    Compute a time×frequency power matrix in dBm.
    Each row is one FFT frame. Returns (n_frames × fft_size) fftshifted.
    """
    n_frames = len(samples) // fft_size
    window   = np.hanning(fft_size).astype(np.float32)
    spec     = np.zeros((n_frames, fft_size), dtype=np.float32)

    for i in range(n_frames):
        frame   = samples[i * fft_size:(i + 1) * fft_size] * window
        X       = np.fft.fft(frame)
        power   = np.abs(X) ** 2 / (fft_size ** 2)
        power   = np.maximum(power, 1e-20)
        spec[i] = np.fft.fftshift(10.0 * np.log10(power) + cal_db)

    # null DC bin
    half = fft_size // 2
    spec[:, half] = (spec[:, half - 1] + spec[:, half + 1]) / 2.0

    return spec


# ── event detection ───────────────────────────────────────────────────────────

def detect_events(spec: np.ndarray, sample_rate_hz: float,
                  fft_size: int, center_mhz: float) -> tuple:
    """
    Walk through the spectrogram frame by frame.
    Any frame where max power exceeds noise_floor + threshold starts an event.
    """
    frame_ms      = (fft_size / sample_rate_hz) * 1000.0
    bin_width_mhz = (sample_rate_hz / 1e6) / fft_size
    n_frames      = spec.shape[0]

    # noise floor from median of all spectrogram values
    # using all values is more robust than per-frame max when SNR is low
    noise_floor = float(np.median(spec))
    threshold   = noise_floor + DETECTION_THRESHOLD_DB

    events    = []
    in_event  = False
    ev_start  = 0.0
    ev_peak   = -200.0
    ev_bin    = 0

    for i in range(n_frames):
        frame_max = float(np.max(spec[i]))
        frame_bin = int(np.argmax(spec[i]))

        if frame_max > threshold:
            if not in_event:
                in_event = True
                ev_start = i * frame_ms
                ev_peak  = frame_max
                ev_bin   = frame_bin
            else:
                if frame_max > ev_peak:
                    ev_peak = frame_max
                    ev_bin  = frame_bin
        else:
            if in_event:
                duration = i * frame_ms - ev_start
                if duration >= MIN_EVENT_DURATION_MS:
                    freq_mhz = center_mhz + (ev_bin - spec.shape[1] // 2) * bin_width_mhz
                    channel  = "unknown"
                    for offset, name in V2X_CHANNELS.items():
                        if abs(freq_mhz - (5000 + offset * 10)) < 5:
                            channel = name
                            break
                    events.append(InterferenceEvent(
                        ev_start, duration, ev_peak, freq_mhz, channel))
                in_event = False

    print(f"[v2x] noise floor: {noise_floor:.1f} dBm  threshold: {threshold:.1f} dBm")
    return events, noise_floor


# ── save csv ──────────────────────────────────────────────────────────────────

def save_csv(events: list, path: str):
    with open(path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['start_ms', 'duration_ms', 'peak_dbm',
                         'center_mhz', 'channel'])
        for e in events:
            writer.writerow([f'{e.start_ms:.3f}', f'{e.duration_ms:.3f}',
                             f'{e.peak_dbm:.1f}', f'{e.center_mhz:.3f}',
                             e.channel])
    print(f"[v2x] event log saved to {path}")


# ── plot ──────────────────────────────────────────────────────────────────────

def plot_results(spec: np.ndarray, events: list, noise_floor: float,
                 center_mhz: float, sample_rate_mhz: float,
                 fft_size: int, sample_rate_hz: float, filename: str):

    n_frames, n_bins  = spec.shape
    frame_ms          = (fft_size / sample_rate_hz) * 1000.0
    total_ms          = n_frames * frame_ms
    freq_start        = center_mhz - sample_rate_mhz / 2
    freq_stop         = center_mhz + sample_rate_mhz / 2

    fig, (ax_spec, ax_psd) = plt.subplots(
        2, 1, figsize=(14, 8),
        gridspec_kw={'height_ratios': [3, 1]}
    )
    fig.patch.set_facecolor('#0e0e0e')

    # spectrogram
    ax_spec.set_facecolor('#0e0e0e')
    vmin = noise_floor - 5
    vmax = np.percentile(spec, 99)
    ax_spec.imshow(spec, aspect='auto', origin='upper',
                   extent=[freq_start, freq_stop, total_ms, 0],
                   cmap='jet', vmin=vmin, vmax=vmax)

    # event boxes with labels inside
    for e in events:
        freq_half = 2.0
        rect = mpatches.Rectangle(
            (e.center_mhz - freq_half, e.start_ms),
            freq_half * 2, e.duration_ms,
            linewidth=1.5, edgecolor='#ffffff',
            facecolor='none', linestyle='--'
        )
        ax_spec.add_patch(rect)
        label_y = e.start_ms + e.duration_ms / 2.0
        ax_spec.text(e.center_mhz, label_y,
                     f'{e.peak_dbm:.0f}dBm\n{e.duration_ms:.1f}ms',
                     color='white', fontsize=7, ha='center', va='center',
                     bbox=dict(facecolor='#000000', alpha=0.4, pad=1,
                               edgecolor='none'))

    ax_spec.set_xlabel('Frequency (MHz)', color='#cccccc')
    ax_spec.set_ylabel('Time (ms)', color='#cccccc')
    ax_spec.set_title(
        f'V2X Band Spectrogram — {Path(filename).stem}  |  '
        f'Center: {center_mhz} MHz  |  {len(events)} event(s) detected',
        color='white', pad=12
    )
    ax_spec.tick_params(colors='#cccccc')
    for spine in ax_spec.spines.values():
        spine.set_edgecolor('#444444')

    # average PSD
    ax_psd.set_facecolor('#0e0e0e')
    avg_psd = np.mean(spec, axis=0)
    freq    = np.linspace(freq_start, freq_stop, n_bins)
    ax_psd.plot(freq, avg_psd, color='#00e676', linewidth=0.7, label='Average PSD')
    ax_psd.axhline(noise_floor, color='#ff6d00', linewidth=1,
                   linestyle='--', label=f'Noise floor {noise_floor:.1f} dBm')
    ax_psd.axhline(noise_floor + DETECTION_THRESHOLD_DB,
                   color='#ffffff', linewidth=0.8, linestyle=':',
                   label=f'Threshold +{DETECTION_THRESHOLD_DB:.0f} dB')
    ax_psd.set_xlabel('Frequency (MHz)', color='#cccccc')
    ax_psd.set_ylabel('Power (dBm)', color='#cccccc')
    ax_psd.grid(True, color='#333333', linewidth=0.4)
    ax_psd.tick_params(colors='#cccccc')
    ax_psd.legend(facecolor='#1a1a1a', labelcolor='#cccccc', fontsize=8)
    for spine in ax_psd.spines.values():
        spine.set_edgecolor('#444444')

    plt.tight_layout()
    plt.subplots_adjust(top=0.90)
    out_path = Path(filename).with_suffix('.png')
    plt.savefig(out_path, dpi=150, facecolor=fig.get_facecolor())
    print(f"[v2x] plot saved to {out_path}")
    plt.show()


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='V2X band interference characterization')
    parser.add_argument('file',    type=str,
                        help='Path to .bin IQ capture file')
    parser.add_argument('--freq',  type=float, default=5900.0,
                        help='Center frequency MHz (default: 5900)')
    parser.add_argument('--rate',  type=float, default=40.0,
                        help='Sample rate MSPS (default: 40)')
    parser.add_argument('--fft',   type=int,   default=2048,
                        help='FFT size per time slice (default: 2048)')
    parser.add_argument('--cal',   type=float, default=0.0,
                        help='Calibration offset dB (default: 0)')
    args = parser.parse_args()

    print(f"[v2x] loading {args.file}")
    samples = load_iq(args.file)
    print(f"[v2x] {len(samples):,} samples  |  "
          f"{len(samples) / (args.rate * 1e6) * 1000:.1f} ms captured")

    spec = compute_spectrogram(samples, args.fft, args.rate * 1e6, args.cal)
    events, noise_floor = detect_events(spec, args.rate * 1e6, args.fft, args.freq)

    print(f"\n[v2x] {len(events)} interference event(s) detected:\n")
    print(f"  {'Start(ms)':>10}  {'Duration(ms)':>13}  "
          f"{'Peak(dBm)':>10}  {'Freq(MHz)':>10}  Channel")
    print("  " + "-" * 60)
    for e in events:
        print(f"  {e.start_ms:>10.1f}  {e.duration_ms:>13.1f}  "
              f"{e.peak_dbm:>10.1f}  {e.center_mhz:>10.1f}  {e.channel}")

    save_csv(events, str(Path(args.file).with_suffix('.csv')))
    plot_results(spec, events, noise_floor,
                 args.freq, args.rate, args.fft, args.rate * 1e6, args.file)


if __name__ == '__main__':
    main()