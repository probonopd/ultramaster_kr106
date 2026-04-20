# Ultramaster KR-106

A virtual analog synthesizer inspired by the Roland Juno polysynths, with DSP calibrated from hardware measurements, firmware analysis, and factory schematics. 6-voice polyphonic. Free and open source (GPL3).

Full documentation: https://kayrockscreenprinting.github.io/ultramaster_kr106/website/guide.html

## Installation

### macOS
- **AU:** Copy `Ultramaster KR-106.component` to `~/Library/Audio/Plug-Ins/Components/`
- **VST3:** Copy `Ultramaster KR-106.vst3` to `~/Library/Audio/Plug-Ins/VST3/`
- **CLAP:** Copy `Ultramaster KR-106.clap` to `~/Library/Audio/Plug-Ins/CLAP/`
- **LV2:** Copy `Ultramaster KR-106.lv2` to `~/Library/Audio/Plug-Ins/LV2/`
- **Standalone:** Move `Ultramaster KR-106.app` to `/Applications/`

### Windows
- **VST3:** Copy `Ultramaster KR-106.vst3` to `C:\Program Files\Common Files\VST3\`
- **CLAP:** Copy `Ultramaster KR-106.clap` to `C:\Program Files\Common Files\CLAP\`
- **LV2:** Copy `Ultramaster KR-106.lv2` to `C:\Program Files\Common Files\LV2\`
- **Standalone:** Run `Ultramaster KR-106.exe` from anywhere.

### Linux
- **VST3:** Copy `Ultramaster KR-106.vst3` to `~/.vst3/`
- **CLAP:** Copy `Ultramaster KR-106.clap` to `~/.clap/`
- **LV2:** Copy `Ultramaster KR-106.lv2` to `~/.lv2/`
- **Standalone:** Run `Ultramaster KR-106` from anywhere.

After copying, rescan plugins in your DAW.

## Controls

### Power & Tuning
- **Power** -- Turns the synth on and off.
- **Tuning** -- Fine-tune the master pitch.

### Performance
- **Master Volume** -- Output level. The red LED indicates clipping.
- **Portamento Rate** -- Glide speed between notes.
- **Portamento Mode** -- Off, Poly I (last-note priority), Poly II (low-note priority).

### Bender
- **Lever** -- Horizontal pitch bend. Push up to trigger LFO vibrato.
- **DCO / VCF / LFO** -- Sensitivity sliders for pitch, filter cutoff, and LFO depth.

### Arpeggiator
- **Transpose** -- When lit, clicking a key sets the transpose root. Click C to reset.
- **Hold** -- Notes sustain after release. Click a held key again to release it.
- **Arpeggio** -- Activates the arpeggiator.
- **Mode** -- Up, Up/Down, or Down.
- **Range** -- 1, 2, or 3 octaves.
- **Rate** -- Arpeggio speed.

### LFO
- **Rate** -- LFO speed.
- **Delay** -- Time before LFO fades in after a note.
- **Mode** -- Free-running or key-triggered.

### DCO (Digitally Controlled Oscillator)
- **LFO** -- Pitch modulation depth (vibrato).
- **PWM** -- Pulse width modulation depth.
- **PWM Mode** -- LFO, Manual, or Envelope.
- **Pulse / Saw / Sub** -- Toggle waveforms. Sub is one octave below.
- **Octave** -- 4', 8', or 16' range.
- **Sub Level** -- Sub-oscillator volume.
- **Noise** -- White noise level.

### HPF (High-Pass Filter)
- **Freq** -- Removes low-frequency content. 4-position switch in 1984 mode, continuous in 1982 mode.

### VCF (Voltage Controlled Filter)
- **Freq** -- Filter cutoff frequency.
- **Res** -- Resonance. Self-oscillates at maximum.
- **Env Polarity** -- Positive or inverted envelope modulation.
- **Env** -- Envelope modulation depth.
- **LFO** -- Filter cutoff modulation from LFO (wah).
- **KBD** -- Keyboard tracking.

### VCA (Voltage Controlled Amplifier)
- **Mode** -- Gate (organ-style) or Envelope (ADSR-shaped).
- **Level** -- Voice volume before chorus and master output.

### Envelope (ADSR)
- **Attack** -- Time to reach full level.
- **Decay** -- Time to fall to sustain level.
- **Sustain** -- Level held while key is down.
- **Release** -- Time to fade after key release.
- **ADSR Mode** -- Switch below the sliders. Left = 1982 (Juno-6 analog RC). Right = 1984 (Juno-106 firmware).

### Chorus
- **Off** -- Bypass.
- **I** -- Subtle, slow modulation.
- **II** -- Wider, faster modulation. I + II can be enabled together.

## Keyboard

Click the on-screen keyboard or use QWERTY:

| Keys | Notes |
|------|-------|
| Z X C V B N M | Lower octave naturals (C D E F G A B) |
| S D - G H J | Lower octave sharps |
| Q W E R T Y U | Upper octave naturals |
| 2 3 - 5 6 7 | Upper octave sharps |
| , . / - I O P [ | Extended notes |
| \` / 1 | Octave down / up |
| Up / Down | Previous / next preset |
| Enter | Open patch selector |

## Presets

128 factory presets derived from original Juno-106 patch data. Click the preset display to open the patch selector grid. Use the mouse wheel or arrow keys to step through presets.

Performance controls (Tuning, Transpose, Hold, Arpeggio, Arp Rate/Mode/Range) are not saved with presets.

## 1982 vs 1984 Mode

The horizontal switch below the ADSR sliders selects between Juno-6 (1982) and Juno-106 (1984) emulation:

- **1982:** Analog RC envelopes, continuous HPF sweep, analog VCF CV path.
- **1984:** Firmware-generated envelopes, 4-position HPF switch, DAC-based VCF control.

Both share the same IR3109 VCF, MN3009 BBD chorus, and DCO architecture, but the control paths differ significantly.

## Links

- Website: https://kayrockscreenprinting.github.io/ultramaster_kr106/website/
- Source: https://github.com/kayrockscreenprinting/ultramaster_kr106
- MIDI Mapping: https://kayrockscreenprinting.github.io/ultramaster_kr106/website/midi_mapping.html
