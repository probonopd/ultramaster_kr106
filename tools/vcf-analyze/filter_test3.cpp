// filter_test3 -- unified VCF test: generate MIDI, render through DSP, analyze WAV.
//
// Captures the VCF frequency response at all 128 slider positions and
// two resonance levels. One program generates the test, renders it
// internally, or analyzes a hardware recording -- same grid definition
// throughout, no pattern recognition needed.
//
// PATCH SETUP (matches ROM calibration, no oscillators):
//   VCA=gate, pulse/saw/sub=off, noise=off (except noise sections),
//   VCF env/lfo/kbd=0, chorus=off, HPF=flat, sustain=max, note=C4.
//
// TEST SEQUENCE (all blocks contiguous, no gaps):
//
//   Preamble (6s calibration):
//     [0]  2s  Noise, filter wide open (byte 127), R=0
//              -> recording level calibration + noise spectrum baseline
//     [1]  2s  Saw wave, filter wide open (byte 127), R=0
//              -> harmonic rolloff reference through fully open filter
//     [2]  2s  Self-oscillation at byte 64, R=max
//              -> pitch reference for tuner calibration
//
//   Section 1 -- Self-oscillation sweep (128s):
//     128 x 1s blocks, R=max (127), VCF freq bytes 0-127
//     -> maps VCF slider position to self-oscillation frequency in Hz
//
//   Section 2 -- Noise sweep, R=0 (32s):
//     16 x 2s blocks, R=0, VCF freq bytes 0,8,16,...120
//     -> passband shape, -3/-6/-12/-24 dB points, rolloff slope
//
//   Section 3 -- Noise sweep, R=50 (32s):
//     16 x 2s blocks, R=50 (0.39), same VCF positions
//     -> slope vs resonance comparison
//
//   Total: 198s (3m18s)
//
// USAGE:
//   filter_test3 gen output.mid         Generate SysEx MIDI for hardware recording
//   filter_test3 render output.wav [sr] Render through DSP engine + analyze
//   filter_test3 analyze input.wav      Analyze a hardware WAV recording

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>

// ============================================================
// Test grid definition (shared by gen/render/analyze)
// ============================================================

static constexpr int kNote = 60; // C4

// Preamble: 3 x 2s calibration blocks
static constexpr int kPreambleCount = 3;
static constexpr float kPreambleDuration = 2.0f;
static constexpr float kPreambleTotal = kPreambleCount * kPreambleDuration; // 6s

// Section 1: self-oscillation sweep
static constexpr int kSelfOscCount = 128;
static constexpr float kSelfOscDuration = 1.0f;
static constexpr int kSelfOscRes = 127;

// Sections 2-3: noise sweeps
static constexpr int kNoiseCount = 16;
static constexpr float kNoiseDuration = 2.0f;
static constexpr int kNoisePositions[16] = {0,8,16,24,32,40,48,56,64,72,80,88,96,104,112,120};
static constexpr int kNoiseResValues[] = {0, 50};
static constexpr int kNoiseResSections = 2;

// Total duration
static constexpr float kTotalDuration =
    kPreambleTotal +
    kSelfOscCount * kSelfOscDuration +
    kNoiseCount * kNoiseDuration * kNoiseResSections;

// SysEx CC mapping
static constexpr int kSxVcfFreq = 0x05;
static constexpr int kSxVcfRes  = 0x06;
static constexpr int kSxNoise   = 0x04;
static constexpr int kSxVcfEnv  = 0x07;
static constexpr int kSxVcfLfo  = 0x08;
static constexpr int kSxVcfKbd  = 0x09;
static constexpr int kSxVcaLvl  = 0x0A;
static constexpr int kSxEnvA    = 0x0B;
static constexpr int kSxEnvD    = 0x0C;
static constexpr int kSxEnvS    = 0x0D;
static constexpr int kSxEnvR    = 0x0E;
static constexpr int kSxSub     = 0x0F;
static constexpr int kSxLfoRate = 0x00;
static constexpr int kSxLfoDly  = 0x01;
static constexpr int kSxDcoLfo  = 0x02;
static constexpr int kSxDcoPwm  = 0x03;

// ============================================================
// MIDI file helpers
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

static void writeMidi(const char* filename, const std::vector<uint8_t>& track, uint16_t ticksPerBeat)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return; }

    // Header
    std::vector<uint8_t> hdr;
    hdr.push_back('M'); hdr.push_back('T'); hdr.push_back('h'); hdr.push_back('d');
    write32(hdr, 6);
    write16(hdr, 0); // format 0
    write16(hdr, 1); // 1 track
    write16(hdr, ticksPerBeat);
    fwrite(hdr.data(), 1, hdr.size(), f);

    // Track
    std::vector<uint8_t> trkHdr;
    trkHdr.push_back('M'); trkHdr.push_back('T'); trkHdr.push_back('r'); trkHdr.push_back('k');
    write32(trkHdr, static_cast<uint32_t>(track.size()));
    fwrite(trkHdr.data(), 1, trkHdr.size(), f);
    fwrite(track.data(), 1, track.size(), f);

    fclose(f);
    fprintf(stderr, "Wrote %s (%.0fs)\n", filename, kTotalDuration);
}

// ============================================================
// gen: Generate MIDI
// ============================================================

static void doGen(const char* outFile)
{
    static constexpr uint16_t kTPB = 480;
    static constexpr uint32_t kTempo = 500000; // 120 BPM
    // At 120 BPM: 480 ticks = 0.5s, 960 = 1s, 1920 = 2s
    static constexpr uint32_t k1sTicks = 960;
    static constexpr uint32_t k2sTicks = 1920;

    std::vector<uint8_t> track;
    addTempo(track, kTempo);

    // --- Patch setup: VCA gate, no oscillators, no chorus, no env mod ---
    // Switches 1: oct=0 (bit 0), no pulse, no saw, chorus off (bit 5)
    addSysEx(track, 0, 0x10, 0x21);
    // Switches 2: PWM LFO, VCF env+, VCA env, HPF flat
    addSysEx(track, 0, 0x11, 0x04); // VCA gate mode
    // Zero all modulators
    addSysEx(track, 0, kSxVcfEnv, 0);
    addSysEx(track, 0, kSxVcfLfo, 0);
    addSysEx(track, 0, kSxVcfKbd, 0);
    addSysEx(track, 0, kSxNoise, 0);
    addSysEx(track, 0, kSxSub, 0);
    addSysEx(track, 0, kSxDcoLfo, 0);
    addSysEx(track, 0, kSxDcoPwm, 0);
    addSysEx(track, 0, kSxLfoRate, 0);
    addSysEx(track, 0, kSxLfoDly, 0);
    addSysEx(track, 0, kSxEnvA, 0);
    addSysEx(track, 0, kSxEnvD, 0);
    addSysEx(track, 0, kSxEnvS, 127);
    addSysEx(track, 0, kSxEnvR, 0);
    addSysEx(track, 0, kSxVcaLvl, 64); // unity

    // --- Preamble: calibration blocks ---
    // [0] 2s noise, filter wide open, R=0
    addSysEx(track, 0, kSxVcfFreq, 127);
    addSysEx(track, 0, kSxVcfRes, 0);
    addSysEx(track, 0, kSxNoise, 127);
    addNoteOn(track, 0, kNote, 127);
    addNoteOff(track, k2sTicks, kNote);
    addSysEx(track, 0, kSxNoise, 0);

    // [1] 2s saw, filter wide open, R=0
    // Switches 1: oct=0, pulse=off, SAW=on, chorus off
    addSysEx(track, 0, 0x10, 0x31); // 0x10 = saw on
    addNoteOn(track, 0, kNote, 127);
    addNoteOff(track, k2sTicks, kNote);
    addSysEx(track, 0, 0x10, 0x21); // saw back off

    // [2] 2s self-osc at byte 64, R=max
    addSysEx(track, 0, kSxVcfFreq, 64);
    addSysEx(track, 0, kSxVcfRes, 127);
    addNoteOn(track, 0, kNote, 127);
    addNoteOff(track, k2sTicks, kNote);

    // --- Section 1: Self-oscillation sweep ---
    addSysEx(track, 0, kSxVcfRes, kSelfOscRes);
    addNoteOn(track, 0, kNote, 127);

    for (int i = 0; i < kSelfOscCount; i++)
    {
        addSysEx(track, (i == 0) ? 0 : k1sTicks, kSxVcfFreq, static_cast<uint8_t>(i));
    }

    // Hold through last position
    addNoteOff(track, k1sTicks, kNote);

    // --- Section 2 & 3: Noise sweeps ---
    for (int ri = 0; ri < kNoiseResSections; ri++)
    {
        addSysEx(track, 0, kSxVcfRes, static_cast<uint8_t>(kNoiseResValues[ri]));
        addSysEx(track, 0, kSxNoise, 127);
        addNoteOn(track, 0, kNote, 127);

        for (int i = 0; i < kNoiseCount; i++)
        {
            addSysEx(track, (i == 0) ? 0 : k2sTicks, kSxVcfFreq,
                     static_cast<uint8_t>(kNoisePositions[i]));
        }

        addNoteOff(track, k2sTicks, kNote);
        addSysEx(track, 0, kSxNoise, 0);
    }

    addEndOfTrack(track);
    writeMidi(outFile, track, kTPB);
}

// ============================================================
// WAV I/O
// ============================================================

struct WavData {
    int sampleRate;
    int channels;
    int numSamples;
    std::vector<float> data;
};

static bool readWav(const char* filename, WavData& wav)
{
    FILE* f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return false; }

    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> buf(fileSize);
    if (fread(buf.data(), 1, fileSize, f) != fileSize)
    { fprintf(stderr, "Error: failed to read %s\n", filename); fclose(f); return false; }
    fclose(f);

    if (fileSize < 44 || memcmp(buf.data(), "RIFF", 4) != 0 ||
        memcmp(buf.data() + 8, "WAVE", 4) != 0)
    { fprintf(stderr, "Error: not a WAV file\n"); return false; }

    uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* dataPtr = nullptr;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= fileSize)
    {
        uint32_t chunkSize = *(uint32_t*)(buf.data() + pos + 4);
        if (memcmp(buf.data() + pos, "fmt ", 4) == 0 && chunkSize >= 16)
        {
            audioFmt = *(uint16_t*)(buf.data() + pos + 8);
            channels = *(uint16_t*)(buf.data() + pos + 10);
            sampleRate = *(uint32_t*)(buf.data() + pos + 12);
            bitsPerSample = *(uint16_t*)(buf.data() + pos + 22);
        }
        else if (memcmp(buf.data() + pos, "data", 4) == 0)
        { dataPtr = buf.data() + pos + 8; dataSize = chunkSize; }
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++;
    }

    if (!dataPtr || channels == 0)
    { fprintf(stderr, "Error: missing fmt/data chunks\n"); return false; }

    wav.sampleRate = sampleRate;
    wav.channels = channels;
    int bytesPerSample = bitsPerSample / 8;
    int frameSize = bytesPerSample * channels;
    wav.numSamples = dataSize / frameSize;
    wav.data.resize(wav.numSamples * channels);

    for (int i = 0; i < wav.numSamples * channels; i++)
    {
        const uint8_t* p = dataPtr + i * bytesPerSample;
        if (audioFmt == 3 && bitsPerSample == 32)
            wav.data[i] = *(float*)p;
        else if (audioFmt == 1 && bitsPerSample == 16)
            wav.data[i] = *(int16_t*)p / 32768.f;
        else if (audioFmt == 1 && bitsPerSample == 24)
        {
            int32_t v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= 0xFF000000;
            wav.data[i] = v / 8388608.f;
        }
        else if (audioFmt == 1 && bitsPerSample == 32)
            wav.data[i] = *(int32_t*)p / 2147483648.f;
        else
        { fprintf(stderr, "Error: unsupported format (fmt=%d, bits=%d)\n", audioFmt, bitsPerSample); return false; }
    }
    return true;
}

static void writeWav24(const char* filename, const float* data, int numSamples, int sampleRate)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return; }

    int bytesPerSample = 3;
    int dataSize = numSamples * bytesPerSample;
    int fileSize = 36 + dataSize;

    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };

    fwrite("RIFF", 1, 4, f); w32(fileSize);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16);
    w16(1); w16(1); w32(sampleRate);
    w32(sampleRate * bytesPerSample); w16(bytesPerSample); w16(24);
    fwrite("data", 1, 4, f); w32(dataSize);

    for (int i = 0; i < numSamples; i++)
    {
        int32_t s = static_cast<int32_t>(std::clamp(data[i], -1.f, 1.f) * 8388607.f);
        uint8_t b[3] = { static_cast<uint8_t>(s & 0xFF),
                         static_cast<uint8_t>((s >> 8) & 0xFF),
                         static_cast<uint8_t>((s >> 16) & 0xFF) };
        fwrite(b, 1, 3, f);
    }

    fclose(f);
    fprintf(stderr, "Wrote %s: %d samples, %d Hz, 24-bit mono (%.1f sec)\n",
            filename, numSamples, sampleRate, numSamples / (float)sampleRate);
}

// ============================================================
// FFT (radix-2)
// ============================================================

static void fft(std::vector<float>& re, std::vector<float>& im)
{
    int n = static_cast<int>(re.size());
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        float ang = -2.f * static_cast<float>(M_PI) / len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < n; i += len)
        {
            float curRe = 1.f, curIm = 0.f;
            for (int j = 0; j < len / 2; j++)
            {
                int u = i + j, v = i + j + len / 2;
                float tRe = re[v] * curRe - im[v] * curIm;
                float tIm = re[v] * curIm + im[v] * curRe;
                re[v] = re[u] - tRe; im[v] = im[u] - tIm;
                re[u] += tRe; im[u] += tIm;
                float nr = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nr;
            }
        }
    }
}

// ============================================================
// Analysis
// ============================================================

// Measure self-oscillation frequency via zero-crossing
static float measureFreq(const float* samples, int numSamples, int sampleRate)
{
    // Skip first 10% for settling
    int skip = numSamples / 10;
    int count = 0;
    float firstCross = -1.f, lastCross = -1.f;

    for (int i = skip + 1; i < numSamples; i++)
    {
        if (samples[i - 1] <= 0.f && samples[i] > 0.f)
        {
            float frac = -samples[i - 1] / (samples[i] - samples[i - 1]);
            float pos = (i - 1 + frac) / sampleRate;
            if (firstCross < 0.f) firstCross = pos;
            lastCross = pos;
            count++;
        }
    }

    if (count < 2) return 0.f;
    return (count - 1) / (lastCross - firstCross);
}

// Measure noise response: -3dB, -6dB, -12dB, -24dB, slope
struct NoiseResult {
    float passbandDb;
    float minus3dbHz, minus6dbHz, minus12dbHz, minus24dbHz;
    float slopeDbOct;
    float peakHz, peakDb, prominenceDb;
};

static NoiseResult analyzeNoise(const float* samples, int numSamples, int sampleRate,
                                 float targetHz)
{
    NoiseResult r = {};

    // Skip first 10% for settling
    int skip = numSamples / 10;
    int len = numSamples - skip;
    const float* seg = samples + skip;

    int fftSize = 1;
    while (fftSize < len) fftSize <<= 1;
    if (fftSize < 8192) fftSize = 8192;

    std::vector<float> re(fftSize, 0.f), im(fftSize, 0.f);
    for (int i = 0; i < len; i++)
    {
        float w = 0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (len - 1)));
        re[i] = seg[i] * w;
    }
    fft(re, im);

    int specSize = fftSize / 2 + 1;
    std::vector<float> magDb(specSize);
    float binHz = static_cast<float>(sampleRate) / fftSize;

    for (int i = 0; i < specSize; i++)
        magDb[i] = 20.f * log10f(sqrtf(re[i] * re[i] + im[i] * im[i]) + 1e-30f);

    // Smooth (~30 Hz window)
    int smoothBins = std::max(1, static_cast<int>(30.f / binHz));
    std::vector<float> smooth(specSize);
    for (int i = 0; i < specSize; i++)
    {
        float sum = 0; int cnt = 0;
        for (int j = std::max(0, i - smoothBins); j <= std::min(specSize - 1, i + smoothBins); j++)
        { sum += magDb[j]; cnt++; }
        smooth[i] = sum / cnt;
    }

    // Passband: average 20 Hz to 0.3x target (or 50 Hz min)
    float pbMax = std::max(targetHz * 0.3f, 50.f);
    int pbLo = std::max(1, static_cast<int>(20.f / binHz));
    int pbHi = std::max(pbLo + 1, std::min(specSize - 1, static_cast<int>(pbMax / binHz)));
    float pbSum = 0; int pbCnt = 0;
    for (int i = pbLo; i <= pbHi; i++) { pbSum += smooth[i]; pbCnt++; }
    r.passbandDb = (pbCnt > 0) ? pbSum / pbCnt : -200.f;

    // Peak
    int pkLo = std::max(1, static_cast<int>(targetHz * 0.3f / binHz));
    int pkHi = std::min(specSize - 1, static_cast<int>(targetHz * 3.f / binHz));
    r.peakDb = -200.f;
    for (int i = pkLo; i <= pkHi; i++)
    {
        if (smooth[i] > r.peakDb)
        { r.peakDb = smooth[i]; r.peakHz = i * binHz; }
    }
    r.prominenceDb = r.peakDb - r.passbandDb;

    // Threshold crossings
    auto findThresh = [&](float dbBelow) -> float {
        float thresh = r.passbandDb - dbBelow;
        for (int i = pbHi; i < specSize - 1; i++)
        {
            if (smooth[i] >= thresh && smooth[i + 1] < thresh)
            {
                float frac = (thresh - smooth[i]) / (smooth[i + 1] - smooth[i]);
                return (i + frac) * binHz;
            }
        }
        return 0.f;
    };

    r.minus3dbHz  = findThresh(3.f);
    r.minus6dbHz  = findThresh(6.f);
    r.minus12dbHz = findThresh(12.f);
    r.minus24dbHz = findThresh(24.f);

    if (r.minus3dbHz > 0 && r.minus24dbHz > r.minus3dbHz)
    {
        float octaves = log2f(r.minus24dbHz / r.minus3dbHz);
        if (octaves > 0.01f) r.slopeDbOct = -21.f / octaves;
    }

    return r;
}

static void doAnalyze(const float* mono, int numSamples, int sampleRate)
{
    int preambleSamples = static_cast<int>(kPreambleDuration * sampleRate);
    int preambleOffset = static_cast<int>(kPreambleTotal * sampleRate);

    // CSV header
    printf("section,vcf_byte,res,target_hz,freq_hz,minus3db_hz,minus6db_hz,minus12db_hz,minus24db_hz,slope_db_oct,passband_db,peak_hz,prominence_db\n");

    // --- Preamble analysis ---
    if (preambleSamples <= numSamples)
    {
        float rms = 0;
        for (int i = 0; i < preambleSamples; i++) rms += mono[i] * mono[i];
        rms = 10.f * log10f(rms / preambleSamples + 1e-30f);
        printf("preamble_noise,127,0,0,0,0,0,0,0,0,%.1f,0,0\n", rms);
    }
    if (2 * preambleSamples <= numSamples)
    {
        float rms = 0;
        const float* seg = mono + preambleSamples;
        for (int i = 0; i < preambleSamples; i++) rms += seg[i] * seg[i];
        rms = 10.f * log10f(rms / preambleSamples + 1e-30f);
        printf("preamble_saw,127,0,0,0,0,0,0,0,0,%.1f,0,0\n", rms);
    }
    if (3 * preambleSamples <= numSamples)
    {
        float hz = measureFreq(mono + 2 * preambleSamples, preambleSamples, sampleRate);
        printf("preamble_pitch,64,127,0,%.2f,0,0,0,0,0,0,0,0\n", hz);
    }

    // --- Section 1: Self-oscillation ---
    int s1Samples = static_cast<int>(kSelfOscDuration * sampleRate);
    for (int i = 0; i < kSelfOscCount; i++)
    {
        int start = preambleOffset + static_cast<int>(i * kSelfOscDuration * sampleRate);
        if (start + s1Samples > numSamples) break;
        float hz = measureFreq(mono + start, s1Samples, sampleRate);
        printf("self_osc,%d,%d,0,%.2f,0,0,0,0,0,0,0,0\n", i, kSelfOscRes, hz);
    }

    // --- Sections 2 & 3: Noise ---
    float s1Total = kPreambleTotal + kSelfOscCount * kSelfOscDuration;
    int s2Samples = static_cast<int>(kNoiseDuration * sampleRate);

    for (int ri = 0; ri < kNoiseResSections; ri++)
    {
        int res = kNoiseResValues[ri];
        for (int i = 0; i < kNoiseCount; i++)
        {
            float tStart = s1Total + (ri * kNoiseCount + i) * kNoiseDuration;
            int start = static_cast<int>(tStart * sampleRate);
            if (start + s2Samples > numSamples) break;

            int selfIdx = kNoisePositions[i];
            int selfStart = preambleOffset + static_cast<int>(selfIdx * kSelfOscDuration * sampleRate);
            float targetHz = 248.f;
            if (selfStart + s1Samples <= numSamples)
                targetHz = measureFreq(mono + selfStart, s1Samples, sampleRate);
            if (targetHz <= 0.f) targetHz = 248.f;

            NoiseResult nr = analyzeNoise(mono + start, s2Samples, sampleRate, targetHz);

            printf("noise,%d,%d,%.1f,0,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                   kNoisePositions[i], res, targetHz,
                   nr.minus3dbHz, nr.minus6dbHz, nr.minus12dbHz, nr.minus24dbHz,
                   nr.slopeDbOct, nr.passbandDb, nr.peakHz, nr.prominenceDb);
        }
    }
}

// ============================================================
// render: Drive DSP directly, write WAV, then analyze
// ============================================================

#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"

enum EParams
{
  kBenderDco = 0, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
  kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
  kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
  kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
  kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw, kDcoSubSw,
  kChorusOff, kChorusI, kChorusII,
  kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
  kVcfEnvInv, kVcaMode,
  kBender, kTuning, kPower,
  kPortaMode, kPortaRate,
  kTransposeOffset, kBenderLfo,
  kAdsrMode,
  kMasterVol,
  kNumParams
};

static void setParam(KR106DSP<float>& dsp, int param, float value)
{
    dsp.SetParam(param, static_cast<double>(value));
}

static void initPatch(KR106DSP<float>& dsp)
{
    setParam(dsp, kPower, 1.f);
    setParam(dsp, kVcaMode, 1.f);       // gate
    setParam(dsp, kDcoPulse, 0.f);
    setParam(dsp, kDcoSaw, 0.f);
    setParam(dsp, kDcoSubSw, 0.f);
    setParam(dsp, kDcoNoise, 0.f);
    setParam(dsp, kDcoSub, 0.f);
    setParam(dsp, kDcoLfo, 0.f);
    setParam(dsp, kDcoPwm, 0.f);
    setParam(dsp, kVcfEnv, 0.f);
    setParam(dsp, kVcfLfo, 0.f);
    setParam(dsp, kVcfKbd, 0.f);
    setParam(dsp, kVcaLevel, 0.5f);     // unity
    setParam(dsp, kEnvA, 0.f);
    setParam(dsp, kEnvD, 0.f);
    setParam(dsp, kEnvS, 1.f);
    setParam(dsp, kEnvR, 0.f);
    setParam(dsp, kLfoRate, 0.f);
    setParam(dsp, kLfoDelay, 0.f);
    setParam(dsp, kHpfFreq, 1.f);       // flat
    setParam(dsp, kChorusOff, 1.f);
    setParam(dsp, kChorusI, 0.f);
    setParam(dsp, kChorusII, 0.f);
    setParam(dsp, kOctTranspose, 1.f);   // middle
    setParam(dsp, kPortaMode, 2.f);      // poly
    setParam(dsp, kAdsrMode, 1.f);       // J106
}

static void doRender(const char* outFile, int sampleRate)
{
    static constexpr int kBlockSize = 512;

    KR106DSP<float> dsp(6);
    dsp.Reset(static_cast<double>(sampleRate), kBlockSize);
    initPatch(dsp);

    int totalSamples = static_cast<int>(kTotalDuration * sampleRate);
    std::vector<float> audio(totalSamples, 0.f);

    std::vector<float> bufL(kBlockSize), bufR(kBlockSize);
    float* outputs[2] = { bufL.data(), bufR.data() };

    int pos = 0;
    auto render = [&](float seconds) {
        int samples = static_cast<int>(seconds * sampleRate);
        while (samples > 0)
        {
            int n = std::min(samples, kBlockSize);
            dsp.ProcessBlock(nullptr, outputs, 2, n);
            for (int i = 0; i < n && pos < totalSamples; i++, pos++)
                audio[pos] = (bufL[i] + bufR[i]) * 0.5f;
            samples -= n;
        }
    };

    // --- Preamble ---
    // [0] Noise, filter wide open, R=0
    setParam(dsp, kVcfFreq, 1.f);
    setParam(dsp, kVcfRes, 0.f);
    setParam(dsp, kDcoNoise, 1.f);
    dsp.NoteOn(kNote, 127);
    render(kPreambleDuration);
    dsp.NoteOff(kNote);
    setParam(dsp, kDcoNoise, 0.f);
    // [1] Saw, filter wide open, R=0
    setParam(dsp, kDcoSaw, 1.f);
    dsp.NoteOn(kNote, 127);
    render(kPreambleDuration);
    dsp.NoteOff(kNote);
    setParam(dsp, kDcoSaw, 0.f);
    // [2] Self-osc at byte 64, R=max
    setParam(dsp, kVcfFreq, 64.f / 127.f);
    setParam(dsp, kVcfRes, 1.f);
    dsp.NoteOn(kNote, 127);
    render(kPreambleDuration);
    dsp.NoteOff(kNote);
    // --- Section 1: Self-oscillation sweep ---
    setParam(dsp, kVcfRes, kSelfOscRes / 127.f);
    setParam(dsp, kDcoNoise, 0.f);
    dsp.NoteOn(kNote, 127);

    for (int i = 0; i < kSelfOscCount; i++)
    {
        setParam(dsp, kVcfFreq, i / 127.f);
        render(kSelfOscDuration);
    }

    dsp.NoteOff(kNote);

    // Sections 2 & 3: Noise
    for (int ri = 0; ri < kNoiseResSections; ri++)
    {
        setParam(dsp, kVcfRes, kNoiseResValues[ri] / 127.f);
        setParam(dsp, kDcoNoise, 1.f);
        dsp.NoteOn(kNote, 127);

        for (int i = 0; i < kNoiseCount; i++)
        {
            setParam(dsp, kVcfFreq, kNoisePositions[i] / 127.f);
            render(kNoiseDuration);
        }

        dsp.NoteOff(kNote);
        setParam(dsp, kDcoNoise, 0.f);
    }

    writeWav24(outFile, audio.data(), pos, sampleRate);
    doAnalyze(audio.data(), pos, sampleRate);
}

// ============================================================
// profile: sweep all 128 R values internally (no WAV output)
// ============================================================

static void doProfile(int sampleRate)
{
    static constexpr int kBlockSize = 512;
    static constexpr float kSelfOscSec = 0.5f;  // enough for frequency measurement
    static constexpr float kNoiseSec = 1.5f;     // enough for FFT

    KR106DSP<float> dsp(6);
    dsp.Reset(static_cast<double>(sampleRate), kBlockSize);
    initPatch(dsp);

    std::vector<float> bufL(kBlockSize), bufR(kBlockSize);
    float* outputs[2] = { bufL.data(), bufR.data() };

    int selfOscSamples = static_cast<int>(kSelfOscSec * sampleRate);
    int noiseSamples = static_cast<int>(kNoiseSec * sampleRate);
    std::vector<float> segment(std::max(selfOscSamples, noiseSamples));

    auto renderSegment = [&](int numSamples) {
        int pos = 0;
        int remaining = numSamples;
        while (remaining > 0)
        {
            int n = std::min(remaining, kBlockSize);
            dsp.ProcessBlock(nullptr, outputs, 2, n);
            for (int i = 0; i < n; i++)
                segment[pos++] = (bufL[i] + bufR[i]) * 0.5f;
            remaining -= n;
        }
    };

    // First pass: measure self-osc frequency at all 128 positions (R=max)
    // This gives us the true cutoff Hz for each VCF byte.

    float selfOscTable[128];
    setParam(dsp, kVcfRes, 1.f);
    setParam(dsp, kDcoNoise, 0.f);
    dsp.NoteOn(kNote, 127);
    for (int f = 0; f < 128; f++)
    {
        setParam(dsp, kVcfFreq, f / 127.f);
        renderSegment(selfOscSamples);
        selfOscTable[f] = measureFreq(segment.data(), selfOscSamples, sampleRate);
    }
    dsp.NoteOff(kNote);

    // CSV header
    printf("vcf_byte,res_byte,self_osc_hz,minus3db_hz,minus6db_hz,minus12db_hz,minus24db_hz,slope_db_oct,passband_db\n");

    // Second pass: noise at every freq x res combination.
    // Keep note held throughout, sweep freq inner / res outer so the
    // filter state changes gradually (no note-on/off transients).
    setParam(dsp, kDcoNoise, 1.f);
    dsp.NoteOn(kNote, 127);

    for (int r = 0; r < 128; r++)
    {
        setParam(dsp, kVcfRes, r / 127.f);

        for (int f = 0; f < 128; f++)
        {
            setParam(dsp, kVcfFreq, f / 127.f);
            renderSegment(noiseSamples);

            float targetHz = (selfOscTable[f] > 0.f) ? selfOscTable[f] : 248.f;
            NoiseResult nr = analyzeNoise(segment.data(), noiseSamples, sampleRate, targetHz);

            printf("%d,%d,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                   f, r, selfOscTable[f],
                   nr.minus3dbHz, nr.minus6dbHz, nr.minus12dbHz, nr.minus24dbHz,
                   nr.slopeDbOct, nr.passbandDb);
        }
    }

    dsp.NoteOff(kNote);

}

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  filter_test3 gen output.mid          Generate test MIDI\n");
        fprintf(stderr, "  filter_test3 render output.wav [sr]  Render through DSP + analyze\n");
        fprintf(stderr, "  filter_test3 analyze input.wav       Analyze a recording\n");
        fprintf(stderr, "  filter_test3 profile [sr]            Sweep all 128 R x 16 freq (internal)\n");
        fprintf(stderr, "\nTest grid: %d self-osc + %d noise x %d res = %.0fs total\n",
                kSelfOscCount, kNoiseCount, kNoiseResSections, kTotalDuration);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "gen")
    {
        const char* out = (argc > 2) ? argv[2] : "filter_test3.mid";
        doGen(out);
    }
    else if (mode == "render")
    {
        const char* out = (argc > 2) ? argv[2] : "filter_test3.wav";
        int sr = (argc > 3) ? atoi(argv[3]) : 44100;
        doRender(out, sr);
    }
    else if (mode == "profile")
    {
        int sr = (argc > 2) ? atoi(argv[2]) : 44100;
        doProfile(sr);
    }
    else if (mode == "analyze")
    {
        if (argc < 3) { fprintf(stderr, "Error: need input.wav\n"); return 1; }
        WavData wav;
        if (!readWav(argv[2], wav)) return 1;

        // Extract mono
        std::vector<float> mono(wav.numSamples);
        for (int i = 0; i < wav.numSamples; i++)
            mono[i] = wav.data[i * wav.channels];

        doAnalyze(mono.data(), wav.numSamples, wav.sampleRate);
    }
    else
    {
        fprintf(stderr, "Unknown mode: %s\n", mode.c_str());
        return 1;
    }

    return 0;
}
