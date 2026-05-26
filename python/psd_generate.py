"""
psd_generate.py

Generates synthetic IQ test data for psd_plot.py.
Produces a clean two-tone signal with a noise floor.

Usage:
    python3 psd_generate.py --out test_tone.bin --freq1 5 --freq2 -8
"""

import argparse
import numpy as np
from pathlib import Path


def generate_two_tone(sample_rate_hz: float, duration_s: float,
                      freq1_mhz: float, freq2_mhz: float,
                      power1_dbm: float = -20.0,
                      power2_dbm: float = -30.0,
                      noise_dbm: float  = -90.0) -> np.ndarray:
    """
    Generate two complex tones plus thermal noise.
    Frequencies are offsets from center in MHz.
    """
    n = int(sample_rate_hz * duration_s)
    t = np.arange(n) / sample_rate_hz

    amp1  = np.sqrt(10 ** (power1_dbm / 10) / 1000)
    amp2  = np.sqrt(10 ** (power2_dbm / 10) / 1000)
    noise_amp = np.sqrt(10 ** (noise_dbm / 10) / 1000 / 2)

    tone1 = amp1 * np.exp(1j * 2 * np.pi * freq1_mhz * 1e6 * t)
    tone2 = amp2 * np.exp(1j * 2 * np.pi * freq2_mhz * 1e6 * t)
    noise = noise_amp * (np.random.randn(n) + 1j * np.random.randn(n))

    return (tone1 + tone2 + noise).astype(np.complex64)


def save_iq(samples: np.ndarray, path: str):
    """Save complex64 samples as int16 interleaved binary."""
    raw = np.empty(len(samples) * 2, dtype=np.int16)
    raw[0::2] = np.clip(samples.real * 2048, -2048, 2047).astype(np.int16)
    raw[1::2] = np.clip(samples.imag * 2048, -2048, 2047).astype(np.int16)
    raw.tofile(path)
    print(f"[psd_generate] saved {len(samples):,} samples to {path}")
    print(f"[psd_generate] file size: {Path(path).stat().st_size / 1e6:.1f} MB")


def main():
    parser = argparse.ArgumentParser(
        description='Generate synthetic IQ data for PSD testing')
    parser.add_argument('--out',      type=str,   default='test_psd.bin',
                        help='Output .bin file (default: test_psd.bin)')
    parser.add_argument('--rate',     type=float, default=40.0,
                        help='Sample rate MSPS (default: 40)')
    parser.add_argument('--duration', type=float, default=1.0,
                        help='Duration seconds (default: 1.0)')
    parser.add_argument('--freq1',    type=float, default=5.0,
                        help='Tone 1 offset MHz from center (default: +5)')
    parser.add_argument('--freq2',    type=float, default=-8.0,
                        help='Tone 2 offset MHz from center (default: -8)')
    parser.add_argument('--power1',   type=float, default=-20.0,
                        help='Tone 1 power dBm (default: -20)')
    parser.add_argument('--power2',   type=float, default=-30.0,
                        help='Tone 2 power dBm (default: -30)')
    parser.add_argument('--noise',    type=float, default=-90.0,
                        help='Noise floor dBm (default: -90)')
    args = parser.parse_args()

    print(f"[psd_generate] generating {args.duration}s at {args.rate} MSPS")
    print(f"[psd_generate] tone1: {args.freq1:+.1f} MHz  {args.power1:.0f} dBm")
    print(f"[psd_generate] tone2: {args.freq2:+.1f} MHz  {args.power2:.0f} dBm")
    print(f"[psd_generate] noise: {args.noise:.0f} dBm")

    samples = generate_two_tone(
        args.rate * 1e6, args.duration,
        args.freq1, args.freq2,
        args.power1, args.power2, args.noise
    )

    save_iq(samples, args.out)
    print(f"\n[psd_generate] run analysis with:")
    print(f"  python3 psd_plot.py {args.out} --freq 915 --rate {args.rate}")


if __name__ == '__main__':
    main()