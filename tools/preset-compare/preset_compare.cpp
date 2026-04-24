// preset_compare -- Compare hardware Juno-106 recordings against KR-106 DSP model.
//
// Given a patch name (e.g. "A23"), finds the right segment in a bank WAV/AIFF
// recording, renders the same preset through the DSP model, and compares
// harmonic structure, envelope shape, spectral content, and levels.
//
// USAGE:
//   preset_compare compare A23 bank_wavs/          Single preset comparison
//   preset_compare compare A23 bank_wavs/ --wav     Also dump HW/DSP wav segments
//   preset_compare batch bank_wavs/                 All presets, summary CSV

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

#include "../../Source/KR106_Presets_JUCE.h"
#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"
#include "../../Source/DSP/KR106ADSR.h"

// ============================================================
// Patch name parsing
// ============================================================

// J106 presets are indices 128-255 in kFactoryPresets.
// "A11" = 128, "A12" = 129, ..., "A18" = 135, "A21" = 136, ..., "B88" = 255.
// Bank files: "bank_A1x.wav" contains A11-A18 (indices 128-135).

static constexpr int kJ106Offset = 128;

struct PatchInfo {
    int arrayIndex;         // index into kFactoryPresets[]
    int bankIndex;          // 0-15 (which bank file)
    int indexInBank;        // 0-7 (position within bank)
    std::string bankName;   // "A1x", "A2x", ..., "B8x"
};

static bool parsePatchName(const char* name, PatchInfo& info)
{
    if (strlen(name) < 3) return false;

    char bank = toupper(name[0]);    // A or B
    int group = name[1] - '0';       // 1-8
    int patch = name[2] - '0';       // 1-8

    if ((bank != 'A' && bank != 'B') || group < 1 || group > 8 || patch < 1 || patch > 8)
        return false;

    int bankOffset = (bank == 'A') ? 0 : 8;
    info.bankIndex = bankOffset + (group - 1);
    info.indexInBank = patch - 1;
    info.arrayIndex = kJ106Offset + info.bankIndex * 8 + info.indexInBank;

    char buf[8];
    snprintf(buf, sizeof(buf), "%c%dx", bank, group);
    info.bankName = buf;

    return true;
}

// ============================================================
// Audio I/O (WAV + AIFF)
// ============================================================

struct AudioData {
    int sampleRate;
    int channels;
    int numSamples;
    std::vector<float> data;  // interleaved if stereo
};

// Big-endian readers for AIFF
static uint16_t readBE16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static uint32_t readBE32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Convert 80-bit IEEE 754 extended (AIFF sample rate) to double
static double readExtended80(const uint8_t* p)
{
    int sign = (p[0] >> 7) & 1;
    int exponent = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++)
        mantissa = (mantissa << 8) | p[2 + i];
    if (exponent == 0 && mantissa == 0) return 0.0;
    double f = static_cast<double>(mantissa) / (1ULL << 63);
    f = ldexp(f, exponent - 16383);
    return sign ? -f : f;
}

static bool readWav(const uint8_t* buf, size_t fileSize, AudioData& audio)
{
    if (fileSize < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;

    uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* dataPtr = nullptr;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= fileSize)
    {
        uint32_t chunkSize = *(uint32_t*)(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && chunkSize >= 16)
        {
            audioFmt = *(uint16_t*)(buf + pos + 8);
            channels = *(uint16_t*)(buf + pos + 10);
            sampleRate = *(uint32_t*)(buf + pos + 12);
            bitsPerSample = *(uint16_t*)(buf + pos + 22);
            if (audioFmt == 0xFFFE && chunkSize >= 40)
                audioFmt = *(uint16_t*)(buf + pos + 32);
        }
        else if (memcmp(buf + pos, "data", 4) == 0)
        { dataPtr = buf + pos + 8; dataSize = chunkSize; }
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++;
    }

    if (!dataPtr || channels == 0) return false;

    audio.sampleRate = sampleRate;
    audio.channels = channels;
    int bytesPerSample = bitsPerSample / 8;
    int frameSize = bytesPerSample * channels;
    audio.numSamples = dataSize / frameSize;
    audio.data.resize(audio.numSamples * channels);

    for (int i = 0; i < audio.numSamples * channels; i++)
    {
        const uint8_t* p = dataPtr + i * bytesPerSample;
        if (audioFmt == 3 && bitsPerSample == 32)
            audio.data[i] = *(float*)p;
        else if (audioFmt == 1 && bitsPerSample == 16)
            audio.data[i] = *(int16_t*)p / 32768.f;
        else if (audioFmt == 1 && bitsPerSample == 24)
        {
            int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= 0xFF000000;
            audio.data[i] = v / 8388608.f;
        }
        else if (audioFmt == 1 && bitsPerSample == 32)
            audio.data[i] = *(int32_t*)p / 2147483648.f;
        else
        { fprintf(stderr, "Error: unsupported WAV format (fmt=%d, bits=%d)\n", audioFmt, bitsPerSample); return false; }
    }
    return true;
}

static bool readAiff(const uint8_t* buf, size_t fileSize, AudioData& audio)
{
    if (fileSize < 12 || memcmp(buf, "FORM", 4) != 0)
        return false;
    bool isAIFC = (memcmp(buf + 8, "AIFC", 4) == 0);
    if (!isAIFC && memcmp(buf + 8, "AIFF", 4) != 0)
        return false;

    int channels = 0, bitsPerSample = 0;
    int numFrames = 0;
    double sampleRate = 0;
    const uint8_t* ssndData = nullptr;
    uint32_t ssndSize [[maybe_unused]] = 0;

    size_t pos = 12;
    while (pos + 8 <= fileSize)
    {
        uint32_t chunkSize = readBE32(buf + pos + 4);
        if (memcmp(buf + pos, "COMM", 4) == 0)
        {
            channels = readBE16(buf + pos + 8);
            numFrames = static_cast<int>(readBE32(buf + pos + 10));
            bitsPerSample = readBE16(buf + pos + 14);
            sampleRate = readExtended80(buf + pos + 16);
        }
        else if (memcmp(buf + pos, "SSND", 4) == 0)
        {
            uint32_t offset = readBE32(buf + pos + 8);
            ssndData = buf + pos + 16 + offset;
            ssndSize = chunkSize - 8 - offset;
        }
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++;
    }

    if (!ssndData || channels == 0 || numFrames == 0) return false;

    audio.sampleRate = static_cast<int>(sampleRate);
    audio.channels = channels;
    audio.numSamples = numFrames;
    audio.data.resize(numFrames * channels);

    int bytesPerSample = bitsPerSample / 8;
    for (int i = 0; i < numFrames * channels; i++)
    {
        const uint8_t* p = ssndData + i * bytesPerSample;
        if (bitsPerSample == 16)
        {
            int16_t v = (p[0] << 8) | p[1]; // big-endian
            audio.data[i] = v / 32768.f;
        }
        else if (bitsPerSample == 24)
        {
            int32_t v = (p[0] << 24) | (p[1] << 16) | (p[2] << 8);
            audio.data[i] = v / 2147483648.f;
        }
        else if (bitsPerSample == 32)
        {
            int32_t v = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            audio.data[i] = v / 2147483648.f;
        }
        else
        { fprintf(stderr, "Error: unsupported AIFF bits=%d\n", bitsPerSample); return false; }
    }
    return true;
}

static bool readAudio(const char* filename, AudioData& audio)
{
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(fileSize);
    if (fread(buf.data(), 1, fileSize, f) != fileSize)
    { fprintf(stderr, "Error: failed to read %s\n", filename); fclose(f); return false; }
    fclose(f);

    if (fileSize >= 4 && memcmp(buf.data(), "RIFF", 4) == 0)
        return readWav(buf.data(), fileSize, audio);
    if (fileSize >= 4 && memcmp(buf.data(), "FORM", 4) == 0)
        return readAiff(buf.data(), fileSize, audio);

    fprintf(stderr, "Error: %s is not a WAV or AIFF file\n", filename);
    return false;
}

// Extract left channel only (for A/B: same chorus phase)
static std::vector<float> leftChannel(const AudioData& audio)
{
    std::vector<float> left(audio.numSamples);
    if (audio.channels == 1)
    {
        for (int i = 0; i < audio.numSamples; i++)
            left[i] = audio.data[i];
    }
    else
    {
        for (int i = 0; i < audio.numSamples; i++)
            left[i] = audio.data[i * audio.channels]; // channel 0 only
    }
    return left;
}

// Mono mixdown (in-place)
static std::vector<float> toMono(const AudioData& audio)
{
    std::vector<float> mono(audio.numSamples);
    if (audio.channels == 1)
    {
        for (int i = 0; i < audio.numSamples; i++)
            mono[i] = audio.data[i];
    }
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
    return mono;
}

// Write mono 24-bit WAV
static void writeWav24(const char* filename, const float* data, int numSamples, int sampleRate)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s for writing\n", filename); return; }

    int dataSize = numSamples * 3;
    int fileSize = 36 + dataSize;
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };

    fwrite("RIFF", 1, 4, f); w32(fileSize);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16);
    w16(1); w16(1); w32(sampleRate);
    w32(sampleRate * 3); w16(3); w16(24);
    fwrite("data", 1, 4, f); w32(dataSize);

    for (int i = 0; i < numSamples; i++)
    {
        int32_t v = std::clamp(static_cast<int32_t>(data[i] * 8388608.f), -8388608, 8388607);
        uint8_t b[3] = { static_cast<uint8_t>(v & 0xFF),
                         static_cast<uint8_t>((v >> 8) & 0xFF),
                         static_cast<uint8_t>((v >> 16) & 0xFF) };
        fwrite(b, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "Wrote %s\n", filename);
}

// Write stereo 24-bit WAV (L=ch1, R=ch2)
static void writeWavStereo24(const char* filename, const float* L, const float* R,
                              int numSamples, int sampleRate)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s for writing\n", filename); return; }

    int bytesPerFrame = 6; // 3 bytes * 2 channels
    int dataSize = numSamples * bytesPerFrame;
    int fileSize = 36 + dataSize;
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };

    fwrite("RIFF", 1, 4, f); w32(fileSize);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16);
    w16(1); w16(2); w32(sampleRate);
    w32(sampleRate * bytesPerFrame); w16(bytesPerFrame); w16(24);
    fwrite("data", 1, 4, f); w32(dataSize);

    for (int i = 0; i < numSamples; i++)
    {
        for (const float* ch : {L, R})
        {
            float s = (i < numSamples) ? ch[i] : 0.f;
            int32_t v = std::clamp(static_cast<int32_t>(s * 8388608.f), -8388608, 8388607);
            uint8_t b[3] = { static_cast<uint8_t>(v & 0xFF),
                             static_cast<uint8_t>((v >> 8) & 0xFF),
                             static_cast<uint8_t>((v >> 16) & 0xFF) };
            fwrite(b, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "Wrote %s (%d frames stereo)\n", filename, numSamples);
}

// ============================================================
// Timing calculator (replicates gen_preset_midi.cpp timing)
// ============================================================

// At 120 BPM, 480 ticks per beat: 1 tick = 500/480 ms = 1.04167 ms
static constexpr double kTickMs = 500.0 / 480.0;

struct PresetTiming {
    double startMs;     // start of this preset's region (after gap)
    double settleMs;    // settle period (250ms)
    double chordOnMs;   // chord NoteOn time
    double chordOffMs;  // chord NoteOff time
    double relEndMs;    // end of release tail
    double quickNoteMs[5][2]; // [note][0=on, 1=off] for C2,C3,C4,C5,C6
    double totalEndMs;  // end of last quick note
};

static PresetTiming computeTiming(int bankStartIndex, int indexInBank)
{
    // Walk through all presets in the bank to accumulate time
    double curMs = 0;
    PresetTiming result = {};

    for (int i = 0; i <= indexInBank; i++)
    {
        int presetIdx = bankStartIndex + i;
        const int* v = kFactoryPresets[presetIdx].values;

        // Gap between presets (2s, except first)
        if (i > 0) curMs += 480.0 * 4.0 * kTickMs;

        double presetStart = curMs;

        // Compute ADSR timing
        float atkSlider = v[16] / 127.f;
        float relSlider = v[19] / 127.f;
        float atkMs = kr106::ADSR::AttackMs(atkSlider);
        float relMs = kr106::ADSR::DecRelMs(relSlider);

        auto msToTicks = [](float ms) -> uint32_t {
            return std::max(static_cast<uint32_t>(ms * 480.f / 500.f + 0.5f), uint32_t(48));
        };

        uint32_t holdTicks = msToTicks(atkMs + 500.f);
        uint32_t relTicks = msToTicks(relMs + 500.f);
        uint32_t settleTicks = 240;

        // Settle
        double settleMs = settleTicks * kTickMs;
        curMs += settleMs;
        double chordOn = curMs;

        // Chord hold
        curMs += holdTicks * kTickMs;
        double chordOff = curMs;

        // Release tail
        double quickStart = curMs + relTicks * kTickMs;

        // Quick notes: C2, C3, C4, C5, C6
        // First note delta = relTicks, subsequent = 480*2
        double qMs = quickStart;
        double quickNotes[5][2];
        for (int n = 0; n < 5; n++)
        {
            if (n > 0) qMs += 480.0 * 2.0 * kTickMs;
            quickNotes[n][0] = qMs;  // NoteOn
            qMs += 480.0 * 2.0 * kTickMs;
            quickNotes[n][1] = qMs;  // NoteOff
        }

        if (i == indexInBank)
        {
            result.startMs = presetStart;
            result.settleMs = settleMs;
            result.chordOnMs = chordOn;
            result.chordOffMs = chordOff;
            result.relEndMs = quickStart;
            for (int n = 0; n < 5; n++)
            {
                result.quickNoteMs[n][0] = quickNotes[n][0];
                result.quickNoteMs[n][1] = quickNotes[n][1];
            }
            result.totalEndMs = qMs;
        }

        curMs = qMs; // advance past this preset for next iteration
    }

    return result;
}

// ============================================================
// DSP rendering
// ============================================================

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

// Preset value indices that are sliders (0-127 -> 0.0-1.0)
static bool isSliderParam(int presetIdx)
{
    return (presetIdx >= 0 && presetIdx <= 19) || presetIdx == 40;
}

static void setParam(KR106DSP<float>& dsp, int param, float value)
{
    dsp.SetParam(param, static_cast<double>(value));
}

static void loadPreset(KR106DSP<float>& dsp, int presetArrayIndex)
{
    const int* v = kFactoryPresets[presetArrayIndex].values;

    setParam(dsp, kPower, 1.f);
    setParam(dsp, kMasterVol, 0.5f);

    // ADSR mode from preset (index 43): 0=J6, 1=J106
    setParam(dsp, kAdsrMode, static_cast<float>(v[43]));

    // Load all 44 preset values
    for (int i = 0; i < 44; i++)
    {
        float val = isSliderParam(i) ? (v[i] / 127.f) : static_cast<float>(v[i]);
        setParam(dsp, i, val);
    }

    // Force Poly I (hardware bubble-sort allocator) for accurate voice management
    setParam(dsp, kPortaMode, 1.f);

    // Zero variance for deterministic comparison
    dsp.ForEachVoice([](kr106::Voice<float>& voice) {
        for (int i = 0; i < kr106::Voice<float>::kNumVarianceParams; i++)
            voice.SetVariance(i, 0.f);
    });
}

static std::vector<float> renderPreset(int presetArrayIndex, int sampleRate,
                                        const PresetTiming& timing)
{
    static constexpr int kBlockSize = 512;

    KR106DSP<float> dsp(6);
    dsp.Reset(static_cast<double>(sampleRate), kBlockSize);
    loadPreset(dsp, presetArrayIndex);

    // Total duration from start to end of last quick note
    double totalMs = timing.totalEndMs - timing.startMs;
    int totalSamples = static_cast<int>(totalMs * sampleRate / 1000.0) + sampleRate; // +1s safety
    std::vector<float> audio(totalSamples, 0.f);

    std::vector<float> bufL(kBlockSize), bufR(kBlockSize);
    float* outputs[2] = { bufL.data(), bufR.data() };

    int pos = 0;
    auto render = [&](double ms) {
        int samples = static_cast<int>(ms * sampleRate / 1000.0);
        while (samples > 0 && pos < totalSamples)
        {
            int n = std::min(samples, kBlockSize);
            n = std::min(n, totalSamples - pos);
            dsp.ProcessBlock(nullptr, outputs, 2, n);
            for (int i = 0; i < n; i++, pos++)
                audio[pos] = (bufL[i] + bufR[i]) * 0.5f;
            samples -= n;
        }
    };

    // Settle (silence)
    render(timing.settleMs);

    // Chord: C4 + E4 + G4
    dsp.NoteOn(60, 100);
    dsp.NoteOn(64, 100);
    dsp.NoteOn(67, 100);
    render(timing.chordOffMs - timing.chordOnMs);
    dsp.NoteOff(60);
    dsp.NoteOff(64);
    dsp.NoteOff(67);

    // Release tail
    render(timing.relEndMs - timing.chordOffMs);

    // Quick notes: C2, C3, C4, C5, C6
    static constexpr int testNotes[] = {36, 48, 60, 72, 84};
    for (int n = 0; n < 5; n++)
    {
        if (n > 0)
            render(timing.quickNoteMs[n][0] - timing.quickNoteMs[n - 1][1]);
        dsp.NoteOn(testNotes[n], 100);
        render(timing.quickNoteMs[n][1] - timing.quickNoteMs[n][0]);
        dsp.NoteOff(testNotes[n]);
    }

    audio.resize(pos);
    return audio;
}

// ============================================================
// FFT (Cooley-Tukey radix-2)
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
// Analysis functions
// ============================================================

// RMS envelope in windows of windowMs
static std::vector<float> extractEnvelope(const float* data, int numSamples,
                                           int sampleRate, float windowMs = 5.f)
{
    int winSamples = std::max(1, static_cast<int>(windowMs * sampleRate / 1000.f));
    int numWindows = numSamples / winSamples;
    std::vector<float> env(numWindows);
    for (int w = 0; w < numWindows; w++)
    {
        float sum = 0;
        for (int i = 0; i < winSamples; i++)
        {
            float s = data[w * winSamples + i];
            sum += s * s;
        }
        env[w] = 10.f * log10f(sum / winSamples + 1e-30f);
    }
    return env;
}

// Find onset: first window above threshold dB below peak
static int findOnset(const std::vector<float>& env, float threshDb = 20.f)
{
    float peak = *std::max_element(env.begin(), env.end());
    float thresh = peak - threshDb;
    for (int i = 0; i < (int)env.size(); i++)
        if (env[i] >= thresh) return i;
    return 0;
}

// Spectral analysis: compute magnitude spectrum in dB
struct Spectrum {
    std::vector<float> magDb;
    float binHz;
    int size;
};

static Spectrum computeSpectrum(const float* data, int numSamples, int sampleRate)
{
    int fftSize = 1;
    while (fftSize < numSamples) fftSize <<= 1;
    if (fftSize < 8192) fftSize = 8192;

    std::vector<float> re(fftSize, 0.f), im(fftSize, 0.f);
    for (int i = 0; i < numSamples; i++)
    {
        float w = 0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (numSamples - 1)));
        re[i] = data[i] * w;
    }
    fft(re, im);

    int specSize = fftSize / 2 + 1;
    Spectrum s;
    s.binHz = static_cast<float>(sampleRate) / fftSize;
    s.size = specSize;
    s.magDb.resize(specSize);
    for (int i = 0; i < specSize; i++)
        s.magDb[i] = 20.f * log10f(sqrtf(re[i] * re[i] + im[i] * im[i]) + 1e-30f);
    return s;
}

// Spectral centroid in Hz
static float spectralCentroid(const Spectrum& s)
{
    float sumWt = 0, sumMag = 0;
    for (int i = 1; i < s.size; i++)
    {
        float mag = powf(10.f, s.magDb[i] / 20.f);
        sumWt += mag * (i * s.binHz);
        sumMag += mag;
    }
    return sumMag > 0 ? sumWt / sumMag : 0.f;
}

// RMS level in dB
static float rmsDb(const float* data, int numSamples)
{
    float sum = 0;
    for (int i = 0; i < numSamples; i++)
        sum += data[i] * data[i];
    return 10.f * log10f(sum / numSamples + 1e-30f);
}

// Peak level in dB
static float peakDb(const float* data, int numSamples)
{
    float peak = 0;
    for (int i = 0; i < numSamples; i++)
        peak = std::max(peak, std::abs(data[i]));
    return 20.f * log10f(peak + 1e-30f);
}

// Spectral RMSE: average dB difference across 1/3-octave bands
static float spectralRMSE(const Spectrum& a, const Spectrum& b, float minHz = 50.f, float maxHz = 16000.f)
{
    // 1/3-octave bands from minHz to maxHz
    float sumSq = 0;
    int count = 0;
    for (float centerHz = minHz; centerHz <= maxHz; centerHz *= powf(2.f, 1.f / 3.f))
    {
        float loHz = centerHz / powf(2.f, 1.f / 6.f);
        float hiHz = centerHz * powf(2.f, 1.f / 6.f);
        int loA = std::max(1, static_cast<int>(loHz / a.binHz));
        int hiA = std::min(a.size - 1, static_cast<int>(hiHz / a.binHz));
        int loB = std::max(1, static_cast<int>(loHz / b.binHz));
        int hiB = std::min(b.size - 1, static_cast<int>(hiHz / b.binHz));

        if (loA >= hiA || loB >= hiB) continue;

        float avgA = 0, avgB = 0;
        for (int i = loA; i <= hiA; i++) avgA += a.magDb[i];
        avgA /= (hiA - loA + 1);
        for (int i = loB; i <= hiB; i++) avgB += b.magDb[i];
        avgB /= (hiB - loB + 1);

        float diff = avgA - avgB;
        sumSq += diff * diff;
        count++;
    }
    return count > 0 ? sqrtf(sumSq / count) : 0.f;
}

// ============================================================
// Comparison
// ============================================================

static void comparePreset(const PatchInfo& patch, const char* wavDir, bool dumpWav)
{
    const char* presetName = kFactoryPresets[patch.arrayIndex].name;
    fprintf(stderr, "\n=== %s ===\n", presetName);

    // Find bank audio file (try .wav, .aif, .aiff)
    std::string bankBase = std::string(wavDir) + "/bank_" + patch.bankName;
    AudioData bankAudio;
    bool found = false;
    // Try various naming patterns: bank_A2x.wav, bank_A2x_bip.aif, etc.
    for (const char* suffix : {"", "_bip"})
    {
        if (found) break;
        for (const char* ext : {".wav", ".aif", ".aiff"})
        {
            std::string path = bankBase + suffix + ext;
            if (readAudio(path.c_str(), bankAudio)) { found = true; break; }
        }
    }
    if (!found)
    {
        fprintf(stderr, "  ERROR: cannot find bank_%s.{wav,aif,aiff} in %s\n",
                patch.bankName.c_str(), wavDir);
        return;
    }

    fprintf(stderr, "  HW: %d Hz, %d ch, %.1f sec\n",
            bankAudio.sampleRate, bankAudio.channels,
            bankAudio.numSamples / (double)bankAudio.sampleRate);

    // Compute timing for target preset
    int bankStart = kJ106Offset + patch.bankIndex * 8;
    PresetTiming timing = computeTiming(bankStart, patch.indexInBank);

    fprintf(stderr, "  Timing: chord %.0f-%.0fms, release to %.0fms, total %.0fms\n",
            timing.chordOnMs - timing.startMs,
            timing.chordOffMs - timing.startMs,
            timing.relEndMs - timing.startMs,
            timing.totalEndMs - timing.startMs);

    // Extract HW segment
    std::vector<float> hwMono = toMono(bankAudio);
    int hwStart = static_cast<int>(timing.startMs * bankAudio.sampleRate / 1000.0);
    int hwEnd = std::min(static_cast<int>(timing.totalEndMs * bankAudio.sampleRate / 1000.0),
                         bankAudio.numSamples);
    if (hwStart >= hwEnd || hwStart >= bankAudio.numSamples)
    {
        fprintf(stderr, "  ERROR: preset segment out of bounds (start=%d, total=%d)\n",
                hwStart, bankAudio.numSamples);
        return;
    }
    int hwLen = hwEnd - hwStart;
    const float* hwData = hwMono.data() + hwStart;

    // Render DSP version
    std::vector<float> dspAudio = renderPreset(patch.arrayIndex, bankAudio.sampleRate, timing);
    int dspLen = static_cast<int>(dspAudio.size());

    fprintf(stderr, "  DSP render: %d samples (%.1f sec)\n",
            dspLen, dspLen / (double)bankAudio.sampleRate);

    // --- Level normalization ---
    // Normalize both to match RMS of chord steady-state (last 50%).
    // HW level depends on interface gain; DSP level depends on master vol.
    // We report the raw level delta for reference, then normalize for comparison.
    int chordStartSamp = static_cast<int>((timing.chordOnMs - timing.startMs) * bankAudio.sampleRate / 1000.0);
    int chordEndSamp = static_cast<int>((timing.chordOffMs - timing.startMs) * bankAudio.sampleRate / 1000.0);
    int chordLen = chordEndSamp - chordStartSamp;

    // Copy HW segment so we can normalize it
    std::vector<float> hwSegment(hwData, hwData + hwLen);

    float dspGain = 1.f;
    if (chordLen > 0 && chordStartSamp + chordLen <= hwLen && chordStartSamp + chordLen <= dspLen)
    {
        int ssStart = chordStartSamp + chordLen / 2;
        int ssLen = chordLen / 2;
        float hwSsRms = rmsDb(hwSegment.data() + ssStart, ssLen);
        float dspSsRms = rmsDb(dspAudio.data() + ssStart, ssLen);
        float levelDelta = dspSsRms - hwSsRms;

        fprintf(stderr, "  Raw level delta: %+.1f dB (DSP - HW), normalizing...\n", levelDelta);

        // Normalize DSP to match HW RMS
        dspGain = powf(10.f, -levelDelta / 20.f);
        for (auto& s : dspAudio) s *= dspGain;
    }

    const float* hwData2 = hwSegment.data();
    const float* dspData = dspAudio.data();

    // Dump WAV if requested (after normalization)
    if (dumpWav)
    {
        char hwFile[256], dspFile[256];
        snprintf(hwFile, sizeof(hwFile), "%s_hw.wav", presetName);
        snprintf(dspFile, sizeof(dspFile), "%s_dsp.wav", presetName);
        for (char* p = hwFile; *p; p++) if (*p == ' ') *p = '_';
        for (char* p = dspFile; *p; p++) if (*p == ' ') *p = '_';
        writeWav24(hwFile, hwData2, hwLen, bankAudio.sampleRate);
        writeWav24(dspFile, dspData, dspLen, bankAudio.sampleRate);
    }

    if (chordLen > 0 && chordStartSamp + chordLen <= hwLen && chordStartSamp + chordLen <= dspLen)
    {
        float hwPeak = peakDb(hwData2 + chordStartSamp, chordLen);
        float dspPeak = peakDb(dspData + chordStartSamp, chordLen);
        float hwRms = rmsDb(hwData2 + chordStartSamp, chordLen);
        float dspRms = rmsDb(dspData + chordStartSamp, chordLen);

        fprintf(stderr, "\n  Chord levels:\n");
        fprintf(stderr, "    Peak:  HW %+.1f dBFS  DSP %+.1f dBFS  (delta %+.1f)\n",
                hwPeak, dspPeak, dspPeak - hwPeak);
        fprintf(stderr, "    RMS:   HW %+.1f dBFS  DSP %+.1f dBFS  (delta %+.1f)\n",
                hwRms, dspRms, dspRms - hwRms);

        // Envelope shape
        auto hwEnv = extractEnvelope(hwData2 + chordStartSamp, chordLen, bankAudio.sampleRate);
        auto dspEnv = extractEnvelope(dspData + chordStartSamp, chordLen, bankAudio.sampleRate);
        int hwOnset = findOnset(hwEnv);
        int dspOnset = findOnset(dspEnv);
        float hwOnsetMs = hwOnset * 5.f;
        float dspOnsetMs = dspOnset * 5.f;
        fprintf(stderr, "    Onset: HW %.0fms  DSP %.0fms  (delta %+.0fms)\n",
                hwOnsetMs, dspOnsetMs, dspOnsetMs - hwOnsetMs);

        // Attack RMS (first half of chord = attack portion)
        {
            int atkLen = chordLen / 2;
            if (atkLen > 0)
            {
                float hwAtkRms = rmsDb(hwData2 + chordStartSamp, atkLen);
                float dspAtkRms = rmsDb(dspData + chordStartSamp, atkLen);
                fprintf(stderr, "    AtkRMS: HW %+.1f dB  DSP %+.1f dB  (delta %+.1f)\n",
                        hwAtkRms, dspAtkRms, dspAtkRms - hwAtkRms);
                printf("%s,attack,rms_db,%.1f,%.1f,%+.1f\n", presetName, hwAtkRms, dspAtkRms, dspAtkRms - hwAtkRms);
            }
        }

        // Spectral comparison of steady-state (last 50% of chord)
        int ssStart = chordStartSamp + chordLen / 2;
        int ssLen = chordLen / 2;
        if (ssLen > 0 && ssStart + ssLen <= hwLen && ssStart + ssLen <= dspLen)
        {
            Spectrum hwSpec = computeSpectrum(hwData2 + ssStart, ssLen, bankAudio.sampleRate);
            Spectrum dspSpec = computeSpectrum(dspData + ssStart, ssLen, bankAudio.sampleRate);

            float hwCentroid = spectralCentroid(hwSpec);
            float dspCentroid = spectralCentroid(dspSpec);
            float rmse = spectralRMSE(hwSpec, dspSpec);

            fprintf(stderr, "\n  Chord spectrum (steady-state):\n");
            fprintf(stderr, "    Centroid: HW %.0f Hz  DSP %.0f Hz  (delta %+.0f)\n",
                    hwCentroid, dspCentroid, dspCentroid - hwCentroid);
            fprintf(stderr, "    Spectral RMSE: %.1f dB (1/3-octave bands)\n", rmse);

            // CSV output
            printf("%s,chord,peak_dbfs,%.1f,%.1f,%+.1f\n", presetName, hwPeak, dspPeak, dspPeak - hwPeak);
            printf("%s,chord,rms_dbfs,%.1f,%.1f,%+.1f\n", presetName, hwRms, dspRms, dspRms - hwRms);
            printf("%s,chord,centroid_hz,%.0f,%.0f,%+.0f\n", presetName, hwCentroid, dspCentroid, dspCentroid - hwCentroid);
            printf("%s,chord,spectral_rmse,,,%.1f\n", presetName, rmse);
            printf("%s,chord,onset_ms,%.0f,%.0f,%+.0f\n", presetName, hwOnsetMs, dspOnsetMs, dspOnsetMs - hwOnsetMs);
        }
    }

    // --- Release tail analysis (between chord NoteOff and first quick note) ---
    {
        int relStartSamp = chordEndSamp;
        int relEndSamp = static_cast<int>((timing.relEndMs - timing.startMs) * bankAudio.sampleRate / 1000.0);
        int relLen = relEndSamp - relStartSamp;
        int windowSamp = static_cast<int>(0.1 * bankAudio.sampleRate); // 100ms windows

        if (relLen > windowSamp * 3 && relStartSamp + relLen <= hwLen && relStartSamp + relLen <= dspLen)
        {
            // RMS at start of release (first 100ms after NoteOff)
            float hwRelStart = rmsDb(hwData2 + relStartSamp, windowSamp);
            float dspRelStart = rmsDb(dspData + relStartSamp, windowSamp);

            // RMS at end of release (last 100ms before quick notes)
            float hwRelEnd = rmsDb(hwData2 + relEndSamp - windowSamp, windowSamp);
            float dspRelEnd = rmsDb(dspData + relEndSamp - windowSamp, windowSamp);

            // Decay amount in dB
            float hwDecay = hwRelEnd - hwRelStart;
            float dspDecay = dspRelEnd - dspRelStart;

            // Decay rate in dB/s
            float relDurS = relLen / (float)bankAudio.sampleRate;
            float hwRate = hwDecay / relDurS;
            float dspRate = dspDecay / relDurS;

            fprintf(stderr, "\n  Release tail (%.0fms):\n", relDurS * 1000);
            fprintf(stderr, "    Start: HW %+.1f dB  DSP %+.1f dB\n", hwRelStart, dspRelStart);
            fprintf(stderr, "    End:   HW %+.1f dB  DSP %+.1f dB\n", hwRelEnd, dspRelEnd);
            fprintf(stderr, "    Decay: HW %+.1f dB  DSP %+.1f dB  (delta %+.1f)\n",
                    hwDecay, dspDecay, dspDecay - hwDecay);
            fprintf(stderr, "    Rate:  HW %.1f dB/s  DSP %.1f dB/s\n", hwRate, dspRate);

            // Overall RMS of entire release tail
            float hwRelRms = rmsDb(hwData2 + relStartSamp, relLen);
            float dspRelRms = rmsDb(dspData + relStartSamp, relLen);

            fprintf(stderr, "    RMS:   HW %+.1f dB  DSP %+.1f dB  (delta %+.1f)\n",
                    hwRelRms, dspRelRms, dspRelRms - hwRelRms);

            printf("%s,release,rms_db,%.1f,%.1f,%+.1f\n", presetName, hwRelRms, dspRelRms, dspRelRms - hwRelRms);
            printf("%s,release,start_db,%.1f,%.1f,%+.1f\n", presetName, hwRelStart, dspRelStart, dspRelStart - hwRelStart);
            printf("%s,release,end_db,%.1f,%.1f,%+.1f\n", presetName, hwRelEnd, dspRelEnd, dspRelEnd - hwRelStart);
            printf("%s,release,decay_db,%.1f,%.1f,%+.1f\n", presetName, hwDecay, dspDecay, dspDecay - hwDecay);
            printf("%s,release,rate_dbps,%.1f,%.1f,%+.1f\n", presetName, hwRate, dspRate, dspRate - hwRate);
        }
    }

    // --- Quick notes: spectral centroid per octave ---
    fprintf(stderr, "\n  Quick notes (spectral centroid):\n");
    static const char* noteNames[] = {"C2", "C3", "C4", "C5", "C6"};
    for (int n = 0; n < 5; n++)
    {
        int qOnSamp = static_cast<int>((timing.quickNoteMs[n][0] - timing.startMs) * bankAudio.sampleRate / 1000.0);
        int qOffSamp = static_cast<int>((timing.quickNoteMs[n][1] - timing.startMs) * bankAudio.sampleRate / 1000.0);
        int qLen = qOffSamp - qOnSamp;
        // Use latter half for steady-state
        int qSS = qOnSamp + qLen / 2;
        int qSSLen = qLen / 2;

        if (qSSLen > 0 && qSS + qSSLen <= hwLen && qSS + qSSLen <= dspLen)
        {
            Spectrum hwSpec = computeSpectrum(hwData2 + qSS, qSSLen, bankAudio.sampleRate);
            Spectrum dspSpec = computeSpectrum(dspData + qSS, qSSLen, bankAudio.sampleRate);
            float hwC = spectralCentroid(hwSpec);
            float dspC = spectralCentroid(dspSpec);
            float hwR = rmsDb(hwData2 + qSS, qSSLen);
            float dspR = rmsDb(dspData + qSS, qSSLen);

            fprintf(stderr, "    %s: centroid HW %.0f Hz  DSP %.0f Hz  (%+.0f)  level %+.1f/%+.1f dB\n",
                    noteNames[n], hwC, dspC, dspC - hwC, hwR, dspR);
            printf("%s,%s,centroid_hz,%.0f,%.0f,%+.0f\n", presetName, noteNames[n], hwC, dspC, dspC - hwC);
            printf("%s,%s,rms_dbfs,%.1f,%.1f,%+.1f\n", presetName, noteNames[n], hwR, dspR, dspR - hwR);
        }
    }
}

// ============================================================
// A/B stereo: L=HW, R=DSP, one file per bank of 8
// ============================================================

// MIDI-to-audio latency in samples (measured from waveform alignment).
// HW recordings arrive ~1600 samples late at 96 kHz due to DAW input buffering.
static constexpr int kHwDelaySamples = 1833;

// Global DSP gain for A/B renders.  Run "preset_compare calibrate wavdir/"
// to measure this from the hardware recordings, then paste the value here.
static constexpr float kAbGain = 0.259652;   // measured via "calibrate lfrancis"
static constexpr float kDetuneCents = -6.f;   // HW tuning offset (from "levels" pitch summary)

// Path to bank MIDI files (relative to preset-compare directory)
static const char* kMidiDir = "../../docs/website/106_calibration/bank_midi_106";
// Path to individual patch MIDI files
static const char* kPatchMidiDir = "../../docs/website/106_calibration/patch_midi_106";
// Path to render_midi tool (relative to preset-compare directory)
static const char* kRenderMidi = "../render-midi/render_midi";

// Single-patch A/B: L=HW left channel, R=DSP left channel
static void abPatch(const PatchInfo& patch, const char* wavDir, const char* outDir)
{
    int bankIdx = patch.bankIndex;
    char bankLetter = (bankIdx < 8) ? 'A' : 'B';
    int group = (bankIdx % 8) + 1;
    char bankName[8];
    snprintf(bankName, sizeof(bankName), "%c%dx", bankLetter, group);

    // Load HW bank audio
    std::string bankBase = std::string(wavDir) + "/bank_" + bankName;
    AudioData bankAudio;
    bool found = false;
    for (const char* suffix : {"", "_bip"})
    {
        if (found) break;
        for (const char* ext : {".wav", ".aif", ".aiff"})
        {
            std::string path = bankBase + suffix + ext;
            if (readAudio(path.c_str(), bankAudio)) { found = true; break; }
        }
    }
    if (!found)
    {
        fprintf(stderr, "  No HW bank file for %s\n", bankName);
        return;
    }

    int sr = bankAudio.sampleRate;
    int bankStart = kJ106Offset + bankIdx * 8;
    const char* presetName = kFactoryPresets[patch.arrayIndex].name;

    // Build patch MIDI filename: "A11_Brass.mid" from "A11 Brass"
    std::string midiName(presetName);
    for (auto& c : midiName) if (c == ' ') c = '_';
    char midiFile[512], dspTmpFile[512];
    snprintf(midiFile, sizeof(midiFile), "%s/%s.mid", kPatchMidiDir, midiName.c_str());
    snprintf(dspTmpFile, sizeof(dspTmpFile), "%s/.tmp_dsp_%s.wav", outDir, midiName.c_str());

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s \"%s\" \"%s\" %d --raw --gain=%.6f --detune=%.1f",
             kRenderMidi, midiFile, dspTmpFile, sr, kAbGain, kDetuneCents);
    fprintf(stderr, "  Rendering %s via render_midi...\n", presetName);
    int ret = system(cmd);
    if (ret != 0)
    {
        fprintf(stderr, "  render_midi failed (exit %d)\n", ret);
        return;
    }

    AudioData dspAudio;
    if (!readAudio(dspTmpFile, dspAudio))
    {
        fprintf(stderr, "  Failed to read DSP render\n");
        return;
    }
    remove(dspTmpFile);

    // Extract HW segment for this patch
    PresetTiming timing = computeTiming(bankStart, patch.indexInBank);
    std::vector<float> hwLeft = leftChannel(bankAudio);
    std::vector<float> dspLeft = leftChannel(dspAudio);

    int hwStart = static_cast<int>(timing.startMs * sr / 1000.0) + kHwDelaySamples;
    int hwEnd = std::min(static_cast<int>(timing.totalEndMs * sr / 1000.0) + kHwDelaySamples,
                         bankAudio.numSamples);
    if (hwStart >= hwEnd || hwStart >= bankAudio.numSamples)
    {
        fprintf(stderr, "  HW segment out of bounds\n");
        return;
    }
    int hwLen = hwEnd - hwStart;

    // DSP render is the full patch (not a bank), starts from 0
    int dspLen = static_cast<int>(dspLeft.size());

    int outLen = std::max(hwLen, dspLen);
    std::vector<float> L(outLen, 0.f);
    std::vector<float> R(outLen, 0.f);
    for (int i = 0; i < hwLen; i++) L[i] = hwLeft[hwStart + i];
    for (int i = 0; i < dspLen; i++) R[i] = dspLeft[i];

    char outFile[512];
    snprintf(outFile, sizeof(outFile), "%s/%s_AB.wav", outDir, midiName.c_str());
    writeWavStereo24(outFile, L.data(), R.data(), outLen, sr);
}

static void abBank(int bankIdx, const char* wavDir, const char* outDir)
{
    char bankLetter = (bankIdx < 8) ? 'A' : 'B';
    int group = (bankIdx % 8) + 1;
    char bankName[8];
    snprintf(bankName, sizeof(bankName), "%c%dx", bankLetter, group);

    // Load HW bank audio
    std::string bankBase = std::string(wavDir) + "/bank_" + bankName;
    AudioData bankAudio;
    bool found = false;
    for (const char* suffix : {"", "_bip"})
    {
        if (found) break;
        for (const char* ext : {".wav", ".aif", ".aiff"})
        {
            std::string path = bankBase + suffix + ext;
            if (readAudio(path.c_str(), bankAudio)) { found = true; break; }
        }
    }
    if (!found)
    {
        fprintf(stderr, "  Skipping %s (no bank file)\n", bankName);
        return;
    }

    int sr = bankAudio.sampleRate;

    // Render DSP via render_midi (uses same DSP path as the plugin)
    char midiFile[512], dspTmpFile[512];
    snprintf(midiFile, sizeof(midiFile), "%s/bank_%s.mid", kMidiDir, bankName);
    snprintf(dspTmpFile, sizeof(dspTmpFile), "%s/.tmp_dsp_%s.wav", outDir, bankName);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s \"%s\" \"%s\" %d --raw --gain=%.6f --detune=%.1f",
             kRenderMidi, midiFile, dspTmpFile, sr, kAbGain, kDetuneCents);
    fprintf(stderr, "  Rendering DSP via render_midi...\n");
    int ret = system(cmd);
    if (ret != 0)
    {
        fprintf(stderr, "  render_midi failed (exit %d)\n", ret);
        return;
    }

    // Read back the DSP render
    AudioData dspBank;
    if (!readAudio(dspTmpFile, dspBank))
    {
        fprintf(stderr, "  Failed to read DSP render %s\n", dspTmpFile);
        return;
    }
    remove(dspTmpFile);

    std::vector<float> hwLeft = leftChannel(bankAudio);
    std::vector<float> dspLeft = leftChannel(dspBank);
    int bankStart = kJ106Offset + bankIdx * 8;

    // Accumulate all 8 presets into L=HW, R=DSP (same chorus phase)
    std::vector<float> allL, allR;

    for (int p = 0; p < 8; p++)
    {
        int arrayIdx = bankStart + p;
        const char* presetName = kFactoryPresets[arrayIdx].name;
        PresetTiming timing = computeTiming(bankStart, p);

        // HW segment (offset by DAW input latency)
        int hwStart = static_cast<int>(timing.startMs * sr / 1000.0) + kHwDelaySamples;
        int hwEnd = std::min(static_cast<int>(timing.totalEndMs * sr / 1000.0) + kHwDelaySamples,
                             bankAudio.numSamples);
        if (hwStart >= hwEnd || hwStart >= bankAudio.numSamples)
        {
            fprintf(stderr, "    %s: HW out of bounds, skipping\n", presetName);
            continue;
        }
        int hwLen = hwEnd - hwStart;

        // DSP segment (no latency offset -- render_midi output is sample-aligned)
        int dspStart = static_cast<int>(timing.startMs * sr / 1000.0);
        int dspEnd = std::min(static_cast<int>(timing.totalEndMs * sr / 1000.0),
                              dspBank.numSamples);
        if (dspStart >= dspEnd || dspStart >= dspBank.numSamples)
        {
            fprintf(stderr, "    %s: DSP out of bounds, skipping\n", presetName);
            continue;
        }
        int dspLen = dspEnd - dspStart;

        std::vector<float> hwSeg(hwLeft.data() + hwStart, hwLeft.data() + hwStart + hwLen);
        std::vector<float> dspSeg(dspLeft.data() + dspStart, dspLeft.data() + dspStart + dspLen);

        // kAbGain already applied by render_midi via --gain flag

        int outLen = std::max(hwLen, dspLen);
        hwSeg.resize(outLen, 0.f);
        dspSeg.resize(outLen, 0.f);

        allL.insert(allL.end(), hwSeg.begin(), hwSeg.end());
        allR.insert(allR.end(), dspSeg.begin(), dspSeg.end());

        fprintf(stderr, "    %s (%.1f sec)\n", presetName, outLen / (float)sr);
    }

    if (allL.empty()) return;

    char outFile[512];
    snprintf(outFile, sizeof(outFile), "%s/bank_%s_AB.wav", outDir, bankName);
    writeWavStereo24(outFile, allL.data(), allR.data(), static_cast<int>(allL.size()), sr);
}

// ============================================================
// Calibrate: measure global DSP-to-HW gain from all presets
// ============================================================

static void calibrate(const char* wavDir)
{
    std::vector<float> deltas; // dB deltas (DSP - HW) for each preset

    for (int bankIdx = 0; bankIdx < 16; bankIdx++)
    {
        char bankLetter = (bankIdx < 8) ? 'A' : 'B';
        int group = (bankIdx % 8) + 1;
        char bankName[8];
        snprintf(bankName, sizeof(bankName), "%c%dx", bankLetter, group);

        // Load HW bank audio
        std::string bankBase = std::string(wavDir) + "/bank_" + bankName;
        AudioData bankAudio;
        bool found = false;
        for (const char* suffix : {"", "_bip"})
        {
            if (found) break;
            for (const char* ext : {".wav", ".aif", ".aiff"})
            {
                std::string path = bankBase + suffix + ext;
                if (readAudio(path.c_str(), bankAudio)) { found = true; break; }
            }
        }
        if (!found)
        {
            fprintf(stderr, "  Skipping %s (no bank file)\n", bankName);
            continue;
        }

        int sr = bankAudio.sampleRate;

        // Render DSP via render_midi
        char midiFile[512], dspTmpFile[512];
        snprintf(midiFile, sizeof(midiFile), "%s/bank_%s.mid", kMidiDir, bankName);
        snprintf(dspTmpFile, sizeof(dspTmpFile), "/tmp/cal_dsp_%s.wav", bankName);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "%s \"%s\" \"%s\" %d --raw --detune=%.1f 2>/dev/null",
                 kRenderMidi, midiFile, dspTmpFile, sr, kDetuneCents);
        if (system(cmd) != 0)
        {
            fprintf(stderr, "  render_midi failed for %s\n", bankName);
            continue;
        }

        AudioData dspBank;
        if (!readAudio(dspTmpFile, dspBank))
        {
            fprintf(stderr, "  Failed to read DSP render for %s\n", bankName);
            continue;
        }
        remove(dspTmpFile);

        std::vector<float> hwLeft = leftChannel(bankAudio);
        std::vector<float> dspLeft = leftChannel(dspBank);
        int bankStart = kJ106Offset + bankIdx * 8;

        for (int p = 0; p < 8; p++)
        {
            int arrayIdx = bankStart + p;
            const char* presetName = kFactoryPresets[arrayIdx].name;
            PresetTiming timing = computeTiming(bankStart, p);

            int hwStart = static_cast<int>(timing.startMs * sr / 1000.0) + kHwDelaySamples;
            int hwEnd = std::min(static_cast<int>(timing.totalEndMs * sr / 1000.0) + kHwDelaySamples,
                                 bankAudio.numSamples);
            if (hwStart >= hwEnd || hwStart >= bankAudio.numSamples) continue;
            int hwLen = hwEnd - hwStart;

            int dspStart = static_cast<int>(timing.startMs * sr / 1000.0);
            int dspEnd = std::min(static_cast<int>(timing.totalEndMs * sr / 1000.0),
                                  dspBank.numSamples);
            if (dspStart >= dspEnd || dspStart >= dspBank.numSamples) continue;
            int dspLen = dspEnd - dspStart;

            int chordStartSamp = static_cast<int>((timing.chordOnMs - timing.startMs) * sr / 1000.0);
            int chordEndSamp = static_cast<int>((timing.chordOffMs - timing.startMs) * sr / 1000.0);
            int chordLen = chordEndSamp - chordStartSamp;

            if (chordLen > 0 && chordStartSamp + chordLen <= hwLen && chordStartSamp + chordLen <= dspLen)
            {
                int ssStart = chordStartSamp + chordLen / 2;
                int ssLen = chordLen / 2;
                float hwRms = rmsDb(hwLeft.data() + hwStart + ssStart, ssLen);
                float dspRms = rmsDb(dspLeft.data() + dspStart + ssStart, ssLen);
                float delta = dspRms - hwRms;
                deltas.push_back(delta);
                fprintf(stderr, "  %s: DSP %+.1f dB  HW %+.1f dB  delta %+.1f dB\n",
                        presetName, dspRms, hwRms, delta);
            }
        }
    }

    if (deltas.empty())
    {
        fprintf(stderr, "No valid presets found.\n");
        return;
    }

    std::sort(deltas.begin(), deltas.end());
    float median = deltas[deltas.size() / 2];
    float gain = powf(10.f, -median / 20.f);

    fprintf(stderr, "\n%zu presets measured.\n", deltas.size());
    fprintf(stderr, "Median delta: %+.2f dB (DSP - HW)\n", median);
    fprintf(stderr, "Set kAbGain = %.6ff\n", gain);
}

// ============================================================
// Levels: per-note amplitude comparison across all presets
// ============================================================
// Uses kAbGain (no per-preset normalization) so deltas show
// true amplitude match including cross-preset consistency.

static void levels(const char* wavDir)
{
    printf("preset,segment,hw_rms,dsp_rms,delta_db,hw_hz,dsp_hz,delta_cents\n");

    int nPresets = 0, nSegments = 0;
    std::vector<float> allDeltas;
    std::vector<float> chordDeltas;
    std::vector<float> noteDeltas[5];
    std::vector<float> noteCents[5]; // pitch offset per octave

    static const char* segNames[] = {"chord", "C2", "C3", "C4", "C5", "C6"};
    // Expected frequencies for quick notes (MIDI 36,48,60,72,84 at 16')
    static constexpr float kExpectedHz[] = {65.41f, 130.81f, 261.63f, 523.25f, 1046.50f};

    // Autocorrelation pitch detection on steady-state audio
    auto detectPitch = [](const float* data, int numSamples, int sr,
                          float minHz, float maxHz) -> float
    {
        // Remove DC
        float dc = 0;
        for (int i = 0; i < numSamples; i++) dc += data[i];
        dc /= numSamples;

        int minLag = std::max(1, static_cast<int>(sr / maxHz));
        int maxLag = std::min(numSamples / 2, static_cast<int>(sr / minHz));
        if (maxLag <= minLag) return 0;

        // Normalized autocorrelation
        float energy = 0;
        for (int i = 0; i < numSamples; i++)
        {
            float s = data[i] - dc;
            energy += s * s;
        }
        if (energy < 1e-10f) return 0;

        // Find first valley then highest peak
        float bestVal = -1;
        int bestLag = minLag;
        bool inValley = false;

        for (int lag = minLag; lag <= maxLag; lag++)
        {
            float sum = 0;
            for (int i = 0; i < numSamples - lag; i++)
                sum += (data[i] - dc) * (data[i + lag] - dc);
            float corr = sum / energy;

            if (corr < 0.3f) inValley = true;
            if (inValley && corr > bestVal)
            {
                bestVal = corr;
                bestLag = lag;
            }
        }

        if (bestVal < 0.3f) return 0; // no clear pitch
        return static_cast<float>(sr) / bestLag;
    };

    for (int bankIdx = 0; bankIdx < 16; bankIdx++)
    {
        char bankLetter = (bankIdx < 8) ? 'A' : 'B';
        int group = (bankIdx % 8) + 1;
        char bankName[8];
        snprintf(bankName, sizeof(bankName), "%c%dx", bankLetter, group);

        std::string bankBase = std::string(wavDir) + "/bank_" + bankName;
        AudioData bankAudio;
        bool found = false;
        for (const char* suffix : {"", "_bip"})
        {
            if (found) break;
            for (const char* ext : {".wav", ".aif", ".aiff"})
            {
                std::string path = bankBase + suffix + ext;
                if (readAudio(path.c_str(), bankAudio)) { found = true; break; }
            }
        }
        if (!found) { fprintf(stderr, "  Skipping %s\n", bankName); continue; }

        int sr = bankAudio.sampleRate;
        char midiFile[512], dspTmpFile[512];
        snprintf(midiFile, sizeof(midiFile), "%s/bank_%s.mid", kMidiDir, bankName);
        snprintf(dspTmpFile, sizeof(dspTmpFile), "/tmp/levels_dsp_%s.wav", bankName);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "%s \"%s\" \"%s\" %d --raw --gain=%.6f --detune=%.1f 2>/dev/null",
                 kRenderMidi, midiFile, dspTmpFile, sr, kAbGain, kDetuneCents);
        if (system(cmd) != 0) { fprintf(stderr, "  render_midi failed for %s\n", bankName); continue; }

        AudioData dspBank;
        if (!readAudio(dspTmpFile, dspBank)) { fprintf(stderr, "  read failed %s\n", bankName); continue; }
        remove(dspTmpFile);

        std::vector<float> hwLeft = leftChannel(bankAudio);
        std::vector<float> dspLeft = leftChannel(dspBank);
        int bankStart = kJ106Offset + bankIdx * 8;

        for (int p = 0; p < 8; p++)
        {
            int arrayIdx = bankStart + p;
            const char* presetName = kFactoryPresets[arrayIdx].name;
            PresetTiming timing = computeTiming(bankStart, p);

            int hwOfs = static_cast<int>(timing.startMs * sr / 1000.0) + kHwDelaySamples;
            int dspOfs = static_cast<int>(timing.startMs * sr / 1000.0);

            // Chord steady-state (last 50%)
            int chordOn = static_cast<int>((timing.chordOnMs - timing.startMs) * sr / 1000.0);
            int chordOff = static_cast<int>((timing.chordOffMs - timing.startMs) * sr / 1000.0);
            int chordLen = chordOff - chordOn;
            int ssStart = chordOn + chordLen / 2;
            int ssLen = chordLen / 2;

            if (ssLen > 0 &&
                hwOfs + ssStart + ssLen <= bankAudio.numSamples &&
                dspOfs + ssStart + ssLen <= dspBank.numSamples)
            {
                float hwR = rmsDb(hwLeft.data() + hwOfs + ssStart, ssLen);
                float dspR = rmsDb(dspLeft.data() + dspOfs + ssStart, ssLen);
                float delta = dspR - hwR;
                printf("%s,chord,%.2f,%.2f,%+.2f,,,\n", presetName, hwR, dspR, delta);
                allDeltas.push_back(delta);
                chordDeltas.push_back(delta);
                nSegments++;
            }

            // Quick notes: C2, C3, C4, C5, C6 (steady-state last 50%)
            for (int n = 0; n < 5; n++)
            {
                int qOn = static_cast<int>((timing.quickNoteMs[n][0] - timing.startMs) * sr / 1000.0);
                int qOff = static_cast<int>((timing.quickNoteMs[n][1] - timing.startMs) * sr / 1000.0);
                int qLen = qOff - qOn;
                int qSS = qOn + qLen / 2;
                int qSSLen = qLen / 2;

                if (qSSLen > 0 &&
                    hwOfs + qSS + qSSLen <= bankAudio.numSamples &&
                    dspOfs + qSS + qSSLen <= dspBank.numSamples)
                {
                    float hwR = rmsDb(hwLeft.data() + hwOfs + qSS, qSSLen);
                    float dspR = rmsDb(dspLeft.data() + dspOfs + qSS, qSSLen);
                    float delta = dspR - hwR;

                    // Pitch detection
                    float expected = kExpectedHz[n];
                    float minHz = expected * 0.5f;
                    float maxHz = expected * 2.0f;
                    float hwHz = detectPitch(hwLeft.data() + hwOfs + qSS, qSSLen,
                                             sr, minHz, maxHz);
                    float dspHz = detectPitch(dspLeft.data() + dspOfs + qSS, qSSLen,
                                              sr, minHz, maxHz);
                    float cents = 0;
                    if (hwHz > 0 && dspHz > 0)
                    {
                        cents = 1200.f * log2f(dspHz / hwHz);
                        noteCents[n].push_back(cents);
                    }

                    printf("%s,%s,%.2f,%.2f,%+.2f,%.1f,%.1f,%+.1f\n",
                           presetName, segNames[n + 1], hwR, dspR, delta,
                           hwHz, dspHz, cents);
                    allDeltas.push_back(delta);
                    noteDeltas[n].push_back(delta);
                    nSegments++;
                }
            }
            nPresets++;
        }
        fprintf(stderr, "  %s done\n", bankName);
    }

    auto stats = [](const char* label, const std::vector<float>& v) {
        if (v.empty()) return;
        float sum = 0, sumSq = 0, mn = v[0], mx = v[0];
        for (float x : v) { sum += x; sumSq += x*x; mn = std::min(mn, x); mx = std::max(mx, x); }
        float mean = sum / v.size();
        float sd = sqrtf(std::max(0.f, sumSq / v.size() - mean * mean));
        fprintf(stderr, "  %-8s  mean=%+.2f  std=%.2f  min=%+.2f  max=%+.2f  (n=%d)\n",
                label, mean, sd, mn, mx, (int)v.size());
    };

    fprintf(stderr, "\n=== AMPLITUDE SUMMARY (%d presets, %d segments) ===\n", nPresets, nSegments);
    stats("ALL", allDeltas);
    stats("chord", chordDeltas);
    static const char* nn[] = {"C2", "C3", "C4", "C5", "C6"};
    for (int n = 0; n < 5; n++)
        stats(nn[n], noteDeltas[n]);

    fprintf(stderr, "\n=== PITCH SUMMARY (cents, DSP - HW, positive = DSP sharp) ===\n");
    for (int n = 0; n < 5; n++)
        stats(nn[n], noteCents[n]);

    // Overall pitch offset
    std::vector<float> allCents;
    for (int n = 0; n < 5; n++)
        allCents.insert(allCents.end(), noteCents[n].begin(), noteCents[n].end());
    stats("ALL", allCents);
}

// ============================================================
// Notes: per-bank stereo files, quick notes only (no chords).
//   Each note rendered as: HW stereo → DSP stereo, back to back.
//   Listener hears hardware then DSP for each note in sequence.
//   One file per bank (16 files, 8 presets × 5 notes each).
// ============================================================

static void notes(const char* wavDir, const char* outDir)
{
    for (int bankIdx = 0; bankIdx < 16; bankIdx++)
    {
        char bankLetter = (bankIdx < 8) ? 'A' : 'B';
        int group = (bankIdx % 8) + 1;
        char bankName[8];
        snprintf(bankName, sizeof(bankName), "%c%dx", bankLetter, group);

        // Load HW bank audio (stereo)
        std::string bankBase = std::string(wavDir) + "/bank_" + bankName;
        AudioData bankAudio;
        bool found = false;
        for (const char* suffix : {"", "_bip"})
        {
            if (found) break;
            for (const char* ext : {".wav", ".aif", ".aiff"})
            {
                std::string path = bankBase + suffix + ext;
                if (readAudio(path.c_str(), bankAudio)) { found = true; break; }
            }
        }
        if (!found) { fprintf(stderr, "  Skipping %s\n", bankName); continue; }

        int sr = bankAudio.sampleRate;
        int hwCh = bankAudio.channels;

        // Render DSP (stereo — chorus produces stereo output)
        char midiFile[512], dspTmpFile[512];
        snprintf(midiFile, sizeof(midiFile), "%s/bank_%s.mid", kMidiDir, bankName);
        snprintf(dspTmpFile, sizeof(dspTmpFile), "/tmp/notes_dsp_%s.wav", bankName);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "%s \"%s\" \"%s\" %d --raw --gain=%.6f --detune=%.1f 2>/dev/null",
                 kRenderMidi, midiFile, dspTmpFile, sr, kAbGain, kDetuneCents);
        if (system(cmd) != 0) { fprintf(stderr, "  render_midi failed for %s\n", bankName); continue; }

        AudioData dspBank;
        if (!readAudio(dspTmpFile, dspBank)) { fprintf(stderr, "  read failed %s\n", bankName); continue; }
        remove(dspTmpFile);

        int dspCh = dspBank.channels;
        int bankStart = kJ106Offset + bankIdx * 8;

        std::vector<float> allL, allR;

        for (int p = 0; p < 8; p++)
        {
            int arrayIdx = bankStart + p;
            const char* presetName = kFactoryPresets[arrayIdx].name;
            PresetTiming timing = computeTiming(bankStart, p);

            int hwOfs = static_cast<int>(timing.startMs * sr / 1000.0) + kHwDelaySamples;
            int dspOfs = static_cast<int>(timing.startMs * sr / 1000.0);

            for (int n = 0; n < 5; n++)
            {
                int qRelOn = static_cast<int>((timing.quickNoteMs[n][0] - timing.startMs) * sr / 1000.0);

                int noteLen;
                if (n < 4)
                    noteLen = static_cast<int>((timing.quickNoteMs[n+1][0] - timing.quickNoteMs[n][0]) * sr / 1000.0);
                else
                    noteLen = static_cast<int>((timing.quickNoteMs[n][1] - timing.quickNoteMs[n][0] + 500.0) * sr / 1000.0);

                int hwStart = hwOfs + qRelOn;
                int dspStart = dspOfs + qRelOn;

                // Bounds check
                int hwAvail = std::max(0, bankAudio.numSamples - hwStart);
                int dspAvail = std::max(0, dspBank.numSamples - dspStart);
                int safeLen = std::min(noteLen, std::min(hwAvail, dspAvail));
                if (safeLen <= 0) continue;

                // HW note (stereo)
                for (int i = 0; i < safeLen; i++)
                {
                    int idx = hwStart + i;
                    if (hwCh >= 2)
                    {
                        allL.push_back(bankAudio.data[idx * hwCh]);
                        allR.push_back(bankAudio.data[idx * hwCh + 1]);
                    }
                    else
                    {
                        allL.push_back(bankAudio.data[idx]);
                        allR.push_back(bankAudio.data[idx]);
                    }
                }

                // DSP note (stereo)
                for (int i = 0; i < safeLen; i++)
                {
                    int idx = dspStart + i;
                    if (dspCh >= 2)
                    {
                        allL.push_back(dspBank.data[idx * dspCh]);
                        allR.push_back(dspBank.data[idx * dspCh + 1]);
                    }
                    else
                    {
                        allL.push_back(dspBank.data[idx]);
                        allR.push_back(dspBank.data[idx]);
                    }
                }
            }

            fprintf(stderr, "    %s\n", presetName);
        }

        if (allL.empty()) continue;

        char outFile[512];
        snprintf(outFile, sizeof(outFile), "%s/notes_%s.wav", outDir, bankName);
        writeWavStereo24(outFile, allL.data(), allR.data(), static_cast<int>(allL.size()), sr);
        fprintf(stderr, "  %s: %.1f sec\n", bankName, allL.size() / (float)sr);
    }
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  preset_compare compare A23 wavdir/ [--wav]\n");
        fprintf(stderr, "  preset_compare batch wavdir/\n");
        fprintf(stderr, "  preset_compare ab wavdir/ outdir/           All 16 banks\n");
        fprintf(stderr, "  preset_compare ab wavdir/ outdir/ A2        Single bank\n");
        fprintf(stderr, "  preset_compare ab wavdir/ outdir/ A23       Single patch (in bank file)\n");
        fprintf(stderr, "  preset_compare calibrate wavdir/\n");
        fprintf(stderr, "  preset_compare levels wavdir/              Per-note amplitude CSV\n");
        fprintf(stderr, "  preset_compare notes wavdir/ [outdir/]      Per-bank notes, HW then DSP\n");
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "compare" && argc >= 4)
    {
        PatchInfo patch;
        if (!parsePatchName(argv[2], patch))
        {
            fprintf(stderr, "Error: invalid patch name '%s' (expected e.g. A23, B51)\n", argv[2]);
            return 1;
        }
        const char* wavDir = argv[3];
        bool dumpWav = (argc > 4 && strcmp(argv[4], "--wav") == 0);

        // CSV header
        printf("preset,region,metric,hw,dsp,delta\n");
        comparePreset(patch, wavDir, dumpWav);
    }
    else if (mode == "batch" && argc >= 3)
    {
        const char* wavDir = argv[2];
        printf("preset,region,metric,hw,dsp,delta\n");

        for (int bank = 0; bank < 16; bank++)
        {
            for (int p = 0; p < 8; p++)
            {
                PatchInfo patch;
                patch.bankIndex = bank;
                patch.indexInBank = p;
                patch.arrayIndex = kJ106Offset + bank * 8 + p;

                char bankName[8];
                char bankLetter = (bank < 8) ? 'A' : 'B';
                int group = (bank % 8) + 1;
                snprintf(bankName, sizeof(bankName), "%c%dx", bankLetter, group);
                patch.bankName = bankName;

                comparePreset(patch, wavDir, false);
            }
        }
    }
    else if (mode == "ab" && argc >= 3)
    {
        const char* wavDir = argv[2];
        const char* outDir = (argc > 3 && argv[3][0] != '-') ? argv[3] : "ab_stereo";

        // Check for optional patch filter (e.g. "ab lfrancis ab_stereo A2" or "ab lfrancis ab_stereo A23")
        const char* filter = nullptr;
        for (int a = 3; a < argc; a++)
            if (argv[a][0] != '-' && a > 2 && strcmp(argv[a], outDir) != 0)
                filter = argv[a];
        if (argc > 4) filter = argv[4];

        if (filter)
        {
            int flen = static_cast<int>(strlen(filter));
            if (flen == 2)
            {
                // Bank filter: "A2" → render bank A2x
                char bank = toupper(filter[0]);
                int group = filter[1] - '0';
                if ((bank == 'A' || bank == 'B') && group >= 1 && group <= 8)
                {
                    int bankIdx = (bank == 'A' ? 0 : 8) + (group - 1);
                    fprintf(stderr, "\n=== Bank %c%d ===\n", bank, group);
                    abBank(bankIdx, wavDir, outDir);
                }
                else
                    fprintf(stderr, "Invalid bank filter: %s\n", filter);
            }
            else if (flen == 3)
            {
                // Patch filter: "A23" → render single patch as stereo L=HW R=DSP
                PatchInfo patch;
                if (parsePatchName(filter, patch))
                    abPatch(patch, wavDir, outDir);
                else
                    fprintf(stderr, "Invalid patch name: %s\n", filter);
            }
            else
                fprintf(stderr, "Invalid filter: %s (use e.g. A2 for bank or A23 for patch)\n", filter);
        }
        else
        {
            for (int bank = 0; bank < 16; bank++)
            {
                char bankLetter = (bank < 8) ? 'A' : 'B';
                int group = (bank % 8) + 1;
                fprintf(stderr, "\n=== Bank %c%d ===\n", bankLetter, group);
                abBank(bank, wavDir, outDir);
            }
            fprintf(stderr, "\nCreated 16 bank A/B files in %s/\n", outDir);
        }
    }
    else if (mode == "calibrate" && argc >= 3)
    {
        calibrate(argv[2]);
    }
    else if (mode == "levels" && argc >= 3)
    {
        levels(argv[2]);
    }
    else if (mode == "notes" && argc >= 3)
    {
        const char* wavDir = argv[2];
        const char* outDir = (argc > 3) ? argv[3] : "notes_ab";
        notes(wavDir, outDir);
    }
    else
    {
        fprintf(stderr, "Unknown mode: %s\n", mode.c_str());
        return 1;
    }

    return 0;
}
