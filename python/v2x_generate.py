"""
v2x_generate.py

Generates synthetic V2X/DSRC IQ test data for v2x_analysis.py.

Models:
- Thermal noise floor
- Periodic C-V2X beacon bursts (10ms every 100ms)
- Configurable interference events

Usage:
    python3 v2x_generate.py --out test_v2x.bin
    python3 v2x_generate.py --out test_v2x.bin --duration 1.0 --events 3
"""

import argparse
import numpy as np
from pathlib import Path


def generate_v2x(sample_rate_hz: float, duration_s: float,
                 n_interference: int, noise_dbm: float = -40.0) -> np.ndarray:
    """
    Generate synthetic V2X band IQ data.

    Produces:
    - Thermal noise floor
    - Periodic V2X beacon bursts at +1.5 MHz offset (CCH-like)
    - n_interference random interference events
    """
    n = int(sample_rate_hz * duration_s)
    t = np.arange(n) / sample_rate_hz

    noise_amp = np.sqrt(10 ** (noise_dbm / 10) / 1000 / 2)
    signal    = noise_amp * (np.random.randn(n) + 1j * np.random.randn(n))

    # V2X beacon bursts — 10ms on, 90ms off, at +1.5 MHz
    beacon_power  = 10 ** (-55.0 / 10) / 1000
    burst_len     = int(0.010 * sample_rate_hz)
    period_len    = int(0.100 * sample_rate_hz)

    for start in range(0, n - burst_len, period_len):
        t_b = t[start:start + burst_len]
        signal[start:start + burst_len] += (
            np.sqrt(beacon_power) *
            np.exp(1j * 2 * np.pi * 1.5e6 * t_b)
        ).astype(np.complex64)

    # random interference events
    rng = np.random.default_rng(seed=42)
    for i in range(n_interference):
        # random start, duration 5–50ms, power -65 to -40 dBm
        start_frac  = rng.uniform(0.05, 0.90)
        duration_s_ = rng.uniform(0.005, 0.050)
        power_dbm   = rng.uniform(-65.0, -40.0)
        freq_offset = rng.uniform(-15e6, 15e6)  # anywhere in the band

        i_start = int(start_frac * n)
        i_len   = min(int(duration_s_ * sample_rate_hz), n - i_start)
        i_power = np.sqrt(10 ** (power_dbm / 10) / 1000)
        t_i     = t[i_start:i_start + i_len]

        # narrowband tone
        signal[i_start:i_start + i_len] += (
            i_power * np.exp(1j * 2 * np.pi * freq_offset * t_i)
        ).astype(np.complex64)

        print(f"[v2x_generate] interference {i+1}: "
              f"start={start_frac*duration_s*1000:.0f}ms  "
              f"duration={duration_s_*1000:.0f}ms  "
              f"power={power_dbm:.0f}dBm  "
              f"offset={freq_offset/1e6:+.1f}MHz")

    return signal.astype(np.complex64)


def save_iq(samples: np.ndarray, path: str):
    """Save complex64 samples as int16 interleaved binary."""
    raw = np.empty(len(samples) * 2, dtype=np.int16)
    raw[0::2] = np.clip(samples.real * 2048, -2048, 2047).astype(np.int16)
    raw[1::2] = np.clip(samples.imag * 2048, -2048, 2047).astype(np.int16)
    raw.tofile(path)
    print(f"[v2x_generate] saved {len(samples):,} samples to {path}")
    print(f"[v2x_generate] file size: {Path(path).stat().st_size / 1e6:.1f} MB")


def main():
    parser = argparse.ArgumentParser(
        description='Generate synthetic V2X IQ data for testing')
    parser.add_argument('--out',      type=str,   default='test_v2x.bin',
                        help='Output .bin file (default: test_v2x.bin)')
    parser.add_argument('--rate',     type=float, default=40.0,
                        help='Sample rate MSPS (default: 40)')
    parser.add_argument('--duration', type=float, default=0.5,
                        help='Duration seconds (default: 0.5)')
    parser.add_argument('--events',   type=int,   default=3,
                        help='Number of random interference events (default: 3)')
    parser.add_argument('--noise',    type=float, default=-40.0,
                        help='Noise floor dBm (default: -90)')
    args = parser.parse_args()

    print(f"[v2x_generate] generating {args.duration}s at {args.rate} MSPS")
    print(f"[v2x_generate] {args.events} random interference events")

    samples = generate_v2x(
        args.rate * 1e6, args.duration,
        args.events, args.noise
    )

    save_iq(samples, args.out)
    print(f"\n[v2x_generate] run analysis with:")
    print(f"  python3 v2x_analysis.py {args.out} --freq 5900 --rate {args.rate}")


if __name__ == '__main__':
    main()