#!/usr/bin/env python3
"""
Generate 128-entry ADSR lookup tables for the KR106 Juno-106 emulation.
Interpolates between measured anchor points in log space using PCHIP
(Piecewise Cubic Hermite Interpolating Polynomial — monotonicity-preserving).

Anchor points curated from measured_data.csv. Endpoints pinned to published
Roland Juno-106 specs (Attack: 1–3000ms, Decay/Release: 2–12000ms).

Usage: python3 generate_lut.py
"""

import numpy as np
from scipy.interpolate import PchipInterpolator

# ──────────────────────────────────────────────────────────────
# Curated anchor points: (slider_0_to_1, time_ms)
# Edit these as new measurements come in.
# All lists MUST be monotonically increasing in both columns.
# ──────────────────────────────────────────────────────────────

ATTACK_ANCHORS = [
    (0.000, 1.0),     # published spec min
    (0.016, 3.0),     # A25 Donald Pluck (medium)
    (0.024, 18.0),    # B82 Piccolo Trumpet (high); A11 Brass gave 30 (medium)
    (0.039, 31.0),    # B16 Recorder (medium)
    (0.055, 63.0),    # A24 Calliope (medium)
    (0.079, 75.0),    # A67 Shaker (medium)
    (0.102, 100.0),   # A15 Moving Strings (medium)
    (0.181, 150.0),   # A14 Flutes (medium)
    (0.339, 236.0),   # B12 Violin (high, no VCF confound)
    (0.457, 835.0),   # A34 Brass III (high)
    (1.000, 3000.0),  # published spec max
]

DECAY_ANCHORS = [
    (0.000, 2.0),      # published spec min
    (0.087, 17.0),     # A37 Pizzicato (high)
    (0.228, 250.0),    # A33 Xylophone (medium)
    (0.346, 565.0),    # B82 Piccolo Trumpet (medium); cross-validated by B12 Violin
    (0.370, 622.0),    # A76 Dark Synth Piano (high)
    (0.520, 925.0),    # A18 Piano I (medium)
    (0.638, 1310.0),   # A14 Flutes (high — gold standard, no VCF)
    (0.850, 11500.0),  # A58 Going Up (high; measured 13.5s, adjusted to fit spec)
    (1.000, 12000.0),  # published spec max
]

RELEASE_ANCHORS = [
    (0.000, 2.0),      # published spec min / A67 Shaker
    (0.016, 12.0),     # A47 Funky I (medium)
    (0.087, 12.0),     # B82 Piccolo Trumpet (high) — flat in low range
    (0.142, 67.0),     # A14 Flutes (high)
    (0.236, 105.0),    # A18 Piano I (medium); Recorder 15ms excluded as non-monotonic
    (0.252, 175.0),    # A11 Brass (medium)
    (0.291, 340.0),    # A12 Brass Swell (medium)
    (0.346, 660.0),    # A16 Brass & Strings (medium)
    (0.669, 2200.0),   # A84 Dust Storm (medium)
    (1.000, 12000.0),  # published spec max
]


def generate_lut(anchors, n=128):
    """Generate n-entry LUT via PCHIP interpolation in log space."""
    x = np.array([a[0] for a in anchors])
    y_log = np.log(np.array([a[1] for a in anchors]))
    interp = PchipInterpolator(x, y_log)
    slider = np.linspace(0, 1, n)
    lut = np.exp(interp(slider))
    return slider, lut


def print_cpp_array(name, lut):
    """Print as C++ array."""
    print(f"static constexpr float {name}[{len(lut)}] = {{")
    for i in range(0, len(lut), 8):
        chunk = lut[i:i+8]
        vals = ", ".join(f"{v:.1f}f" for v in chunk)
        comma = "," if i + 8 < len(lut) else ""
        print(f"  {vals}{comma}")
    print("};")
    print()


def print_comparison(stage_name, anchors, lut):
    """Print measured vs interpolated comparison."""
    print(f"{stage_name} — Anchor vs Interpolated:")
    print(f"  {'Raw':>6}  {'Sldr':>4}  {'Anchor':>8}  {'Interp':>8}  {'Err':>6}")
    for raw, ms in anchors:
        idx = min(int(round(raw * 127)), 127)
        interp_val = lut[idx]
        err_pct = (interp_val - ms) / ms * 100
        print(f"  {raw:6.4f}  {idx:4d}  {ms:8.1f}  {interp_val:8.1f}  {err_pct:5.1f}%")
    print()


def print_sample_values(stage_name, lut):
    """Print LUT at regular intervals for quick sanity check."""
    print(f"{stage_name} — Sampled values:")
    print(f"  {'Slider':>6}  {'Index':>5}  {'ms':>10}")
    for s in [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]:
        idx = min(int(round(s * 127)), 127)
        print(f"  {s:6.1f}  {idx:5d}  {lut[idx]:10.1f}")
    print()


if __name__ == "__main__":
    print("=" * 64)
    print("  KR106 ADSR Lookup Tables")
    print("  From measured Juno-106 factory preset recordings")
    print("=" * 64)

    stages = [
        ("Attack",  "kAttackLUT",  ATTACK_ANCHORS),
        ("Decay",   "kDecayLUT",   DECAY_ANCHORS),
        ("Release", "kReleaseLUT", RELEASE_ANCHORS),
    ]

    for stage_name, cpp_name, anchors in stages:
        slider, lut = generate_lut(anchors)
        print(f"\n// {stage_name}: {anchors[0][1]:.0f}ms – {anchors[-1][1]:.0f}ms")
        print_cpp_array(cpp_name, lut)
        print_comparison(stage_name, anchors, lut)
        print_sample_values(stage_name, lut)

    print("=" * 64)
    print("Usage in DSP:")
    print("  float s = static_cast<float>(value) * 127.0f;")
    print("  int idx = static_cast<int>(s);")
    print("  float frac = s - idx;")
    print("  if (idx >= 127) { idx = 126; frac = 1.0f; }")
    print("  float ms = kLUT[idx] + frac * (kLUT[idx+1] - kLUT[idx]);")
    print("=" * 64)
