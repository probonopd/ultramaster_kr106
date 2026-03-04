#!/usr/bin/env python3
"""
Model the Juno-6 analog ADSR curve from circuit analysis.

Circuit chain (from schematic):
  100K(B) linear slider + 4.7K shunt (pin1-pin2) → TA75358S buffer → IR3R01 (47nF)

The IR3R01 uses an internal exponential-rate oscillator:
  - CV input (0-5V) controls the RATE of charge/discharge
  - rate ∝ e^(k * V)
  - time = constant / rate ∝ e^(-k * V)

We derive the pot taper from the circuit, then fit the exponential slope (k)
to match the published Juno-6 specs (Attack: 1-3000ms, Decay/Release: 2-12000ms).
Finally, compare the Juno-6 model to our Juno-106 measured data.
"""

import numpy as np
import csv

# ──────────────────────────────────────────────────────────────
# Circuit parameters from Juno-6 schematic (page 9 + panel board)
# ──────────────────────────────────────────────────────────────
R_POT = 100_000      # 100K(B) linear slider
R_SHUNT = 4_700      # 4K7 between pin 1 (ground) and pin 2 (wiper)
V_SUPPLY = 5.0       # +5V at top of slider
C_TIMING = 47e-9     # 47nF timing capacitor on IR3R01


def pot_taper(s):
    """
    Compute voltage at wiper of 100K linear pot with 4.7K shunt.

    s: slider position 0.0 (bottom/ground) to 1.0 (top/+5V)
    Returns: voltage at wiper (0 to V_SUPPLY)

    Circuit: R_lower = s * R_POT in parallel with R_SHUNT
             R_upper = (1 - s) * R_POT
             V = V_SUPPLY * R_eff_lower / (R_eff_lower + R_upper)
    """
    s = np.asarray(s, dtype=float)
    v = np.zeros_like(s)

    # Handle s=0 (V=0) and s=1 (V=Vcc) separately to avoid division issues
    mask = (s > 0) & (s < 1)
    sm = s[mask]

    r_lower = sm * R_POT
    r_eff = (r_lower * R_SHUNT) / (r_lower + R_SHUNT)  # parallel
    r_upper = (1 - sm) * R_POT

    v[mask] = V_SUPPLY * r_eff / (r_eff + r_upper)
    v[s >= 1.0] = V_SUPPLY

    return v


def juno6_time(s, t_min, t_max):
    """
    Full Juno-6 slider → time mapping.

    The IR3R01's exponential oscillator gives:
      time(V) = t_min * (t_max / t_min) ^ (V / V_max)

    where V is the tapered pot voltage.

    s: slider position 0-1
    t_min: time at slider=0 (ms)
    t_max: time at slider=1 (ms)
    """
    v = pot_taper(s)
    # Exponential mapping: V=0 → t_min, V=V_SUPPLY → t_max
    ratio = t_max / t_min
    return t_min * np.power(ratio, v / V_SUPPLY)


def read_measured_data(csv_path):
    """Read measured data, return dict of stage -> [(raw, ms, confidence)]."""
    data = {'A': [], 'D': [], 'R': []}
    with open(csv_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('Preset'):
                continue
            parts = line.split(',')
            if len(parts) < 5:
                continue
            stage = parts[1].strip()
            raw = float(parts[2])
            ms = float(parts[3])
            conf = parts[4].strip()
            if stage in data:
                data[stage].append((raw, ms, conf))
    return data


if __name__ == "__main__":
    print("=" * 72)
    print("  Juno-6 Analog ADSR Model (from circuit analysis)")
    print("  100K(B) pot + 4.7K shunt → IR3R01 exponential oscillator (47nF)")
    print("=" * 72)

    # ── Pot taper curve ──────────────────────────────────────
    print("\n1. Pot taper (voltage at wiper vs slider position):")
    print(f"   {'Slider':>6}  {'Voltage':>8}  {'Linear':>8}  {'Compression':>11}")
    for s in np.arange(0, 1.05, 0.1):
        s = min(s, 1.0)
        v = float(pot_taper(np.array([s]))[0])
        v_lin = s * V_SUPPLY
        comp = v / v_lin if v_lin > 0 else 0
        print(f"   {s:6.1f}  {v:7.3f}V  {v_lin:7.3f}V  {comp:10.1%}")

    # ── Juno-6 model curves ──────────────────────────────────
    # Published specs: Attack 1-3000ms, Decay/Release 2-12000ms
    specs = {
        'A': (1.0, 3000.0, "Attack"),
        'D': (2.0, 12000.0, "Decay"),
        'R': (2.0, 12000.0, "Release"),
    }

    slider = np.linspace(0, 1, 128)

    print("\n2. Juno-6 model — time vs slider (sampled):")
    for stage, (t_min, t_max, name) in specs.items():
        times = juno6_time(slider, t_min, t_max)
        print(f"\n   {name} ({t_min}ms – {t_max}ms):")
        print(f"   {'Slider':>6}  {'Index':>5}  {'Time(ms)':>10}")
        for s_val in [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]:
            idx = min(int(round(s_val * 127)), 127)
            print(f"   {s_val:6.1f}  {idx:5d}  {times[idx]:10.1f}")

    # ── Compare to Juno-106 measured data ─────────────────────
    print("\n3. Juno-6 model vs Juno-106 measured data:")
    data = read_measured_data('measured_data.csv')

    for stage, (t_min, t_max, name) in specs.items():
        points = data.get(stage, [])
        if not points:
            continue

        print(f"\n   {name}:")
        print(f"   {'Raw':>6}  {'Measured':>10}  {'Juno6 Model':>12}  {'Ratio':>8}  {'Conf':>6}")

        for raw, ms_measured, conf in sorted(points, key=lambda x: x[0]):
            ms_model = float(juno6_time(np.array([raw]), t_min, t_max)[0])
            ratio = ms_measured / ms_model if ms_model > 0 else float('inf')
            print(f"   {raw:6.4f}  {ms_measured:10.1f}  {ms_model:12.1f}  {ratio:7.2f}x  {conf:>6}")

    # ── Print C++ lookup tables for the Juno-6 model ──────────
    print("\n" + "=" * 72)
    print("4. Juno-6 C++ Lookup Tables:")
    print("=" * 72)

    for stage, (t_min, t_max, name) in specs.items():
        times = juno6_time(slider, t_min, t_max)
        cpp_name = f"kJuno6{name}LUT"
        print(f"\n// Juno-6 {name}: {t_min:.0f}ms – {t_max:.0f}ms")
        print(f"static constexpr float {cpp_name}[128] = {{")
        for i in range(0, 128, 8):
            chunk = times[i:i+8]
            vals = ", ".join(f"{v:.1f}f" for v in chunk)
            comma = "," if i + 8 < 128 else ""
            print(f"  {vals}{comma}")
        print("};")
