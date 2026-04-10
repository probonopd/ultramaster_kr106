#pragma once

#include <cmath>
#include <cstdint>

// LEVELS (calibrated April 2026):
// Calibrated against the DSP saw oscillator at -12.2 dBFS rms (single
// voice at middle C, VCA and master at 0 dB, filters open) to match
// hardware S/N ratios:
//   Dry path: 67.8 dB below signal (hw: saw -8.5, dry noise -76.3 dBFS)
//   Wet path: 48.5 dB below signal (hw: chorus on, noise -57.0 dBFS)
//
// Hardware per-band breakdown over dry baseline:
//
//   band             dry        chorus    chorus-dry
//   60 Hz           -86 dB     -77 dB     +9 dB
//   120 Hz          -89 dB     -71 dB     +18 dB
//   240 Hz          -91 dB     -81 dB     +10 dB
//   1-5 kHz         -90 dB     -69 dB     +21 dB
//   5-15 kHz        -94 dB     -66 dB     +28 dB
//   15-30 kHz       -97 dB     -64 dB     +33 dB
//
// Hardware wet noise is significantly HF-tilted; modeled here with a
// +15 dB high shelf at 5 kHz on the wet broadband generator.
//
// CAVEAT: the DSP wavetable saw has less HF content than hardware (the
// hardware analog saw extends much further into the HF band, masking
// noise that the DSP saw doesn't). For dark patches, the wet HF noise
// in the DSP may sound brighter than hardware. Default user noise
// calibration should sit around 0.7x to compensate; users wanting full
// hardware realism can scale to 1.0x.

namespace kr106 {

// ============================================================
// White noise — uniform PRNG, ±1 peak, optional 1-pole HF tilt
// ============================================================
// LCG matching the previous inline implementation. Seed each instance
// differently so parallel channels decorrelate. Two filter stages: a
// post-PRNG smoothing LPF (existing), and an optional one-pole high
// shelf for the wet-path HF noise tilt.
struct AnalogFloorNoise
{
  uint32_t mSeed = 0x12345678u;
  float mLpState = 0.f;
  float mLpCoeff = 0.f;       // PRNG smoothing LPF
  float mShelfState = 0.f;    // high-shelf state
  float mShelfCoeff = 0.f;    // shelf corner coefficient (0 = disabled)
  float mShelfGain = 0.f;     // shelf high-frequency gain - 1 (0 = flat)

  void Init(float sampleRate, float cutoffHz = 20000.f)
  {
    const float fc = std::min(cutoffHz, sampleRate * 0.45f);
    mLpCoeff = 1.f - expf(-2.f * static_cast<float>(M_PI) * fc / sampleRate);
    mShelfCoeff = 0.f;
    mShelfGain = 0.f;
  }

  // Optional: enable a high-shelf to tilt the spectrum upward.
  // shelfFcHz: corner frequency of the shelf
  // hfBoostDb: boost above the shelf (e.g. 12.0 for +12 dB at HF)
  void SetHighShelf(float shelfFcHz, float hfBoostDb, float sampleRate)
  {
    const float fc = std::min(shelfFcHz, sampleRate * 0.45f);
    mShelfCoeff = 1.f - expf(-2.f * static_cast<float>(M_PI) * fc / sampleRate);
    mShelfGain  = powf(10.f, hfBoostDb / 20.f) - 1.f;
  }

  float Process()
  {
    mSeed = mSeed * 196314165u + 907633515u;
    float white = (2.f * static_cast<float>(mSeed) /
                   static_cast<float>(0xFFFFFFFFu)) - 1.f;
    mLpState += mLpCoeff * (white - mLpState);

    if (mShelfGain != 0.f)
    {
      // High shelf = input + shelf_gain * (input - lowpass(input))
      mShelfState += mShelfCoeff * (mLpState - mShelfState);
      return mLpState + mShelfGain * (mLpState - mShelfState);
    }
    return mLpState;
  }
};

// ============================================================
// Rail ripple — 120 Hz + harmonics (full-wave rectifier output)
// ============================================================
struct RailRipple
{
  float mPhase = 0.f;
  float mInc = 0.f;
  float mA1 = 0.f, mA2 = 0.f, mA3 = 0.f;

  void SetMainsHz(float mainsHz, float sampleRate)
  {
    mInc = (2.f * mainsHz) / sampleRate; // full-wave = 2× mains
  }

  void SetAmplitudes(float a1, float a2, float a3)
  {
    mA1 = a1;
    mA2 = a2;
    mA3 = a3;
  }

  void Reset() { mPhase = 0.f; }

  float Process()
  {
    mPhase += mInc;
    if (mPhase >= 1.f) mPhase -= 1.f;
    const float tp = 2.f * static_cast<float>(M_PI) * mPhase;
    return mA1 * sinf(tp) + mA2 * sinf(2.f * tp) + mA3 * sinf(3.f * tp);
  }
};

// ============================================================
// BBD clock feedthrough — sine at the current MN3009 master clock rate
// ============================================================
// Returns a unit-amplitude sine at the clock rate passed in each sample.
// Process() should be fed the instantaneous BBD master clock frequency
// from the Chorus' delay-to-clock calculation. The MN3009 master clock
// runs at 2× the per-stage transfer rate (each cycle moves a charge
// packet through 2 stages, one per phase φ1/φ2), so:
//
//   f_clk_master = 2 * N_stages / (2 * τ) = N_stages / τ
//
// For N_stages = 256 and τ = 3.30 ms, f_clk = 77.6 kHz — above Nyquist
// at 96 kHz host rate. The Process() guard self-mutes when this happens.
// At 192 kHz host rate (Nyquist 96 kHz) the clock is in-band and adds
// a faint sweeping whistle that tracks the LFO, matching real hardware.
struct BBDClockFeedthrough
{
  float mPhase = 0.f;
  float mSampleRate = 44100.f;

  void Init(float sampleRate)
  {
    mSampleRate = sampleRate;
    mPhase = 0.f;
  }

  void Reset() { mPhase = 0.f; }

  float Process(float clockHz)
  {
    const float nyq = 0.5f * mSampleRate;
    if (clockHz >= nyq) return 0.f;
    // Linear amplitude taper from 0.9*Nyquist to Nyquist
    float taper = 1.f;
    const float rolloffStart = 0.9f * nyq;
    if (clockHz > rolloffStart) taper = (nyq - clockHz) / (nyq - rolloffStart);
    mPhase += clockHz / mSampleRate;
    if (mPhase >= 1.f) mPhase -= 1.f;
    return taper * sinf(2.f * static_cast<float>(M_PI) * mPhase);
  }
};

// ============================================================
// Calibrated levels (from background_noise.wav, April 2026)
// ============================================================
namespace analog_noise {

  // Dry path — always on, injected pre-HPF in DSP.h.
  // Calibrated to give -67.8 dB S/N below the DSP saw at typical playing
  // level (-12.2 dBFS rms reference).
  constexpr float kDryBroadbandGain = 1.7e-4f;
  
  // Dry rail ripple — calibrated from hardware band measurements scaled
  // to DSP saw level. Amplitudes (not rms) — RailRipple uses sin amplitude.
  constexpr float kDryRipple120 = 3.3e-5f;
  constexpr float kDryRipple240 = 2.6e-5f;
  constexpr float kDryRipple360 = 1.2e-5f;
  
  // Wet path — chorus-on only, injected inside Chorus::Process.
  // Wet broadband baseline plus a high shelf at 5 kHz reproduces the
  // hardware HF tilt (+21 dB midband, +33 dB HF over dry).
  constexpr float kWetBroadbandGain = 3.3e-4f;
  constexpr float kWetShelfCornerHz = 5000.f;
  constexpr float kWetShelfBoostDb  = 15.f;
  
  // Wet rail ripple — measured +18 dB above dry at 120 Hz, decreasing
  // at higher harmonics. Injected common-mode (shared rail) into both
  // wet channels.
  constexpr float kWetRipple120 = 2.6e-4f;
  constexpr float kWetRipple240 = 8.3e-5f;
  constexpr float kWetRipple360 = 2.9e-5f;
  
  // BBD clock feedthrough amplitude. Above Nyquist at common host sample
  // rates with the 3.30 ms center delay, so the BBDClockFeedthrough self-
  // mutes most of the time. Only matters at 192 kHz host rate where the
  // clock falls in-band.
  constexpr float kBBDClockGain = 6.0e-5f;
  
  } // namespace analog_noise
  
} // namespace kr106