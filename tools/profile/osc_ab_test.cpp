// osc_ab_test -- A/B comparison: wavetable+upsampling vs polyBLEP at Nx
//
// Runs the full DSP chain with both oscillator modes on the same preset
// and reports timing. Optionally writes WAV output for both modes.
//
// USAGE:
//   osc_ab_test [preset] [seconds] [samplerate] [oversample] [--wav]
//   Defaults: B81 Init (index 0), 5 seconds, 96000 Hz, 4x oversample
//   --wav: write wavetable.wav and polyblep.wav for listening comparison

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>

#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"
#include "../../Source/KR106_Presets_JUCE.h"

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
  kSettingVoices, kSettingOversample, kSettingIgnoreVel,
  kSettingArpLimitKbd, kSettingArpSync, kSettingLfoSync,
  kSettingMonoRetrig, kSettingMidiSysEx,
  kArpQuantize, kLfoQuantize,
  kSettingOscMode,
  kNumParams
};

static bool isSliderParam(int i)
{
    return (i >= 0 && i <= 19) || i == 40;
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
    setParam(dsp, kAdsrMode, static_cast<float>(v[43]));
    for (int i = 0; i < 44; i++)
    {
        float val = isSliderParam(i) ? (v[i] / 127.f) : static_cast<float>(v[i]);
        setParam(dsp, i, val);
    }
    setParam(dsp, kPortaMode, 2.f);
    dsp.mMasterVol = 0.5f * 0.5f;
    dsp.mMasterVolSmooth = dsp.mMasterVol;
    dsp.mVcaLevelSmooth = dsp.mVcaLevel;
}

static void writeWav(const char* filename, const float* L, const float* R,
                     int numSamples, int sampleRate)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return; }

    int dataSize = numSamples * 2 * 3; // stereo 24-bit
    int fileSize = 44 + dataSize;
    uint8_t hdr[44] = {};
    memcpy(hdr, "RIFF", 4);
    hdr[4] = (fileSize-8)&0xFF; hdr[5]=((fileSize-8)>>8)&0xFF;
    hdr[6] = ((fileSize-8)>>16)&0xFF; hdr[7]=((fileSize-8)>>24)&0xFF;
    memcpy(hdr+8, "WAVEfmt ", 8);
    hdr[16]=16; hdr[20]=1; hdr[22]=2;
    hdr[24]=sampleRate&0xFF; hdr[25]=(sampleRate>>8)&0xFF;
    hdr[26]=(sampleRate>>16)&0xFF; hdr[27]=(sampleRate>>24)&0xFF;
    int byteRate = sampleRate * 2 * 3;
    hdr[28]=byteRate&0xFF; hdr[29]=(byteRate>>8)&0xFF;
    hdr[30]=(byteRate>>16)&0xFF; hdr[31]=(byteRate>>24)&0xFF;
    hdr[32]=6; hdr[34]=24;
    memcpy(hdr+36, "data", 4);
    hdr[40]=dataSize&0xFF; hdr[41]=(dataSize>>8)&0xFF;
    hdr[42]=(dataSize>>16)&0xFF; hdr[43]=(dataSize>>24)&0xFF;
    fwrite(hdr, 1, 44, f);

    for (int i = 0; i < numSamples; i++)
    {
        for (int c = 0; c < 2; c++)
        {
            float s = std::clamp(c == 0 ? L[i] : R[i], -1.f, 1.f);
            int32_t v = static_cast<int32_t>(s * 8388607.f);
            uint8_t b[3] = { static_cast<uint8_t>(v & 0xFF),
                             static_cast<uint8_t>((v >> 8) & 0xFF),
                             static_cast<uint8_t>((v >> 16) & 0xFF) };
            fwrite(b, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "Wrote %s\n", filename);
}

using Clock = std::chrono::high_resolution_clock;

struct RunResult {
    double ms;
    double rtFactor;
    std::vector<float> outL, outR;
};

static RunResult runTest(int presetIdx, float sr, int oversample, int oscMode,
                         float seconds, bool captureAudio)
{
    static constexpr int kBlockSize = 512;
    int totalSamples = static_cast<int>(sr * seconds);
    int totalBlocks = totalSamples / kBlockSize;
    int warmupBlocks = static_cast<int>(sr / kBlockSize); // 1 second warmup

    KR106DSP<float> dsp(6);
    dsp.Reset(sr, kBlockSize);
    dsp.SetOversample(oversample);
    loadPreset(dsp, presetIdx);

    // Zero variance for deterministic comparison
    dsp.ForEachVoice([oscMode](kr106::Voice<float>& v) {
        for (int i = 0; i < kr106::Voice<float>::kNumVarianceParams; i++)
            v.SetVariance(i, 0.f);
        v.mOscMode = oscMode;
    });

    std::vector<float> bufL(kBlockSize, 0.f);
    std::vector<float> bufR(kBlockSize, 0.f);
    float* outputs[2] = { bufL.data(), bufR.data() };

    // Trigger 6 voices
    static constexpr int kNotes[] = { 48, 52, 55, 60, 64, 67 };
    for (int n = 0; n < 6; n++)
        dsp.NoteOn(kNotes[n], 127);

    // Warmup
    for (int b = 0; b < warmupBlocks; b++)
    {
        memset(bufL.data(), 0, kBlockSize * sizeof(float));
        memset(bufR.data(), 0, kBlockSize * sizeof(float));
        dsp.ProcessBlock(nullptr, outputs, 2, kBlockSize);
    }

    RunResult result;
    if (captureAudio)
    {
        result.outL.resize(totalSamples, 0.f);
        result.outR.resize(totalSamples, 0.f);
    }

    auto t0 = Clock::now();
    int pos = 0;
    for (int b = 0; b < totalBlocks; b++)
    {
        int n = std::min(kBlockSize, totalSamples - pos);
        memset(bufL.data(), 0, n * sizeof(float));
        memset(bufR.data(), 0, n * sizeof(float));
        dsp.ProcessBlock(nullptr, outputs, 2, n);
        if (captureAudio)
        {
            memcpy(result.outL.data() + pos, bufL.data(), n * sizeof(float));
            memcpy(result.outR.data() + pos, bufR.data(), n * sizeof(float));
        }
        pos += n;
    }
    auto t1 = Clock::now();

    result.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.rtFactor = (seconds * 1000.0) / result.ms;
    return result;
}

int main(int argc, char* argv[])
{
    int presetIdx = 0;
    float seconds = 5.f;
    float sr = 96000.f;
    int oversample = 4;
    bool writeWavFiles = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--wav") == 0) { writeWavFiles = true; continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            fprintf(stderr, "Usage: osc_ab_test [preset] [seconds] [samplerate] [oversample] [--wav]\n");
            return 0;
        }
    }
    int posArg = 0;
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-') continue;
        switch (posArg++) {
            case 0: presetIdx = atoi(argv[i]); break;
            case 1: seconds = static_cast<float>(atof(argv[i])); break;
            case 2: sr = static_cast<float>(atof(argv[i])); break;
            case 3: oversample = atoi(argv[i]); break;
        }
    }

    int absIdx = 128 + presetIdx;
    if (absIdx >= kNumFactoryPresets) { fprintf(stderr, "Invalid preset\n"); return 1; }

    fprintf(stderr, "=== Oscillator A/B Test ===\n");
    fprintf(stderr, "Preset: %s (J106 index %d)\n", kFactoryPresets[absIdx].name, presetIdx);
    fprintf(stderr, "%.0f Hz, %dx oversample, %.1f sec\n\n", sr, oversample, seconds);

    // Run wavetable mode
    fprintf(stderr, "Running wavetable + upsampling...\n");
    auto wt = runTest(absIdx, sr, oversample, 0, seconds, writeWavFiles);
    fprintf(stderr, "  Wavetable: %7.1f ms (%5.1fx RT, %5.1f%% CPU)\n",
            wt.ms, wt.rtFactor, 100.0 / wt.rtFactor);

    // Run polyBLEP mode
    fprintf(stderr, "Running polyBLEP at %dx...\n", oversample);
    auto blep = runTest(absIdx, sr, oversample, 1, seconds, writeWavFiles);
    fprintf(stderr, "  PolyBLEP:  %7.1f ms (%5.1fx RT, %5.1f%% CPU)\n",
            blep.ms, blep.rtFactor, 100.0 / blep.rtFactor);

    fprintf(stderr, "\n  Difference: %+.1f%% (%s is faster)\n",
            100.0 * (blep.ms - wt.ms) / wt.ms,
            blep.ms < wt.ms ? "polyBLEP" : "wavetable");

    // CSV summary to stdout
    printf("mode,ms,rt_factor,cpu_pct\n");
    printf("wavetable,%.1f,%.1f,%.1f\n", wt.ms, wt.rtFactor, 100.0 / wt.rtFactor);
    printf("polyblep,%.1f,%.1f,%.1f\n", blep.ms, blep.rtFactor, 100.0 / blep.rtFactor);

    if (writeWavFiles)
    {
        writeWav("wavetable.wav", wt.outL.data(), wt.outR.data(),
                 static_cast<int>(wt.outL.size()), static_cast<int>(sr));
        writeWav("polyblep.wav", blep.outL.data(), blep.outR.data(),
                 static_cast<int>(blep.outL.size()), static_cast<int>(sr));
    }

    return 0;
}
