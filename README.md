# Ultramaster KR-106

[![Release](https://github.com/kayrockscreenprinting/ultramaster_kr106/actions/workflows/release.yml/badge.svg)](https://github.com/kayrockscreenprinting/ultramaster_kr106/actions/workflows/release.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A synthesizer plugin emulating the Roland Juno-6, Juno-60, and Juno-106, built with [JUCE](https://juce.com/).

![KR-106 Screenshot](docs/website/screenshot.png)

6-voice polyphonic with dual-mode DSP (Juno-60 analog / Juno-106 firmware), per-voice analog variance,
TPT ladder filter with OTA saturation, ngspice-verified BBD chorus, arpeggiator with DAW sync,
portamento, and 240 factory presets (112 Juno-60 + 128 Juno-106). Parameter curves calibrated from hardware
measurements, circuit simulation, and firmware analysis.

**Formats:** AU, VST3, LV2, CLAP, Standalone
**Platforms:** macOS (10.15+), Windows, Linux

**SysEx:** Full Juno-106 SysEx support (IPR/APR send and receive). A separate AU variant
(aumf) is included for Logic Pro, which blocks SysEx on standard AU instruments.

**[Download latest release](https://github.com/kayrockscreenprinting/ultramaster_kr106/releases/latest)**

See [docs/DSP_ARCHITECTURE.md](docs/DSP_ARCHITECTURE.md) for a detailed writeup of the
signal chain and emulation techniques.

## Building

### macOS

Requires **CMake** (3.22+) and **Xcode** (or Command Line Tools).

```bash
git clone --recursive https://github.com/kayrockscreenprinting/ultramaster_kr106.git
cd ultramaster_kr106
make build    # AU, VST3, Standalone
make run      # Build and launch Standalone
```

### Windows

Requires **CMake** (3.22+) and **Visual Studio 2022** (or Build Tools with C++ workload).

```bash
git clone --recursive https://github.com/kayrockscreenprinting/ultramaster_kr106.git
cd ultramaster_kr106
cmake -B build
cmake --build build --config Release
```

Plugins are output to `build/KR106_artefacts/Release/`.

### Linux

Requires **CMake** (3.22+) and a C++17 compiler.

```bash
git clone --recursive https://github.com/kayrockscreenprinting/ultramaster_kr106.git
cd ultramaster_kr106
make deps     # Install ALSA, X11, freetype, etc. (apt)
make build    # VST3, LV2, Standalone
```

For a release build:

```bash
CONFIG=Release make build
```

**Packagers:** If the build fails trying to copy plugins to `~/.lv2/` or similar
system directories, disable the post-build copy:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKR106_COPY_AFTER_BUILD=OFF
cmake --build build --config Release
```

Run `make help` for all available targets.

## Project Structure

```
Source/
  PluginProcessor.cpp/h        Audio processor, parameter setup, preset management
  PluginEditor.cpp/h           Custom GUI layout
  KR106_Presets_JUCE.h         240 factory presets (112 J60 + 128 J106)

  Controls/
    KR106Knob.h                Bitmap rotary knob (sprite sheet)
    KR106Slider.h              Pixel-perfect vertical fader
    KR106Switch.h              3-way toggle switch (vertical/horizontal)
    KR106Button.h              Momentary button with LED
    KR106Keyboard.h            On-screen keyboard with transpose
    KR106Scope.h               Oscilloscope with clickable vertical zoom
    KR106Bender.h              Pitch bend lever
    KR106Tooltip.h             Parameter value tooltip overlay

  DSP/
    KR106_DSP.h                Top-level DSP orchestrator, HPF, signal routing
    KR106_DSP_SetParam.h       Per-model parameter dispatch (J6/J60/J106 curves)
    KR106Voice.h               Per-voice: VCF, ADSR, oscillator mixing, portamento
    KR106OscillatorsWT.h       Bandlimited wavetable saw, pulse, sub
    KR106VCF.h                 TPT SVF ladder filter with OTA saturation model
    KR106Chorus.h              MN3009 BBD chorus with Hermite interpolation
    BBDFilter.h                Chorus pre/post filter (Butterworth biquad, ngspice-verified)
    KR106LFO.h                 Global triangle LFO with delay envelope
    KR106ADSR.h                Dual-mode ADSR (analog RC / firmware integer)
    KR106Arpeggiator.h         Note sequencer with DAW sync (Up / Down / Up-Down)
    KR106ParamValue.h          Unified parameter display (Hz, ms, dB, cents)
    KR106VcfFreqJ6.h           J6/J60 VCF frequency curves (circuit models)
    KR106VcfFreqJ106.h         J106 VCF frequency (firmware integer math)

docs/
  DSP_ARCHITECTURE.md          Detailed DSP design documentation
  HOLD_ARP_FLOW.md             Hold + arpeggiator interaction flow

tools/
  preset-gen/                  Original patch files and conversion utilities
  preset-midi/                 MIDI file generator for hardware A/B testing
  render-midi/                 Offline MIDI renderer (headless DSP)
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on submitting issues and pull requests.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
Third-party library licenses are listed in [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
