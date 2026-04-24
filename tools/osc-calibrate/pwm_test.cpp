// pwm_test -- Generate MIDI for pulse width calibration on Juno-106.
//
// Section 1: Manual PWM sweep -- 106 blocks (byte 0-105), 0.5s each.
//   Pulse on, no saw/sub/noise, PWM=manual, VCF wide open, R=0, HPF flat.
//   Measures static duty cycle at each PWM byte value.
//
// Section 2: LFO PWM at selected depths -- 6 blocks, 4s each.
//   PWM=LFO, LFO rate at a slow setting so we capture full cycles.
//   PWM depth at bytes 0, 21, 42, 63, 84, 105.
//   Measures modulated duty cycle range vs depth.
//
// Note: G3 (MIDI 55, 196 Hz) = 490 samples/cycle at 96 kHz.
//
// USAGE:
//   pwm_test gen output.mid
//   pwm_test analyze input.wav|.aif    (future)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <cmath>

// ============================================================
// MIDI helpers (same as osc_calibrate)
// ============================================================

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

// APR (manual mode): F0 41 31 00 00 [16 sliders] [sw1] [sw2] F7
static void addAPR(std::vector<uint8_t>& track, uint32_t delta,
                   const uint8_t sliders[16], uint8_t sw1, uint8_t sw2)
{
    writeVarLen(track, delta);
    track.push_back(0xF0);
    writeVarLen(track, 23);
    track.push_back(0x41);
    track.push_back(0x31);
    track.push_back(0x00);
    track.push_back(0x00);
    for (int i = 0; i < 16; i++)
        track.push_back(sliders[i]);
    track.push_back(sw1);
    track.push_back(sw2);
    track.push_back(0xF7);
}

static void writeMidi(const char* filename, const std::vector<uint8_t>& track,
                      uint16_t ticksPerBeat, float totalSec)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return; }

    std::vector<uint8_t> hdr;
    hdr.push_back('M'); hdr.push_back('T'); hdr.push_back('h'); hdr.push_back('d');
    write32(hdr, 6);
    write16(hdr, 0);
    write16(hdr, 1);
    write16(hdr, ticksPerBeat);
    fwrite(hdr.data(), 1, hdr.size(), f);

    std::vector<uint8_t> trkHdr;
    trkHdr.push_back('M'); trkHdr.push_back('T'); trkHdr.push_back('r'); trkHdr.push_back('k');
    write32(trkHdr, static_cast<uint32_t>(track.size()));
    fwrite(trkHdr.data(), 1, trkHdr.size(), f);
    fwrite(track.data(), 1, track.size(), f);

    fclose(f);
    fprintf(stderr, "Wrote %s (%.0fs)\n", filename, totalSec);
}

// ============================================================
// gen
// ============================================================

static constexpr uint16_t kTPB = 480;
static constexpr uint32_t kTempo = 500000; // 120 BPM
// At 120 BPM, 480 TPB: 1 beat = 0.5s = 480 ticks
static constexpr uint32_t k250msTicks = 240;    // 0.25s
static constexpr uint32_t k4sTicks = 480 * 8;   // 4.0s
static constexpr uint32_t kSettleTicks = 72;     // ~75ms settle

static constexpr int kNote = 55; // G3 = 196 Hz

static void doGen(const char* outFile)
{
    std::vector<uint8_t> track;
    addTempo(track, kTempo);

    // Base patch: pulse only, VCF wide open, R=0, HPF flat, VCA gate,
    // sustain max, attack/decay/release=0, no env/lfo/kbd on VCF
    // SysEx order: LfoRate LfoDelay DcoLfo DcoPwm Noise VcfFreq VcfRes
    //              VcfEnv VcfLfo VcfKbd VcaLevel EnvA EnvD EnvS EnvR Sub
    uint8_t base[16] = {
        0,   // 0x00 LFO Rate
        0,   // 0x01 LFO Delay
        0,   // 0x02 DCO LFO
        0,   // 0x03 DCO PWM (swept)
        0,   // 0x04 Noise = 0
        127, // 0x05 VCF Freq = max (wide open)
        0,   // 0x06 VCF Res = 0
        0,   // 0x07 VCF Env = 0
        0,   // 0x08 VCF LFO = 0
        0,   // 0x09 VCF KBD = 0
        64,  // 0x0A VCA Level
        0,   // 0x0B Env A = 0 (instant)
        0,   // 0x0C Env D = 0
        127, // 0x0D Env S = max
        0,   // 0x0E Env R = 0
        0    // 0x0F Sub = 0
    };
    // SW1: pulse on, no saw, 16', chorus off
    uint8_t sw1 = 0x01 | 0x08 | 0x20; // oct 16' + pulse + chorus off
    // SW2: PWM manual + VCA gate + HPF flat (pos 1: (3-1)<<3 = 0x10)
    uint8_t sw2_manual = 0x01 | 0x04 | 0x10; // PWM manual + VCA gate + HPF flat
    uint8_t sw2_lfo    = 0x00 | 0x04 | 0x10; // PWM LFO + VCA gate + HPF flat

    float totalSec = 0.f;
    static constexpr uint32_t k1sTicks = 480 * 2; // 1.0s at 120 BPM

    // === Preamble: Waveform references, 1s each ===
    // All at PWM=0 (50% duty), sub=max, noise=max where applicable
    fprintf(stderr, "Preamble: 7 waveform references (7s)\n");
    struct WaveRef {
        const char* name;
        uint8_t sw1Bits;    // pulse | saw bits (OR'd with 0x01 oct + 0x20 chorus off)
        uint8_t subLevel;   // 0x0F: sub osc level
        uint8_t noiseLevel; // 0x04: noise level
        uint8_t pwmByte;    // 0x03: PWM slider
    };
    static constexpr uint8_t kSw1Base = 0x01 | 0x20; // oct 16' + chorus off
    WaveRef refs[] = {
        { "Saw",           0x10, 0,   0,   0 },   // saw only
        { "Pulse",         0x08, 0,   0,   0 },   // pulse 50%
        { "Sub",           0x00, 127, 0,   0 },   // sub only
        { "Noise",         0x00, 0,   127, 0 },   // noise only
        { "Saw+Pulse",     0x18, 0,   0,   0 },   // saw + pulse 50%
        { "Saw+Sub",       0x10, 127, 0,   0 },   // saw + sub
        { "Saw+Pulse+Sub", 0x18, 127, 0,   0 },   // saw + pulse + sub
    };
    for (auto& ref : refs)
    {
        uint8_t s[16]; memcpy(s, base, 16);
        s[0x03] = ref.pwmByte;
        s[0x04] = ref.noiseLevel;
        s[0x0F] = ref.subLevel;
        addAPR(track, 10, s, kSw1Base | ref.sw1Bits, sw2_manual);
        addNoteOn(track, kSettleTicks - 10, kNote, 127);
        addNoteOff(track, k1sTicks - kSettleTicks, kNote);
        totalSec += 1.0f;
    }

    // === Section 1: Manual PWM sweep, bytes 0-105 ===
    fprintf(stderr, "Section 1: Manual PWM sweep (106 blocks x 0.25s = 26.5s)\n");
    for (int b = 0; b <= 105; b++)
    {
        uint8_t s[16]; memcpy(s, base, 16);
        s[0x03] = static_cast<uint8_t>(b);
        addAPR(track, 10, s, sw1, sw2_manual);  // 10 ticks after note-off for VCA close
        addNoteOn(track, kSettleTicks - 10, kNote, 127);
        addNoteOff(track, k250msTicks - kSettleTicks, kNote);
        totalSec += 0.25f;
    }

    // === Section 2: LFO PWM at 6 depth settings, 4s each ===
    // LFO rate: byte 30 (~1.5 Hz, gives ~2-3 full cycles in 4s)
    static constexpr int kLfoRate = 30;
    static constexpr int kLfoDepths[] = { 0, 21, 42, 63, 84, 105 };
    static constexpr int kNumLfoTests = 6;

    fprintf(stderr, "Section 2: LFO PWM at 6 depths (6 blocks x 4s = 24s)\n");
    for (int d = 0; d < kNumLfoTests; d++)
    {
        uint8_t s[16]; memcpy(s, base, 16);
        s[0x00] = kLfoRate;  // LFO rate
        s[0x03] = static_cast<uint8_t>(kLfoDepths[d]); // PWM depth
        addAPR(track, 10, s, sw1, sw2_lfo);
        addNoteOn(track, kSettleTicks - 10, kNote, 127);
        addNoteOff(track, k4sTicks - kSettleTicks, kNote);
        totalSec += 4.0f;
    }

    addEndOfTrack(track);
    writeMidi(outFile, track, kTPB, totalSec);
    fprintf(stderr, "Total: %.0fs (%.1f min)\n", totalSec, totalSec / 60.f);
    fprintf(stderr, "Note: G3 (MIDI 55, 196 Hz), record at 96 kHz\n");
}

// ============================================================
// Audio I/O (from osc_calibrate)
// ============================================================

struct AudioData {
    int sampleRate;
    int channels;
    int numSamples;
    std::vector<float> data;
};

static uint16_t readBE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static uint32_t readBE32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static double readExtended80(const uint8_t* p)
{
    int sign = (p[0] >> 7) & 1;
    int exp = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++)
        mantissa = (mantissa << 8) | p[2 + i];
    if (exp == 0 && mantissa == 0) return 0.0;
    double val = static_cast<double>(mantissa) / (1ULL << 63) * pow(2.0, exp - 16383);
    return sign ? -val : val;
}

static bool readWav(const char* filename, AudioData& out)
{
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_SET);
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) < 12) { fclose(f); return false; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    { fclose(f); return false; }

    int fmt_channels = 0, fmt_sr = 0, fmt_bits = 0;
    bool foundFmt = false, foundData = false;
    long dataStart = 0;
    uint32_t dataSize = 0;
    uint8_t chdr[8];

    while (fread(chdr, 1, 8, f) == 8)
    {
        uint32_t ckSize = chdr[4] | (chdr[5] << 8) | (chdr[6] << 16) | (chdr[7] << 24);
        if (memcmp(chdr, "fmt ", 4) == 0)
        {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) < 16) { fclose(f); return false; }
            fmt_channels = fmt[2] | (fmt[3] << 8);
            fmt_sr = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            fmt_bits = fmt[14] | (fmt[15] << 8);
            fseek(f, static_cast<long>(ckSize) - 16, SEEK_CUR);
            foundFmt = true;
        }
        else if (memcmp(chdr, "data", 4) == 0)
        {
            dataStart = ftell(f);
            dataSize = ckSize;
            foundData = true;
            break;
        }
        else
            fseek(f, ckSize + (ckSize & 1), SEEK_CUR);
    }
    if (!foundFmt || !foundData || (fmt_bits != 16 && fmt_bits != 24))
    { fclose(f); return false; }

    int bytesPerSample = fmt_bits / 8;
    int numSamples = static_cast<int>(dataSize / (bytesPerSample * fmt_channels));
    out.sampleRate = fmt_sr;
    out.channels = fmt_channels;
    out.numSamples = numSamples;
    out.data.resize(numSamples * fmt_channels);
    fseek(f, dataStart, SEEK_SET);
    size_t total = static_cast<size_t>(numSamples) * fmt_channels;
    std::vector<uint8_t> raw(total * bytesPerSample);
    fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    for (size_t i = 0; i < total; i++)
    {
        if (fmt_bits == 16) {
            int16_t val = static_cast<int16_t>(raw[i*2] | (raw[i*2+1] << 8));
            out.data[i] = val / 32768.f;
        } else {
            int32_t val = raw[i*3] | (raw[i*3+1] << 8) | (raw[i*3+2] << 16);
            if (val & 0x800000) val |= static_cast<int32_t>(0xFF000000);
            out.data[i] = val / 8388608.f;
        }
    }
    return true;
}

static bool readAiff(const char* filename, AudioData& out)
{
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    uint8_t formHdr[12];
    if (fread(formHdr, 1, 12, f) < 12) { fclose(f); return false; }
    bool isAIFC = memcmp(formHdr + 8, "AIFC", 4) == 0;
    if (memcmp(formHdr, "FORM", 4) != 0 ||
        (memcmp(formHdr + 8, "AIFF", 4) != 0 && !isAIFC))
    { fclose(f); return false; }

    int aiff_channels = 0, aiff_bits = 0;
    uint32_t aiff_frames = 0;
    double aiff_sr = 0;
    long dataStart = 0;
    bool foundComm = false, foundData = false;
    uint8_t chdr[8];

    while (fread(chdr, 1, 8, f) == 8)
    {
        uint32_t ckSize = readBE32(chdr + 4);
        long ckStart = ftell(f);
        if (memcmp(chdr, "COMM", 4) == 0) {
            uint8_t comm[26];
            fread(comm, 1, std::min(ckSize, 26u), f);
            aiff_channels = readBE16(comm);
            aiff_frames = readBE32(comm + 2);
            aiff_bits = readBE16(comm + 6);
            aiff_sr = readExtended80(comm + 8);
            foundComm = true;
        } else if (memcmp(chdr, "SSND", 4) == 0) {
            uint8_t ssnd[8];
            fread(ssnd, 1, 8, f);
            dataStart = ftell(f) + readBE32(ssnd);
            foundData = true;
        }
        fseek(f, ckStart + ckSize + (ckSize & 1), SEEK_SET);
    }
    if (!foundComm || !foundData || (aiff_bits != 16 && aiff_bits != 24))
    { fclose(f); return false; }

    int bytesPerSample = aiff_bits / 8;
    int numSamples = static_cast<int>(aiff_frames);
    out.sampleRate = static_cast<int>(aiff_sr);
    out.channels = aiff_channels;
    out.numSamples = numSamples;
    out.data.resize(numSamples * aiff_channels);
    fseek(f, dataStart, SEEK_SET);
    size_t total = static_cast<size_t>(numSamples) * aiff_channels;
    std::vector<uint8_t> raw(total * bytesPerSample);
    fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    for (size_t i = 0; i < total; i++) {
        if (aiff_bits == 16) {
            int16_t val = static_cast<int16_t>((raw[i*2] << 8) | raw[i*2+1]);
            out.data[i] = val / 32768.f;
        } else {
            int32_t val = (raw[i*3] << 16) | (raw[i*3+1] << 8) | raw[i*3+2];
            if (val & 0x800000) val |= static_cast<int32_t>(0xFF000000);
            out.data[i] = val / 8388608.f;
        }
    }
    return true;
}

static bool readAudio(const char* filename, AudioData& out)
{
    std::string name(filename);
    auto endsWith = [&](const char* suffix) {
        std::string s(suffix);
        if (name.size() < s.size()) return false;
        std::string tail = name.substr(name.size() - s.size());
        for (auto& c : tail) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return tail == s;
    };
    if (endsWith(".aif") || endsWith(".aiff"))
        return readAiff(filename, out);
    return readWav(filename, out);
}

// ============================================================
// Analysis helpers
// ============================================================

// Measure +peak, -peak, and RMS of a block.
// Skips the first 1000 samples to avoid startup transients.
struct PeakInfo {
    float peakPos;   // max positive sample
    float peakNeg;   // min negative sample (stored as positive magnitude)
    float rms;
};

static PeakInfo measurePeaks(const float* mono, int start, int len)
{
    PeakInfo p = { 0.f, 0.f, 0.f };
    int skip = std::min(1000, len / 4);
    double sumSq = 0.0;
    int count = 0;
    for (int i = skip; i < len; i++)
    {
        float s = mono[start + i];
        if (s > p.peakPos) p.peakPos = s;
        if (s < -p.peakNeg) p.peakNeg = -s; // store as positive
        sumSq += s * s;
        count++;
    }
    if (count > 0) p.rms = sqrtf(static_cast<float>(sumSq / count));
    return p;
}

// Compute the midpoint threshold for a pulse wave block.
// Uses the mean of the upper and lower plateaus (robust against DC offset).
static float pulseThreshold(const float* mono, int start, int len)
{
    // Find the high and low plateau levels using 1st/99th percentiles.
    // This handles duty cycles up to 99% (narrow portion = 1% of cycle).
    // More robust than raw min/max against edge transients.
    std::vector<float> sorted(len);
    for (int i = 0; i < len; i++) sorted[i] = mono[start + i];
    std::sort(sorted.begin(), sorted.end());
    int loIdx = std::max(1, len / 100);
    int hiIdx = std::min(len - 2, len - len / 100 - 1);
    float lo = sorted[loIdx];
    float hi = sorted[hiIdx];
    return (lo + hi) * 0.5f;
}

// ============================================================
// analyze
// ============================================================

static void doAnalyze(const char* inputFile)
{
    AudioData audio;
    if (!readAudio(inputFile, audio))
    {
        fprintf(stderr, "Error: cannot read %s\n", inputFile);
        return;
    }

    int sr = audio.sampleRate;
    fprintf(stderr, "Read %s: %d Hz, %d ch, %.1fs\n",
            inputFile, sr, audio.channels,
            static_cast<float>(audio.numSamples) / sr);

    // Mix to mono
    std::vector<float> mono(audio.numSamples);
    if (audio.channels == 1)
        mono = audio.data;
    else
    {
        for (int i = 0; i < audio.numSamples; i++)
        {
            float sum = 0;
            for (int c = 0; c < audio.channels; c++)
                sum += audio.data[i * audio.channels + c];
            mono[i] = sum / audio.channels;
        }
    }

    // Block timing (must match gen)
    float settleSec = 75.f / 1000.f; // 75ms settle
    int settleSamples = static_cast<int>(settleSec * sr);

    // === Preamble: 7 x 1s waveform references ===
    static constexpr int kNumRefs = 7;
    static const char* refNames[] = {
        "Saw", "Pulse", "Sub", "Noise",
        "Saw+Pulse", "Saw+Sub", "Saw+Pulse+Sub"
    };
    int blockSamples_1s = sr; // 1.0s
    int soundingSamples_1s = blockSamples_1s - settleSamples;

    fprintf(stderr, "\n=== Preamble: Waveform references ===\n");
    printf("section,label,peak_pos,peak_neg,rms,rms_dB\n");

    float sawRms = 0.f;
    int blockPos = 0;
    for (int r = 0; r < kNumRefs; r++)
    {
        int start = blockPos + settleSamples;
        if (start + soundingSamples_1s > audio.numSamples) break;
        auto pk = measurePeaks(mono.data(), start, soundingSamples_1s);
        float dB = (pk.rms > 0) ? 20.f * log10f(pk.rms) : -999.f;
        if (r == 0) sawRms = pk.rms;
        float relDb = (sawRms > 0 && pk.rms > 0) ? 20.f * log10f(pk.rms / sawRms) : -999.f;

        printf("preamble,%s,%.6f,%.6f,%.6f,%.1f\n",
               refNames[r], pk.peakPos, pk.peakNeg, pk.rms, dB);
        fprintf(stderr, "  %-16s +pk=%.4f  -pk=%.4f  rms=%.4f  %+.1f dB  (%+.1f vs saw)\n",
                refNames[r], pk.peakPos, pk.peakNeg, pk.rms, dB, relDb);
        blockPos += blockSamples_1s;
    }

    // === Section 1: Manual PWM sweep, 106 x 0.5s ===
    int blockSamples_250ms = sr / 4; // 0.25s
    int soundingSamples_250ms = blockSamples_250ms - settleSamples;

    fprintf(stderr, "\n=== Section 1: Manual PWM sweep ===\n");
    printf("section,byte,cycles,mean_duty_pct,min_duty_pct,max_duty_pct,peak_pos,peak_neg,rms\n");

    for (int b = 0; b <= 105; b++)
    {
        int start = blockPos + settleSamples;
        if (start + soundingSamples_250ms > audio.numSamples)
        {
            fprintf(stderr, "  Warning: recording too short at byte %d\n", b);
            break;
        }
        auto pk = measurePeaks(mono.data(), start, soundingSamples_250ms);

        // Per-cycle duty measurement using adaptive threshold
        const float* m = mono.data() + start;
        int len = soundingSamples_250ms;
        float thresh = pulseThreshold(m, 0, len);
        int cycle = 0;
        int positiveSamples = 0;
        int cycleSamples = 0;
        bool wasAbove = m[0] > thresh;
        bool hadFirstCrossing = false;
        float minD = 1.f, maxD = 0.f;
        double sum = 0.0;

        for (int i = 1; i < len; i++)
        {
            bool isAbove = m[i] > thresh;
            cycleSamples++;
            if (isAbove) positiveSamples++;

            // Positive-going threshold crossing = cycle boundary
            if (isAbove && !wasAbove)
            {
                if (hadFirstCrossing && cycleSamples > 10)
                {
                    float duty = static_cast<float>(positiveSamples) / static_cast<float>(cycleSamples);
                    minD = std::min(minD, duty);
                    maxD = std::max(maxD, duty);
                    sum += duty;
                    cycle++;
                }
                hadFirstCrossing = true;
                positiveSamples = 0;
                cycleSamples = 0;
            }
            wasAbove = isAbove;
        }

        float meanD = (cycle > 0) ? static_cast<float>(sum / cycle) * 100.f : 0.f;
        printf("manual,%d,%d,%.2f,%.2f,%.2f,%.6f,%.6f,%.6f\n",
               b, cycle, meanD, minD * 100.f, maxD * 100.f, pk.peakPos, pk.peakNeg, pk.rms);
        if (b % 10 == 0 || b == 105)
            fprintf(stderr, "  byte %3d: %2d cycles, duty=%.2f%% [%.2f-%.2f]  +pk=%.4f  -pk=%.4f  rms=%.4f\n",
                    b, cycle, meanD, minD * 100.f, maxD * 100.f, pk.peakPos, pk.peakNeg, pk.rms);

        blockPos += blockSamples_250ms;
    }

    // === Section 2: LFO PWM at 6 depths, 6 x 4s ===
    static constexpr int kLfoDepths[] = { 0, 21, 42, 63, 84, 105 };
    static constexpr int kNumLfoTests = 6;
    int blockSamples_4s = sr * 4; // 4.0s
    int soundingSamples_4s = blockSamples_4s - settleSamples;

    fprintf(stderr, "\n=== Section 2: LFO PWM ===\n");
    printf("section,depth_byte,cycle,duty_pct\n");

    for (int d = 0; d < kNumLfoTests; d++)
    {
        int start = blockPos + settleSamples;
        if (start + soundingSamples_4s > audio.numSamples)
        {
            fprintf(stderr, "  Warning: recording too short at depth %d\n", kLfoDepths[d]);
            break;
        }

        // Walk through every sample, measure duty cycle per-cycle
        // using positive-going threshold crossings as cycle boundaries.
        const float* m = mono.data() + start;
        int len = soundingSamples_4s;
        float thresh = pulseThreshold(m, 0, len);
        int cycle = 0;
        int positiveSamples = 0;
        int cycleSamples = 0;
        bool wasAbove = m[0] > thresh;
        bool hadFirstCrossing = false;
        float minD = 1.f, maxD = 0.f;
        double sum = 0.0;

        for (int i = 1; i < len; i++)
        {
            bool isAbove = m[i] > thresh;
            cycleSamples++;
            if (isAbove) positiveSamples++;

            // Positive-going threshold crossing = cycle boundary
            if (isAbove && !wasAbove)
            {
                if (hadFirstCrossing && cycleSamples > 10)
                {
                    float duty = static_cast<float>(positiveSamples) / static_cast<float>(cycleSamples);
                    printf("lfo,%d,%d,%.4f\n", kLfoDepths[d], cycle, duty * 100.f);
                    minD = std::min(minD, duty);
                    maxD = std::max(maxD, duty);
                    sum += duty;
                    cycle++;
                }
                hadFirstCrossing = true;
                positiveSamples = 0;
                cycleSamples = 0;
            }
            wasAbove = isAbove;
        }

        float meanD = (cycle > 0) ? static_cast<float>(sum / cycle) : 0.f;
        fprintf(stderr, "  depth %3d: %d cycles, duty %.2f%% - %.2f%% (mean %.2f%%, range %.2f%%)\n",
                kLfoDepths[d], cycle, minD * 100.f, maxD * 100.f, meanD * 100.f, (maxD - minD) * 100.f);

        blockPos += blockSamples_4s;
    }

    fprintf(stderr, "\nDone. CSV on stdout.\n");
}

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s gen output.mid            Generate test MIDI\n", argv[0]);
        fprintf(stderr, "  %s analyze input.wav|.aif    Analyze recording\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "gen") == 0)
        doGen(argv[2]);
    else if (strcmp(argv[1], "analyze") == 0)
        doAnalyze(argv[2]);
    else
        fprintf(stderr, "Unknown command: %s\n", argv[1]);

    return 0;
}
