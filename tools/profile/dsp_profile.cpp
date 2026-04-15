// dsp_profile -- Standalone CPU profiler for KR106 DSP engine.
//
// Renders a fixed test scenario (6-voice chord, configurable preset)
// and reports timing breakdown: oscillator, VCF (with resampling),
// VCA/envelope, post-mix (HPF, chorus, etc).
//
// USAGE:
//   dsp_profile [samplerate] [preset] [seconds] [oversample]
//   Defaults: 96000, B81 Init (index 0), 10 seconds, 4x oversample

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

using Clock = std::chrono::high_resolution_clock;

int main(int argc, char* argv[])
{
    float sr = (argc > 1) ? static_cast<float>(atof(argv[1])) : 96000.f;
    int presetIdx = (argc > 2) ? atoi(argv[2]) : 0; // 0 = first J106 preset
    float seconds = (argc > 3) ? static_cast<float>(atof(argv[3])) : 10.f;
    int oversample = (argc > 4) ? atoi(argv[4]) : 4;

    // J106 presets start at index 128
    int absIdx = 128 + presetIdx;
    if (absIdx >= kNumFactoryPresets) { fprintf(stderr, "Invalid preset index\n"); return 1; }

    static constexpr int kBlockSize = 512;
    int totalSamples = static_cast<int>(sr * seconds);
    int totalBlocks = totalSamples / kBlockSize;

    fprintf(stderr, "=== KR106 DSP Profiler ===\n");
    fprintf(stderr, "Preset: %s (index %d)\n", kFactoryPresets[absIdx].name, presetIdx);
    fprintf(stderr, "Sample rate: %.0f Hz\n", sr);
    fprintf(stderr, "Oversample: %dx\n", oversample);
    fprintf(stderr, "Duration: %.1f sec (%d blocks of %d)\n", seconds, totalBlocks, kBlockSize);
    fprintf(stderr, "\n");

    // Initialize DSP (VCF always runs at fixed 4x internally now)
    KR106DSP<float> dsp(6);
    dsp.Reset(sr, kBlockSize);
    loadPreset(dsp, absIdx);

    // Allocate output buffers
    std::vector<float> bufL(kBlockSize, 0.f);
    std::vector<float> bufR(kBlockSize, 0.f);
    float* outputs[2] = { bufL.data(), bufR.data() };

    // Notes for 1-6 voice tests
    static constexpr int kAllNotes[] = { 48, 52, 55, 60, 64, 67 };
    int warmupBlocks = static_cast<int>(sr / kBlockSize);

    fprintf(stderr, "--- Voice count sweep ---\n");
    for (int nVoices = 1; nVoices <= 6; nVoices++)
    {
        dsp.Reset(sr, kBlockSize);
        loadPreset(dsp, absIdx);
        for (int n = 0; n < nVoices; n++)
            dsp.NoteOn(kAllNotes[n], 127);

        // Warm up
        for (int b = 0; b < warmupBlocks; b++)
        {
            memset(bufL.data(), 0, kBlockSize * sizeof(float));
            memset(bufR.data(), 0, kBlockSize * sizeof(float));
            dsp.ProcessBlock(nullptr, outputs, 2, kBlockSize);
        }

        auto t0 = Clock::now();
        for (int b = 0; b < totalBlocks; b++)
        {
            memset(bufL.data(), 0, kBlockSize * sizeof(float));
            memset(bufR.data(), 0, kBlockSize * sizeof(float));
            dsp.ProcessBlock(nullptr, outputs, 2, kBlockSize);
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double rt = (seconds * 1000.0) / ms;

        fprintf(stderr, "  %d voice%s: %7.1f ms (%5.1fx RT, %5.1f%% CPU)\n",
                nVoices, nVoices == 1 ? " " : "s", ms, rt, 100.0 / rt);
    }

    // Also measure 0 voices (post-mix only) -- fresh DSP instance
    {
        KR106DSP<float> dsp0(6);
        dsp0.Reset(sr, kBlockSize);
        auto& dsp = dsp0;
        for (int b = 0; b < warmupBlocks; b++)
        {
            memset(bufL.data(), 0, kBlockSize * sizeof(float));
            memset(bufR.data(), 0, kBlockSize * sizeof(float));
            dsp.ProcessBlock(nullptr, outputs, 2, kBlockSize);
        }
        auto t0 = Clock::now();
        for (int b = 0; b < totalBlocks; b++)
        {
            memset(bufL.data(), 0, kBlockSize * sizeof(float));
            memset(bufR.data(), 0, kBlockSize * sizeof(float));
            dsp.ProcessBlock(nullptr, outputs, 2, kBlockSize);
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        fprintf(stderr, "  0 voices: %7.1f ms (post-mix only)\n", ms);
    }

    return 0;
}
