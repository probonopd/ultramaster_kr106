// Generate a MIDI file for VCF filter analysis matching ROM test mode 3.
//
// Setup: ROM calibration patch ($0FA0) -- VCF freq=49, VCF env=0,
// VCF kbd=127, VCA gate mode, no oscillators, no chorus. Cutoff
// frequency is set by VCF freq slider + keyboard tracking from the
// note played. No-noise steps test self-oscillation; noise-on steps
// give the filter frequency response.
//
// Resonances: 0, 25, 50, 76, 101, 127
// Notes: C2(36), C3(48), C4(60), C5(72), C6(84), C7(96)
//
// Usage: gen_filter_test2 [output.mid]
//   Default: filter_test2.mid

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

// --- MIDI file helpers ---

static void writeVarLen(std::vector<uint8_t>& buf, uint32_t val)
{
    uint8_t bytes[4];
    int n = 0;
    bytes[n++] = val & 0x7F;
    while (val >>= 7)
        bytes[n++] = (val & 0x7F) | 0x80;
    for (int i = n - 1; i >= 0; i--)
        buf.push_back(bytes[i]);
}

static void write16(std::vector<uint8_t>& buf, uint16_t val)
{
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

static void write32(std::vector<uint8_t>& buf, uint32_t val)
{
    buf.push_back((val >> 24) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

static void addSysEx(std::vector<uint8_t>& track, uint32_t delta,
                     uint8_t ctrl, uint8_t val)
{
    writeVarLen(track, delta);
    track.push_back(0xF0);
    writeVarLen(track, 6);
    track.push_back(0x41);
    track.push_back(0x32);
    track.push_back(0x00);
    track.push_back(ctrl);
    track.push_back(val);
    track.push_back(0xF7);
}

static void addNoteOn(std::vector<uint8_t>& track, uint32_t delta,
                      uint8_t note, uint8_t vel)
{
    writeVarLen(track, delta);
    track.push_back(0x90);
    track.push_back(note);
    track.push_back(vel);
}

static void addNoteOff(std::vector<uint8_t>& track, uint32_t delta,
                       uint8_t note)
{
    writeVarLen(track, delta);
    track.push_back(0x80);
    track.push_back(note);
    track.push_back(0x00);
}

static void addTempo(std::vector<uint8_t>& track, uint32_t usPerBeat)
{
    writeVarLen(track, 0);
    track.push_back(0xFF);
    track.push_back(0x51);
    track.push_back(0x03);
    track.push_back((usPerBeat >> 16) & 0xFF);
    track.push_back((usPerBeat >> 8) & 0xFF);
    track.push_back(usPerBeat & 0xFF);
}

static void addEndOfTrack(std::vector<uint8_t>& track)
{
    writeVarLen(track, 0);
    track.push_back(0xFF);
    track.push_back(0x2F);
    track.push_back(0x00);
}

// SysEx CC mapping:
// 0x00 = LFO Rate,  0x01 = LFO Delay, 0x02 = DCO LFO, 0x03 = DCO PWM
// 0x04 = DCO Noise, 0x05 = VCF Freq,  0x06 = VCF Res,  0x07 = VCF Env
// 0x08 = VCF LFO,   0x09 = VCF KBD,   0x0A = VCA Level, 0x0B = Env A
// 0x0C = Env D,     0x0D = Env S,      0x0E = Env R,    0x0F = DCO Sub
// 0x10 = Switches 1, 0x11 = Switches 2

int main(int argc, char* argv[])
{
    const char* outFile = (argc > 1) ? argv[1] : "filter_test2.mid";

    // At 120 BPM, 480 ticks = 0.5s
    static constexpr uint32_t kHoldTicks = 480 * 6;   // 3s
    static constexpr uint32_t kGapTicks = 480;         // 0.5s

    std::vector<uint8_t> track;
    addTempo(track, 500000); // 120 BPM

    // --- Patch setup (matches ROM test mode 3 at $0FA0) ---
    // ROM bytes (bit 7 stripped): 40 00 00 00 00 31 7F 00 00 7F 40 00 00 7F 00 00
    // Switches: 0x22 (8', no osc, chorus off), 0x10 (HPF=1, gate, ENV+)
    addSysEx(track, 0, 0x00, 64);   // LFO Rate = 64 (moderate)
    addSysEx(track, 5, 0x01, 0);    // LFO Delay = 0
    addSysEx(track, 5, 0x02, 0);    // DCO LFO = 0
    addSysEx(track, 5, 0x03, 0);    // DCO PWM = 0
    addSysEx(track, 5, 0x04, 0);    // DCO Noise = 0 (toggled per step)
    addSysEx(track, 5, 0x05, 49);   // VCF Freq = 49 (0x31, ROM calibration)
    addSysEx(track, 5, 0x06, 127);  // VCF Res = 127 (overridden per step)
    addSysEx(track, 5, 0x07, 0);    // VCF Env = 0 (none)
    addSysEx(track, 5, 0x08, 0);    // VCF LFO = 0
    addSysEx(track, 5, 0x09, 127);  // VCF KBD = 127 (max tracking)
    addSysEx(track, 5, 0x0A, 64);   // VCA Level = 64 (moderate)
    addSysEx(track, 5, 0x0B, 0);    // Env A = 0 (instant)
    addSysEx(track, 5, 0x0C, 0);    // Env D = 0
    addSysEx(track, 5, 0x0D, 127);  // Env S = 127 (max)
    addSysEx(track, 5, 0x0E, 0);    // Env R = 0
    addSysEx(track, 5, 0x0F, 0);    // DCO Sub = 0
    // Switches 1: 0x22 = 8', no pulse, no saw, chorus off
    addSysEx(track, 5, 0x10, 0x22);
    // Switches 2: 0x10 = MAN PWM, ENV+ positive, GATE VCA, HPF=1
    addSysEx(track, 5, 0x11, 0x10);

    // Settle
    uint32_t settleTicks = 480; // 0.5s

    // --- Test grid ---
    uint8_t resValues[] = {0, 25, 50, 76, 101, 127};
    int nRes = 6;

    uint8_t notes[] = {36, 48, 60, 72, 84, 96}; // C2-C7
    const char* noteNames[] = {"C2", "C3", "C4", "C5", "C6", "C7"};
    int nNotes = 6;

    bool first = true;
    int stepCount = 0;

    for (int r = 0; r < nRes; r++)
    {
        // Set resonance
        addSysEx(track, first ? settleTicks : kGapTicks, 0x06, resValues[r]);
        first = false;

        for (int n = 0; n < nNotes; n++)
        {
            // --- Without noise (pulse only) ---
            addSysEx(track, kGapTicks, 0x04, 0);   // noise off
            addNoteOn(track, 10, notes[n], 100);
            addNoteOff(track, kHoldTicks, notes[n]);
            stepCount++;

            // --- With noise ---
            addSysEx(track, kGapTicks, 0x04, 127);  // noise on
            addNoteOn(track, 10, notes[n], 100);
            addNoteOff(track, kHoldTicks, notes[n]);
            stepCount++;
        }
    }

    // Final gap
    addEndOfTrack(track);

    // Write MIDI file
    std::vector<uint8_t> midi;
    midi.push_back('M'); midi.push_back('T'); midi.push_back('h'); midi.push_back('d');
    write32(midi, 6);
    write16(midi, 0);
    write16(midi, 1);
    write16(midi, 480);
    midi.push_back('M'); midi.push_back('T'); midi.push_back('r'); midi.push_back('k');
    write32(midi, static_cast<uint32_t>(track.size()));
    midi.insert(midi.end(), track.begin(), track.end());

    FILE* file = fopen(outFile, "wb");
    if (!file) { fprintf(stderr, "Error: cannot open %s\n", outFile); return 1; }
    fwrite(midi.data(), 1, midi.size(), file);
    fclose(file);

    float totalSec = stepCount * 3.5f + nRes * 0.5f + 0.5f;
    fprintf(stderr, "Wrote %s\n", outFile);
    fprintf(stderr, "  %d resonances x %d notes x 2 (no-noise/noise) = %d steps\n",
            nRes, nNotes, stepCount);
    fprintf(stderr, "  3s hold + 0.5s gap per step, ~%.0fs total\n", totalSec);
    fprintf(stderr, "  Resonance values: ");
    for (int r = 0; r < nRes; r++)
        fprintf(stderr, "%d%s", resValues[r], r < nRes-1 ? ", " : "\n");
    fprintf(stderr, "  Notes: ");
    for (int n = 0; n < nNotes; n++)
        fprintf(stderr, "%s(%d)%s", noteNames[n], notes[n], n < nNotes-1 ? ", " : "\n");
    fprintf(stderr, "  ROM test mode 3: VCF freq=49, env=0, kbd=127, gate VCA, no osc\n");

    return 0;
}
