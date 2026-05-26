"""
psd_plot.py

Reads a raw IQ capture file (.bin), computes PSD using Welch's method,
plots the spectrum in dBm, and overlays a noise floor estimate.

File format expected: raw interleaved int16 I/Q pairs (SC16_Q11)
as written by the C++ save_iq_file() function.

Usage:
    python3 psd_plot.py <path_to_iq_file.bin> --freq 915 --rate 40

Arguments:
    file        path to .bin IQ capture file
    --freq      center frequency in MHz (default: 915)
    --rate      sample rate in MSPS (default: 40)
    --fft       FFT size (default: 4096)
    --cal       calibration offset in dB (default: 0)
"""

import argparse
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path


def load_iq(path: str) -> np.ndarray:
    """Read raw int16 interleaved IQ file, return complex64 array."""
    raw = np.fromfile(path, dtype=np.int16)

    # must be even — I/Q pairs
    if len(raw) % 2 != 0:
        raw = raw[:-1]

    # normalize to (-1, +1) matching the C++ / 2048.0f scaling
    samples = raw.astype(np.float32) / 2048.0

    # interleaved: [I0, Q0, I1, Q1, ...] → complex
    return samples[0::2] + 1j * samples[1::2]


def compute_psd_welch(samples: np.ndarray, fft_size: int,
                      sample_rate_hz: float, cal_db: float) -> tuple:
    """
    Compute PSD using Welch's method.
    Returns (freq_mhz array, psd_dbm array, center_bin_mask).
    """
    n_frames = len(samples) // fft_size
    if n_frames == 0:
        raise ValueError(f"Not enough samples for fft_size={fft_size}")

    # Hann window
    window = np.hanning(fft_size).astype(np.float32)
    window_power = np.sum(window ** 2)

    accum = np.zeros(fft_size, dtype=np.float64)

    for i in range(n_frames):
        frame = samples[i * fft_size:(i + 1) * fft_size]
        windowed = frame * window
        X = np.fft.fft(windowed)
        accum += np.abs(X) ** 2

    # average over frames
    avg = accum / n_frames

    # normalize: divide by N^2, apply calibration
    power = avg / (fft_size ** 2)
    power = np.maximum(power, 1e-20)  # avoid log(0)
    psd_dbm = 10.0 * np.log10(power) + cal_db

    # fftshift so DC is centered
    psd_dbm = np.fft.fftshift(psd_dbm)

    # null center bin (DC offset artifact)
    half = fft_size // 2
    psd_dbm[half] = (psd_dbm[half - 1] + psd_dbm[half + 1]) / 2.0

    return psd_dbm, n_frames


def estimate_noise_floor(psd_dbm: np.ndarray) -> float:
    """
    Estimate noise floor as the median of the lower 60% of values.
    Median is robust to signal spikes — gives the background level.
    """
    sorted_vals = np.sort(psd_dbm)
    lower_60 = sorted_vals[:int(len(sorted_vals) * 0.60)]
    return float(np.median(lower_60))


def plot_psd(psd_dbm: np.ndarray, center_mhz: float,
             sample_rate_mhz: float, noise_floor: float,
             n_frames: int, filename: str):
    """Plot PSD with noise floor overlay."""

    n = len(psd_dbm)
    freq = np.linspace(center_mhz - sample_rate_mhz / 2,
                       center_mhz + sample_rate_mhz / 2,
                       n)

    fig, ax = plt.subplots(figsize=(14, 5))
    fig.patch.set_facecolor('#0e0e0e')
    ax.set_facecolor('#0e0e0e')

    # PSD trace
    ax.plot(freq, psd_dbm, color='#00e676', linewidth=0.6,
            label=f'PSD ({n_frames} frames averaged)')

    # noise floor line
    ax.axhline(noise_floor, color='#ff6d00', linewidth=1.2,
               linestyle='--', label=f'Noise floor ≈ {noise_floor:.1f} dBm')

    # mark peaks more than 10 dB above noise floor
    peak_threshold = noise_floor + 10.0
    peak_mask = psd_dbm > peak_threshold
    if np.any(peak_mask):
        ax.fill_between(freq, noise_floor, psd_dbm,
                        where=peak_mask, alpha=0.25,
                        color='#ff6d00', label='Signal > noise+10dB')

    ax.set_xlabel('Frequency (MHz)', color='#cccccc')
    ax.set_ylabel('Power (dBm)', color='#cccccc')
    ax.set_title(f'PSD — {Path(filename).name}  |  Center: {center_mhz} MHz',
                 color='#ffffff')
    ax.set_ylim(noise_floor - 20, max(psd_dbm) + 10)
    ax.grid(True, color='#333333', linewidth=0.4)
    ax.tick_params(colors='#cccccc')
    ax.legend(facecolor='#1a1a1a', labelcolor='#cccccc', fontsize=9)

    for spine in ax.spines.values():
        spine.set_edgecolor('#444444')

    plt.tight_layout()
    out_path = Path(filename).with_suffix('.png')
    plt.savefig(out_path, dpi=150, facecolor=fig.get_facecolor())
    print(f"[psd_plot] saved plot to {out_path}")
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Plot PSD from IQ capture file')
    parser.add_argument('file',          type=str,   help='Path to .bin IQ file')
    parser.add_argument('--freq',        type=float, default=915.0,
                        help='Center frequency MHz (default: 915)')
    parser.add_argument('--rate',        type=float, default=40.0,
                        help='Sample rate MSPS (default: 40)')
    parser.add_argument('--fft',         type=int,   default=4096,
                        help='FFT size (default: 4096)')
    parser.add_argument('--cal',         type=float, default=0.0,
                        help='Calibration offset dB (default: 0)')
    args = parser.parse_args()

    print(f"[psd_plot] loading {args.file}")
    samples = load_iq(args.file)
    print(f"[psd_plot] {len(samples):,} samples loaded")

    psd_dbm, n_frames = compute_psd_welch(
        samples, args.fft,
        args.rate * 1e6, args.cal
    )

    noise_floor = estimate_noise_floor(psd_dbm)
    print(f"[psd_plot] noise floor estimate: {noise_floor:.1f} dBm")
    print(f"[psd_plot] peak power: {np.max(psd_dbm):.1f} dBm")
    print(f"[psd_plot] SNR estimate: {np.max(psd_dbm) - noise_floor:.1f} dB")

    plot_psd(psd_dbm, args.freq, args.rate,
             noise_floor, n_frames, args.file)


if __name__ == '__main__':
    main()