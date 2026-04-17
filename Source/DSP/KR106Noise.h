#pragma once

#include <cmath>
#include <cstdint>

// Shared noise generator modeled on the Juno-106 noise source circuit.
//
// Circuit path (from service manual):
//   2SC945 (B,C=GND, E=noise) → 470K to +15V → 1µF cap → 4.7K to GND →
//   BA662 (-IN) → BA662 output → 100pF to GND + 300K to GND →
//   NPN buffer (C=+15V, E=10K to -15V) → 10µF cap → 47K to GND → output
//
// Poles derived from component values:
//   LPF:  100pF × 300K = 5305 Hz  (parallel RC load on BA662 current output)
//   HPF:  1µF × 4.7K = 34 Hz      (input coupling cap into BA662 termination)
//
// Flicker (1/f) tilt: decade-spaced EMA bank fed the same white source.
// Sample-rate independent; all corners from exp(-2π·fc/sr).
//
// Popcorn/microplasma: two random telegraph processes mixed in at low
// amplitude. Emulates the burst-noise character of 2SC945 in reverse
// breakdown. Mean dwell times are set in seconds and converted to
// per-sample flip probabilities, so behavior is sample-rate independent.
//
// White noise approximated by 4-sample CLT (sum of uniform randoms).

namespace kr106 {

struct Noise
{
  uint32_t mSeed = 22222;

  // Main circuit filters
  float mLPState = 0.f;
  float mHPState = 0.f;
  float mLPA = 0.f;
  float mHPA = 0.f;

  // Flicker (1/f) tilt bank
  static constexpr int kFlickerStages = 4;
  float mFlickerState[kFlickerStages] = {0};
  float mFlickerA[kFlickerStages] = {0};
  static constexpr float kFlickerFreqs[kFlickerStages] =
    { 30.f, 300.f, 3000.f, 15000.f };
  static constexpr float kFlickerMix = 1.5f;

  // Fast random in [0,1) using the same LCG
  float NextUniform()
  {
    mSeed = mSeed * 196314165u + 907633515u;
    return mSeed / static_cast<float>(0xFFFFFFFFu);
  }

  void SetSampleRate(float sampleRate)
  {
    constexpr float kLPFreq = 5305.f;
    constexpr float kHPFreq = 34.f;

    float r = kLPFreq / sampleRate;
    float fcLP = kLPFreq * (1.f - 2.2f * r * r);

    mLPA = expf(-2.f * static_cast<float>(M_PI) * fcLP / sampleRate);
    mHPA = expf(-2.f * static_cast<float>(M_PI) * kHPFreq / sampleRate);
    mLPState = 0.f;
    mHPState = 0.f;

    // Flicker bank
    for (int i = 0; i < kFlickerStages; i++)
    {
      mFlickerA[i] = expf(-2.f * static_cast<float>(M_PI)
                          * kFlickerFreqs[i] / sampleRate);
      mFlickerState[i] = 0.f;
    }
  }

  float Process()
  {
    // 4-sample CLT white noise
    float g = 0.f;
    for (int j = 0; j < 4; j++)
    {
      mSeed = mSeed * 196314165u + 907633515u;
      g += 2.f * mSeed / static_cast<float>(0xFFFFFFFFu) - 1.f;
    }
    float white = g * 0.5f;

    // Flicker bank: decade-spaced LPFs summed, fed from white
    float flicker = 0.f;
    for (int i = 0; i < kFlickerStages; i++)
    {
      mFlickerState[i] = mFlickerA[i] * mFlickerState[i]
                       + (1.f - mFlickerA[i]) * white;
      flicker += mFlickerState[i];
    }
    flicker *= (1.f / kFlickerStages);

    // Combine: white carrier + flicker tilt
    float shaped = white + kFlickerMix * flicker;

    // Circuit filters
    mLPState = mLPA * mLPState + (1.f - mLPA) * shaped;
    mHPState = mHPA * mHPState + (1.f - mHPA) * mLPState;

    return mLPState - mHPState;
  }
};

} // namespace kr106