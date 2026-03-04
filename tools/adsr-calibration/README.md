# ADSR Calibration — Juno-106 Envelope Timing

## Overview

The KR106 plugin emulates the Roland Juno-106 synthesizer. The Juno-106's ADSR envelope is generated digitally by an 80C49 microcontroller through DACs (NOT the IR3R01 analog chip used in the earlier Juno-6/60). The slider-to-milliseconds mapping is nonlinear and cannot be captured by a single formula.

We measured envelope timings from real Juno-106 factory preset recordings, then generated 128-entry lookup tables via PCHIP interpolation in log space. These LUTs replace the previous cubic approximation (`1.5 + s³ × range`), which was inaccurate in the mid-range.

## Published Roland Specs
- Attack: 1–3000 ms
- Decay: 2–12000 ms
- Release: 2–12000 ms
- Sustain: 0–1 (linear, no LUT needed)

## File Inventory

| File | Purpose |
|------|---------|
| `measured_data.csv` | All measured timing data from real Juno-106 recordings. ~80 data points across ~30 presets. Columns: Preset, Stage (A/D/R), Raw slider (0-1), Measured ms, Confidence (high/medium/low), Notes. |
| `generate_lut.py` | Generates the 128-entry C++ LUT arrays from curated anchor points using PCHIP interpolation in log space. Run: `python3 generate_lut.py` (requires numpy, scipy). |
| `juno6_model.py` | Juno-6 analog ADSR model from circuit analysis (100K pot + 4.7K shunt + IR3R01). For reference/comparison only — confirmed that Juno-6 and Juno-106 have fundamentally different ADSR curves. |
| `adsr_overview.csv` | All 128 presets with raw slider values and measured data merged in. For quick reference when choosing presets to measure. |
| `README.md` | This file. |

## Where the LUTs Live in DSP Code

**File:** `DSP/KR106_DSP.h`

Three `static constexpr float` arrays:
- `kAttackLUT[128]` — Attack: 1–3000 ms
- `kDecayLUT[128]` — Decay: 2–12000 ms
- `kReleaseLUT[128]` — Release: 2–12000 ms

Helper function `LookupADSR(const float* lut, float s)` does linear interpolation between table entries. The `SetParam` cases for `kEnvA`, `kEnvD`, `kEnvR` call this instead of the old cubic formula.

## Workflow to Update the LUTs

1. Add new measurements to `measured_data.csv`
2. Curate anchor points in `generate_lut.py` (ATTACK_ANCHORS, DECAY_ANCHORS, RELEASE_ANCHORS)
   - Only use high/medium confidence points
   - Both columns (slider and ms) must be monotonically increasing
   - Pin endpoints to published spec values
3. Run `python3 generate_lut.py` — prints C++ arrays and comparison tables
4. Copy the three arrays into `DSP/KR106_DSP.h`, replacing the existing ones
5. Build and test

## Current Anchor Points (as of 2026-03-04)

### Attack (11 points)
```
Slider   ms      Source
0.000    1.0     spec min
0.016    3.0     A25 Donald Pluck
0.024    18.0    B82 Piccolo Trumpet (high)
0.039    31.0    B16 Recorder
0.055    63.0    A24 Calliope
0.079    75.0    A67 Shaker
0.102    100.0   A15 Moving Strings
0.181    150.0   A14 Flutes
0.339    236.0   B12 Violin (high, no VCF)
0.457    835.0   A34 Brass III (high)
1.000    3000.0  spec max
```

### Decay (9 points)
```
Slider   ms       Source
0.000    2.0      spec min
0.087    17.0     A37 Pizzicato (high)
0.228    250.0    A33 Xylophone
0.346    565.0    B82 Piccolo Trumpet (cross-validated by B12 Violin)
0.370    622.0    A76 Dark Synth Piano (high)
0.520    925.0    A18 Piano I
0.638    1310.0   A14 Flutes (gold standard — no VCF, early/late agree within 3%)
0.850    11500.0  A58 Going Up (measured 13.5s, capped to fit spec)
1.000    12000.0  spec max
```

### Release (10 points)
```
Slider   ms       Source
0.000    2.0      spec min
0.016    12.0     A47 Funky I
0.087    12.0     B82 Piccolo Trumpet (high) — flat in low range
0.142    67.0     A14 Flutes (high)
0.236    105.0    A18 Piano I
0.252    175.0    A11 Brass
0.291    340.0    A12 Brass Swell
0.346    660.0    A16 Brass & Strings
0.669    2200.0   A84 Dust Storm
1.000    12000.0  spec max
```

## Known Issues and Gaps

### Decay 0.64–0.85 gap
Only one reliable point in this range (Flutes at 0.638→1310ms). Next anchor is Going Up at 0.850→11500ms — a ~9x jump. Every attempt to measure D≈0.70 has failed:
- B66 Toy Rhodes D=0.701→262ms (overlapping notes, bad measurement)
- B22 Nylon Guitar D=0.701→134ms (VCF confound extreme)
- A61 Piano II D=0.772→550ms tau (VCF-compressed)
- B54 Auto Release Noise Sweep D=0.622→1500ms (massive VCF confound)

The PCHIP interpolation handles this gap smoothly, but a clean measurement at D≈0.70 would significantly improve accuracy. Best candidates: any preset with D in 0.65–0.85, sustain=0, and minimal VCF envelope depth.

### VCF Confound
The Juno-106 shares one ADSR for both VCA and VCF. During decay/release, the VCF closing makes the sound die faster than the VCA alone. This means:
- Decay measurements are most reliable when sustain≈0 AND VCF envelope depth is low
- Release measurements may be compressed by VCF closure
- Many measurements in `measured_data.csv` are marked "low" confidence for this reason

### Sysex Corruption
5 of the 128 factory presets had zeroed volume in our sysex dump, suggesting data corruption. Other parameters may also have bit errors, which could explain some non-monotonic measurements.

### Attack Non-Monotonicity
A16 Brass & Strings (A=0.346→515ms) was excluded from anchors because B12 Violin (A=0.339→236ms, high confidence, no VCF) gives a much lower value at nearly the same slider position. The Brass & Strings attack may be inflated by VCF opening during the attack phase.

### Release Flat Region
Release is flat at ~12ms from slider 0 to ~0.087 (R=0→2ms, R=0.016→12ms, R=0.087→12ms). This is confirmed by high-confidence Piccolo Trumpet data.

## Measurement Methodology

Two approaches were used:

1. **Automated analysis prompt** — A standardized prompt was fed to Claude along with audio file data. It measures:
   - Attack: 10→90% rise time
   - Decay: exponential tau fit (time to drop to 1/e ≈ 37% of peak)
   - Release: exponential tau fit after key-up
   - VCF confound flag and confidence rating

2. **User visual analysis** — Looking at waveforms in an audio editor. For these:
   - Attack = time from silence to full volume
   - Decay = total audible decay time (divide by 3 to estimate tau)
   - Release = total tail length after note ends (divide by 3 to estimate tau)

### Tau ↔ Audible Time Conversion
- tau = time for signal to drop to ~37% (1/e) of starting value
- Signal drops to ~5% at 3×tau (effectively silent)
- So: total audible time ≈ 3 × tau

## Juno-6 vs Juno-106

The Juno-6 (1982) uses the IR3R01 analog envelope chip with:
- 100K(B) linear slider + 4.7K shunt resistor (creates aggressive reverse-log taper)
- TA75358S buffer op-amp
- 47nF timing capacitor
- Exponential-rate charge/discharge: time ∝ e^(-k×V)

The Juno-106 (1984) replaced this with an 80C49 microcontroller generating envelopes in software through DACs. The curves are fundamentally different — the Juno-6 model (`juno6_model.py`) gives values 100–900x different from Juno-106 measurements in the mid-range.

A separate Juno-6 ADSR mode is planned for the future, to be measured from real Juno-6 hardware.

## Preset ADSR Values

Raw slider values (0-1, derived from 7-bit sysex byte / 127) are in `adsr_overview.csv` and the project's `preset_adsr.csv`. The ms columns in `preset_adsr.csv` were computed from the OLD cubic formula and should be ignored — only the raw columns are meaningful.
