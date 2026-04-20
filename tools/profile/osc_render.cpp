// osc_render -- Render raw oscillator output to WAV for inspection.
//
// Renders polyBLEP on L channel and wavetable on R channel.
// No VCF, VCA, chorus, or any other processing -- just the oscillator.
//
// USAGE:
//   osc_render [freq] [seconds] [samplerate] [output.wav]
//   Defaults: 110 Hz, 2 seconds, 96000 Hz, osc_render.wav

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include "../../Source/DSP/KR106Oscillators.h"
#include "../../Source/DSP/KR106OscillatorsWT.h"

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
}

int main(int argc, char* argv[])
{
    float freq = (argc > 1) ? static_cast<float>(atof(argv[1])) : 110.f;
    float seconds = (argc > 2) ? static_cast<float>(atof(argv[2])) : 2.f;
    float sr = (argc > 3) ? static_cast<float>(atof(argv[3])) : 96000.f;
    const char* outFile = (argc > 4) ? argv[4] : "osc_render.wav";

    int numSamples = static_cast<int>(sr * seconds);
    float cps = freq / sr;

    // Pulse at 50% duty, sub on
    float pw = 0.50f;
    bool sawOn = false, pulseOn = true, subOn = true;
    float subLevel = 1.f;

    // --- PolyBLEP ---
    kr106::Oscillators blep;
    blep.Init(sr);
    blep.mSawAmp = kr106::kSawAmpJ106;
    blep.mPulseAmp = kr106::kPulseAmpJ106;
    blep.mSubAmp = kr106::kSubAmpJ106;

    std::vector<float> blepOut(numSamples);
    for (int i = 0; i < numSamples; i++)
    {
        bool sync = false;
        blepOut[i] = blep.Process(cps, pw, sawOn, pulseOn, subOn, subLevel, 0.f, sync);
    }

    // --- Wavetable ---
    kr106::SawTables tables;
    tables.Init(sr);

    kr106::OscillatorsWT wt;
    wt.SetTables(&tables);
    wt.Init(sr);
    wt.mSawAmp = kr106::kSawAmpJ106;
    wt.mPulseAmp = kr106::kPulseAmpJ106;
    wt.mSubAmp = kr106::kSubAmpJ106;

    std::vector<float> wtOut(numSamples);
    for (int i = 0; i < numSamples; i++)
    {
        wt.PrepareBlock(cps, pw, sawOn, pulseOn, subOn, subLevel);
        bool sync = false;
        wtOut[i] = wt.ProcessSample(sync);
    }

    writeWav(outFile, blepOut.data(), wtOut.data(), numSamples, static_cast<int>(sr));

    // Peak levels
    float blepPk = 0.f, wtPk = 0.f;
    for (int i = 0; i < numSamples; i++)
    {
        blepPk = std::max(blepPk, fabsf(blepOut[i]));
        wtPk = std::max(wtPk, fabsf(wtOut[i]));
    }

    fprintf(stderr, "Rendered %s: %.0f Hz, %.1fs, %.0f Hz sample rate\n", outFile, freq, seconds, sr);
    fprintf(stderr, "  L = polyBLEP  peak=%.4f (%.1f dB)\n", blepPk, 20.f * log10f(blepPk));
    fprintf(stderr, "  R = wavetable peak=%.4f (%.1f dB)\n", wtPk, 20.f * log10f(wtPk));

    return 0;
}
