# KR-106 Tools

Standalone command-line tools for testing, calibrating, and rendering the KR-106 DSP engine. All C++ tools are header-only C++17 with no JUCE dependency -- they include the DSP headers directly.

## Building

Each directory has its own Makefile:

```bash
cd tools/vcf-analyze && make
cd tools/preset-midi && make
cd tools/render-midi && make
cd tools/vcf-sweep && make
cd tools/osc-compare && make
```

---

## render-midi/

Offline rendering and preset analysis.

| Tool | Description |
|------|-------------|
| **render_midi** | Plays a MIDI file through the full KR-106 DSP engine (6 voices, VCF, chorus, HPF) and writes a normalized 24-bit stereo WAV. Supports Note On/Off, Roland Juno-106 SysEx (IPR/APR), and tempo changes. |
| **peak_presets** | Renders all factory presets and reports peak levels. Used to calibrate the default master volume. |
| **vca_sweep** | Sweeps VCA level slider 0-1 and measures output amplitude. Verifies the dB-linear VCA law. |

```bash
./render_midi input.mid output.wav 44100
./peak_presets > peaks.csv
```

---

## vcf-analyze/

VCF calibration and hardware comparison pipeline.

### filter_test3 (primary tool)

Unified VCF test: generates MIDI, renders through DSP, or analyzes a hardware recording -- same test grid definition throughout.

**Test sequence (198s total, no gaps):**

| Section | Duration | Description |
|---------|----------|-------------|
| Preamble [0] | 2s | Noise, filter wide open, R=0 -- recording level calibration + noise spectrum baseline |
| Preamble [1] | 2s | Saw wave, filter wide open, R=0 -- harmonic rolloff reference |
| Preamble [2] | 2s | Self-oscillation at byte 64, R=max -- pitch reference for tuner calibration |
| Section 1 | 128s | 128 x 1s self-oscillation (R=max, VCF freq 0-127) -- maps slider to Hz |
| Section 2 | 32s | 16 x 2s noise at R=0 (freq bytes 0,8,16,...120) -- passband shape + rolloff |
| Section 3 | 32s | 16 x 2s noise at R=50 -- slope vs resonance comparison |

```bash
./filter_test3 gen output.mid          # Generate SysEx MIDI for hardware recording
./filter_test3 render output.wav [sr]  # Render through DSP + analyze (writes WAV + CSV to stdout)
./filter_test3 analyze input.wav       # Analyze a hardware WAV recording
./filter_test3 profile [sr]            # Full 128x128 (freq x res) internal sweep, CSV to stdout
```

The `profile` mode sweeps all 16384 freq/res combinations internally (no WAV output). Self-oscillation measured first at all 128 positions (R=max), then noise swept with note held continuously for stable filter state.

### Legacy analyzers

| Tool | Description |
|------|-------------|
| **gen_filter_test** | Generates MIDI for 9 cutoffs (C1-C9) x 11 resonance levels, 1s per step |
| **gen_filter_test2** | Generates MIDI matching ROM test mode 3 format: 6 res x 6 notes (C2-C7), noise + self-osc segments |
| **vcf_analyze** | Analyzes rendered WAV from gen_filter_test. Reports -3dB, passband, peak, slope. CSV output |
| **vcf_analyze_hw** | Analyzes hardware recordings in ROM test mode 3 format. Reports -3dB/-6dB/-12dB/-24dB thresholds and slope |
| **vcf_analyze_j6** | Variant of vcf_analyze_hw for Juno-6 manual recordings (6s blocks, no gaps) |

### Hardware recordings

| File | Source | Description |
|------|--------|-------------|
| J106_c2-to-c7.wav | Juno-106 (original Roland voice chips) | ROM test mode 3: 6 res x 6 notes |
| lewis_francis_filter_test2.wav | Juno-106 (replacement voice chips) | Same test, different unit |
| j6_c2-to-c7.wav | Juno-6 (manual recording) | 6 res x 6 notes, 3s self-osc + 3s noise, no gaps |

---

## preset-midi/

SysEx MIDI file generator for loading presets into hardware or plugin.

**gen_preset_midi** -- Generates MIDI files that load a Juno-106 patch via SysEx (0x31 manual mode) and play test notes. For A/B comparison between hardware and plugin.

```bash
./gen_preset_midi 0            # single preset (A11 Brass)
./gen_preset_midi all outdir/  # 256 individual files (128 factory + 128 user bank)
./gen_preset_midi batch outdir/ # 16 bank files (8 presets each)
```

Each file sends a full APR SysEx (0x31, manual mode -- loads parameters directly rather than recalling a stored patch), then plays a C major chord with full attack/release envelope timing, followed by a C2-C6 keyboard survey.

---

## vcf-sweep/

Standalone VCF unit tests. Each generates a 24-bit mono WAV or CSV. Tests the VCF in isolation without the full signal chain.

| Tool | Description |
|------|-------------|
| **vcf_sweep** | Continuous cutoff sweep 0-1 over 10s. Optional noise input and resonance. `./vcf_sweep out.wav 10 44100 j106 1.0 1.0` |
| **vcf_noise** | White noise through VCF at 6 frequencies x 6 resonance steps, 2s each |
| **vcf_quick** | Self-oscillation pitch + amplitude at 9 frequencies. CSV output |
| **vcf_res_sweep** | White noise at fixed cutoff, resonance swept 0-1 |
| **vcf_thd** | Self-oscillation THD measurement via Goertzel DFT. CSV output |
| **vcf_peaks** | Saw passthrough peak vs self-oscillation peak at 8 octaves |
| **vcf_cutoff** | Measures actual -3dB cutoff vs target at 10 frequencies x 6 resonance levels |

---

## osc-compare/

**osc_compare** -- Compares polyBLEP and wavetable oscillator implementations. Renders both at multiple frequencies and reports aliasing levels.

---

## preset-gen/

Factory preset conversion utilities. Converts original Juno-106 patch data (.pat files, raw SysEx dumps) to the C++ preset arrays in `KR106_Presets_JUCE.h`.

| Tool | Description |
|------|-------------|
| **gen_presets.py** | Main converter: reads lib.json + Factory_Patches.pat, writes C++ header |
| **convert.C** | Low-level patch format converter |
| **copy_patch.c** | Copies individual patches between banks |
| **list_patches.c** | Lists patch names from a .pat file |
| **kr106_patch.c/h** | Patch data structure and I/O routines |

---

## adsr-calibration/

ADSR envelope timing measurements and curve fitting from Juno-6 hardware.

| Tool | Description |
|------|-------------|
| **juno6_model.py** | Juno-6 ADSR circuit simulation (IR3R01 RC model) |
| **fit_slider_to_ms.py** | Fits measured attack/decay/release times to slider curves |
| **fit_sysex_to_slider.py** | Maps SysEx byte values to slider positions |
| **generate_lut.py** | Generates lookup tables from fitted curves |

Data files: `measured_data.csv` (hardware measurements), `adsr_overview.csv` (summary), `preset_adsr_sliders.csv` (factory preset ADSR values).

---

## wasm/

WebAssembly build of the DSP engine for the browser-based preview on the website.

| File | Description |
|------|-------------|
| **kr106_wasm.cpp** | WASM wrapper around KR106DSP -- exposes NoteOn/NoteOff/SetParam/ProcessBlock |
| **kr106-processor.js** | AudioWorklet processor that loads the WASM module |
| **Makefile** | Builds with emscripten (em++) |
