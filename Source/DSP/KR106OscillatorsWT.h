#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

// Oscillator mix constants (from hardware measurements)
namespace kr106 {

// J6/J60: BA662 VCA per waveform, calibrated from Juno-6 recordings
static constexpr float kSawAmpJ6    = 0.5f;      // SAW is 0v to 12v
static constexpr float kPulseAmpJ6  = 0.5f;      // PULSE is 0v to +12v TL074
static constexpr float kSubAmpJ6    = 0.5942f;   // SUB at +1.5 dB over saw (J6 measurement)
static constexpr float kNoiseAmpJ6  = 1.f;       // Noise level (J6 calibration)

// J106: MC5534 pre-mixed saw/pulse, separate sub and noise mixing resistors.
// Calibrated from hardware 106_calibration recording (peak-to-peak matched
// to noise RMS at HPF flat, VCF wide open).
// Sub/pulse ratio: 1.51x from osc_calibrate recording (both square waves,
// ratio is recording-gain-independent).
static constexpr float kSawAmpJ106   = 0.5f;     // scaled to match J6 saw level
static constexpr float kPulseAmpJ106 = 0.417f;   // 0.834x saw (hardware measurement)
static constexpr float kSubAmpJ106   = 0.630f;   // 1.51x pulse, +5.3 dB vs saw (hardware cal)
static constexpr float kNoiseAmpJ106 = 1.385f;   // 2.77x saw (39K mixing R, hardware cal)

static constexpr float kSwitchRamp = 1.f / 64.f; // ~1.5ms at 44.1k
} // namespace kr106

// Bandlimited wavetable oscillators modeled on the Juno-6/106 DCO
//
// Zero-aliasing alternative to the PolyBLEP oscillator. Waveforms are
// generated from precomputed additive-synthesis saw tables (one per
// octave), with Hermite interpolation and octave crossfading.
//
// Saw:   direct table lookup with optional phase curvature
// Pulse: saw(phase) - saw(phase - pw) — bandlimited by construction
// Sub:   saw(subPhase - 0.5) - saw(subPhase) at half frequency
// Noise: identical to PolyBLEP version (LCG + CLT + TPT lowpass)
//
// The saw table is the only stored waveform. Pulse and sub are derived
// algebraically from saw lookups, inheriting the band-limiting. This
// means continuous PW modulation works perfectly with no additional
// tables or interpolation.
//
// With wavetable oscillators, oversampling is unnecessary for alias
// suppression — the tables contain no energy above Nyquist by
// construction. Oversampling may still be needed for the VCF's
// nonlinear accuracy, but the oscillators themselves are clean at 1x.

namespace kr106 {

// Shared bandlimited saw wavetable storage.
// Allocate one instance (e.g. in the DSP class) and pass a pointer
// to each voice's OscillatorsWT. Read-only after Init().
struct SawTables {
  static constexpr int kSize = 4096;
  static constexpr int kMask = kSize - 1;
  static constexpr int kNumTables = 11; // octaves 0–10 (C0 through ~C10)
  static constexpr float kBaseFreq = 16.352f; // C0 in Hz

  float tables[kNumTables][kSize] = {};
  float sampleRate = 44100.f;

  // Generate all tables via additive synthesis.
  // Table t is safe for fundamentals up to kBaseFreq * 2^(t+1).
  // Each table contains a bandlimited saw: f(phase) ≈ 2*phase - 1,
  // with harmonics truncated at Nyquist for that octave's ceiling.
  // Uses double precision accumulation to keep numerical error below
  // -140 dB. Runs once at startup (~50ms at 44.1 kHz).
  void Init(float sr) {
    sampleRate = sr;
    float nyquist = sr * 0.5f;
    InitSincKernel();

    for (int t = 0; t < kNumTables; t++) {
      float freqBound = kBaseFreq * powf(2.f, static_cast<float>(t + 1));
      int maxH = std::max(1, static_cast<int>(nyquist / freqBound));

      for (int n = 0; n < kSize; n++) {
        double phase = static_cast<double>(n) / kSize;
        double val = 0.0;
        for (int h = 1; h <= maxH; h++)
          val -= sin(2.0 * M_PI * h * phase) / h;
        val *= 2.0 / M_PI;
        tables[t][n] = static_cast<float>(val);
      }
    }

  }

  // 16-tap windowed sinc interpolation from a cyclic table.
  // Uses a precomputed polyphase kernel (Blackman-Harris window).
  // Provides near-perfect reconstruction up to table Nyquist,
  // so all harmonics stored in the wavetable are faithfully reproduced
  // regardless of the playback-to-table sample rate ratio.
  static constexpr int kSincTaps = 16;       // taps (8 each side of center)
  static constexpr int kSincHalf = kSincTaps / 2;
  static constexpr int kSincPhases = 1024;   // fractional phase resolution
  // Polyphase kernel: kSincPhases rows × kSincTaps columns.
  // Initialized once by InitSincKernel().
  static inline float sSincKernel[kSincPhases][kSincTaps] = {};
  static inline bool sSincInit = false;

  static void InitSincKernel()
  {
    if (sSincInit) return;
    for (int p = 0; p < kSincPhases; p++)
    {
      float frac = static_cast<float>(p) / kSincPhases;
      double sum = 0.0;
      for (int t = 0; t < kSincTaps; t++)
      {
        // Tap position relative to the interpolation point.
        // t=0 is the leftmost tap (kSincHalf-1 samples before center),
        // t=kSincHalf-1 is one sample before center, t=kSincHalf is center.
        double x = static_cast<double>(t - kSincHalf + 1) - static_cast<double>(frac);

        // sinc(x)
        double s;
        if (fabs(x) < 1e-12)
          s = 1.0;
        else
          s = sin(M_PI * x) / (M_PI * x);

        // Blackman-Harris 4-term window (92 dB sidelobe rejection)
        double n = static_cast<double>(t) + 1.0 - static_cast<double>(frac);
        double wn = n / static_cast<double>(kSincTaps);
        double w = 0.35875
                 - 0.48829 * cos(2.0 * M_PI * wn)
                 + 0.14128 * cos(4.0 * M_PI * wn)
                 - 0.01168 * cos(6.0 * M_PI * wn);

        double val = s * w;
        sSincKernel[p][t] = static_cast<float>(val);
        sum += val;
      }
      // Normalize so kernel sums to 1.0 (preserves DC gain)
      float invSum = static_cast<float>(1.0 / sum);
      for (int t = 0; t < kSincTaps; t++)
        sSincKernel[p][t] *= invSum;
    }
    sSincInit = true;
  }

  // phase must be in [0, 1).
  static float Read(const float* tbl, float phase) {
    float idx = phase * kSize;
    int i = static_cast<int>(idx);
    float frac = idx - static_cast<float>(i);

    int p = static_cast<int>(frac * kSincPhases);
    if (p >= kSincPhases) p = kSincPhases - 1;
    const float* kernel = sSincKernel[p];

    float sum = 0.f;
    int base = i - kSincHalf + 1;
    for (int t = 0; t < kSincTaps; t++)
      sum += tbl[(base + t) & kMask] * kernel[t];
    return sum;
  }

  // Read with crossfade between two adjacent octave tables.
  // tblIdx and blend are precomputed from the pitch (see OctaveToTable).
  float ReadBlended(float phase, int tblIdx, float blend) const {
    phase -= floorf(phase); // wrap to [0, 1)
    float v0 = Read(tables[tblIdx], phase);
    float v1 = Read(tables[tblIdx + 1], phase);
    return v0 + (v1 - v0) * blend;
  }

  // Convert a continuous octave index to table index + blend fraction.
  // octave = log2f(freq / kBaseFreq).
  // Table t covers fundamentals up to kBaseFreq * 2^(t+1), so a note
  // at exactly the boundary should use table t, not t+1. The -1 offset
  // ensures the mapping is correct: at 261.6 Hz (octave 4.0), tblIdx=3
  // with blend=1.0, reading fully from table 3 (183 harmonics at 96k).
  static void OctaveToTable(float octave, int& tblIdx, float& blend) {
    octave = std::max(0.f, std::min(octave - 1.f, static_cast<float>(kNumTables - 2)));
    tblIdx = static_cast<int>(octave);
    blend = octave - static_cast<float>(tblIdx);
  }
};

// Wavetable oscillator — drop-in replacement for kr106::Oscillators.
// Same Process() signature, same member layout for state that Voice
// accesses directly (mPos, mSubState, mPulseInvert, gains).
struct OscillatorsWT {
  float mPos = 0.f;       // phase accumulator [0, 1)
  bool mSubState = false;  // flip-flop for sub oscillator
  float mSawGain = 1.f;   // crossfade gains for pop-free switching
  float mPulseGain = 1.f;
  float mSubGain = 0.f;
  float mSwitchRamp = kSwitchRamp;

  bool mPulseInvert = false; // J106: inverted duty cycle (see Oscillators.h)

  // Per-model mix levels (set by Voice when model changes)
  float mSawAmp   = kSawAmpJ6;
  float mPulseAmp = kPulseAmpJ6;
  float mSubAmp   = kSubAmpJ6;
  float mNoiseAmp = kNoiseAmpJ6;

  // Ramp curvature: subtle quadratic bow matching hardware measurements.
  // Applied as phase warp before table lookup. At 0.03 the spectral
  // effect is below -100 dB so it doesn't meaningfully affect aliasing.
  // I think the curve we saw was a result of my Juno6 HPF never being fully off
  static constexpr float kSawCurve = 0.00f; // disabled curve

  const SawTables* mTables = nullptr;

  void SetTables(const SawTables* tables) { mTables = tables; }

  void Init(float sampleRate) {
    mSwitchRamp = 1.f - expf(-1.f / (0.00145f * sampleRate));
    Reset();
  }

  void Reset() {
    mPos = 0.f;
    mSubState = false;
    // Don't reset mSawGain/mPulseGain/mSubGain — they ramp via mSwitchRamp
    // and resetting to 0 on every new voice causes audible fade-in artifacts
    // during rapid retriggering.
  }

  // Audio taper: 50K pot emulation (exponential curve, same for sub + noise).
  // Call once per block at the voice level; pass the result to Process.
  static float AudioTaper(float x) {
    static const float kScale = 1.f / (std::exp(3.f) - 1.f);
    return (std::exp(3.f * x) - 1.f) * kScale;
  }

  // Process one sample.
  // cps: frequency in cycles per sample (freqHz / sampleRate)
  // pulseWidth: pulse width [0.52, 0.98] (caller scales from knob/LFO)
  // sawOn, pulseOn, subOn: waveform enable switches
  // subLevel: sub oscillator mix gain (pre-tapered via AudioTaper)
  // noiseAmp: noise mix gain (pre-tapered via AudioTaper, 0 = off)
  // sync: set true on phase wraparound (for scope sync output)
  float Process(float cps, float pulseWidth, bool sawOn, bool pulseOn, bool subOn, float subLevel,
                float noiseAmp, bool& sync) {

    // --- Phase accumulator (shared by all oscillators) ---
    mPos += cps;
    sync = false;

    if (mPos >= 1.f) {
      mPos -= 1.f;
      mSubState = !mSubState;
      sync = mSubState; // sync pulse every 2 DCO cycles (sub period)
    }

    // --- Table selection (once per sample, shared by all waveforms) ---
    // Saw and pulse use the same octave; sub is one octave lower.
    float freq = cps * mTables->sampleRate;
    float octave = log2f(std::max(freq, 1.f) / SawTables::kBaseFreq);
    int sawTbl;
    float sawBlend;
    SawTables::OctaveToTable(octave, sawTbl, sawBlend);

    int subTbl;
    float subBlend;
    SawTables::OctaveToTable(octave - 1.f, subTbl, subBlend);

    // --- Saw: curved phase + table lookup ---
    // Phase curvature models the slight RC bow of the hardware capacitor
    // ramp. Applied to the saw only; pulse/sub use linear phase.
    float curvedPos = mPos * (1.f + kSawCurve * (1.f - mPos));
    float saw = mTables->ReadBlended(curvedPos, sawTbl, sawBlend);

    // --- Pulse: saw(phase) - saw(phase - pw) ---
    // Bandlimited by construction: difference of two bandlimited saws.
    // The PW curvature correction maps the hardware comparator threshold
    // (which sees the curved ramp) back to linear phase space.
    float effPW = pulseWidth;
    if (mPulseInvert) effPW = 1.f - effPW;
    effPW = std::clamp(effPW, 0.01f, 0.99f);

  //  effPW = std::max(0.01f, std::min(effPW, 0.99f));


    float pulse = mTables->ReadBlended(mPos, sawTbl, sawBlend)
                - mTables->ReadBlended(mPos - effPW, sawTbl, sawBlend)
                - (2.f * effPW - 1.f);

    // --- Sub: half-frequency square wave from saw difference ---
    // Derive a continuous sub phase from the saw phase and flip-flop state.
    // subPhase goes 0→0.5 during the first saw cycle (sub=+1), then
    // 0.5→1.0 during the second (sub=-1), forming one complete sub period.
    // The saw-difference identity at pw=0.5 gives a bandlimited square.
    float subPhase = (mPos + (mSubState ? 1.f : 0.f)) * 0.5f;
    float sub = mTables->ReadBlended(subPhase - 0.5f, subTbl, subBlend)
              - mTables->ReadBlended(subPhase, subTbl, subBlend);

    // --- Oscillator mixing ---
    mSawGain += ((sawOn ? 1.f : 0.f) - mSawGain) * mSwitchRamp;
    mPulseGain += ((pulseOn ? 1.f : 0.f) - mPulseGain) * mSwitchRamp;
    mSubGain += ((subOn ? 1.f : 0.f) - mSubGain) * mSwitchRamp;

    float out = saw * mSawAmp * mSawGain + pulse * mPulseAmp * mPulseGain +
                sub * mSubAmp * subLevel * mSubGain;

    // Noise is now mixed at the voice level from a shared source
    // (single generator for all voices, matching real hardware).
    // noiseAmp parameter is retained for API compatibility but ignored here.
    (void)noiseAmp;

    return out;
  }
};

} // namespace kr106
