#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <bitset>
#include <memory>
#include <cstdint>
#include <climits>

#include "KR106Voice.h"
#include "KR106LFO.h"
#include "KR106Arpeggiator.h"
#include "KR106Chorus.h"

// Top-level KR-106 DSP orchestrator

// Modulation bus indices (passed as inputs[] to voices)
enum EModulations
{
  kModLFO = 0,
  kNumModulations
};

// ============================================================
// RBJ Biquad HPF
// ============================================================
namespace kr106 {

// KR-106 4-position HPF switch (replicates the 4052 dual-switch network).
//
// Mode 0 (slider bottom): Low-shelf bass boost  ~+10 dB below ~150 Hz
// Mode 1:                 Flat / bypass
// Mode 2:                 1-pole HPF at ~240 Hz
// Mode 3 (slider top):    1-pole HPF at ~720 Hz
struct HPF
{
  static constexpr float kShelfFreqHz  = 150.f;
  static constexpr float kShelfGainLin = 3.162f; // ~+10 dB
  static constexpr float kHPFFreqs[4] = { 0.f, 0.f, 240.f, 720.f };
  static constexpr float kDCBlockHz = 5.f; // DC blocker cutoff for modes 0 & 1

  int   mMode = 1;
  float mSampleRate = 44100.f;
  float mG   = 0.f;
  float mHpS = 0.f;
  float mLpS = 0.f;
  float mDcG = 0.f;  // DC blocker coefficient
  float mDcS = 0.f;  // DC blocker state

  void Init()       { mHpS = 0.f; mLpS = 0.f; mDcS = 0.f; }
  void SetSampleRate(float sr) { mSampleRate = sr; Recalc(); }

  void SetMode(int mode)
  {
    int newMode = std::clamp(mode, 0, 3);
    if (newMode == mMode) return;
    mMode = newMode;
    Recalc();
  }

  void Recalc()
  {
    // DC blocker (~5 Hz) — always computed, used in modes 0 & 1
    float dcFrq = std::clamp(kDCBlockHz / (mSampleRate * 0.5f), 0.001f, 0.9f);
    mDcG = tanf(dcFrq * static_cast<float>(M_PI) * 0.5f);

    if (mMode == 1) { mG = 0.f; return; }
    float fc = (mMode == 0) ? kShelfFreqHz : kHPFFreqs[mMode];
    float frq = std::clamp(fc / (mSampleRate * 0.5f), 0.001f, 0.9f);
    mG = tanf(frq * static_cast<float>(M_PI) * 0.5f);
  }

  // 1-pole DC blocker: subtract LP at ~5 Hz
  float DCBlock(float input)
  {
    float v = (input - mDcS) * mDcG / (1.f + mDcG);
    float lp = mDcS + v;
    mDcS = lp + v;
    return input - lp;
  }

  float Process(float input)
  {
    if (mMode == 1) return DCBlock(input);
    if (mMode == 0)
    {
      float v = (input - mLpS) * mG / (1.f + mG);
      float lp = mLpS + v;
      mLpS = lp + v;
      return DCBlock(input + (kShelfGainLin - 1.f) * lp);
    }
    float v = (input - mHpS) * mG / (1.f + mG);
    float lp = mHpS + v;
    mHpS = lp + v;
    return input - lp;
  }
};

} // namespace kr106

// ============================================================
// KR106DSP — top-level orchestrator
// ============================================================
template <typename T>
class KR106DSP
{
public:
  KR106DSP(int nVoices)
  {
    for (int i = 0; i < nVoices; i++)
    {
      auto voice = std::make_unique<kr106::Voice<T>>();
      voice->InitVariance(i);
      mVoices.push_back(std::move(voice));
    }
    mLFOBuffer.reserve(4096);
    mSyncBuffer.reserve(4096);
    mModulations.resize(kNumModulations, nullptr);
  }

  // --- Voice container helpers ---
  size_t NVoices() const { return mVoices.size(); }
  kr106::Voice<T>* GetVoice(int i) { return mVoices[i].get(); }

  template <typename F>
  void ForEachVoice(F func)
  {
    for (auto& v : mVoices) func(*v);
  }

  static double MidiToPitch(int note) { return (note - 69) / 12.0; }

  void TriggerUnisonVoices(int note, int velocity)
  {
    double pitch = MidiToPitch(note);
    float vel = velocity / 127.f;
    bool anyBusy = false;
    ForEachVoice([&](kr106::Voice<T>& v) { anyBusy |= v.GetBusy(); });
    ForEachVoice([pitch, vel, anyBusy](kr106::Voice<T>& v) {
      v.SetUnisonPitch(pitch);
      v.Trigger(vel, anyBusy);
    });
  }

  void ReleaseUnisonVoices()
  {
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
  }

  int FindLowestFreeVoice()
  {
    int nv = static_cast<int>(NVoices());
    for (int i = 0; i < nv; i++)
      if (mVoiceNote[i] < 0 && mVoices[i]->GetBusy()) return i;
    for (int i = 0; i < nv; i++)
      if (!mVoices[i]->GetBusy()) return i;
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    return oldest;
  }

  int FindRoundRobinVoice()
  {
    int nv = static_cast<int>(NVoices());
    for (int j = 0; j < nv; j++)
    {
      int i = (mRoundRobinNext + j) % nv;
      if (!mVoices[i]->GetBusy()) { mRoundRobinNext = (i + 1) % nv; return i; }
    }
    for (int j = 0; j < nv; j++)
    {
      int i = (mRoundRobinNext + j) % nv;
      if (mVoiceNote[i] < 0) { mRoundRobinNext = (i + 1) % nv; return i; }
    }
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    mRoundRobinNext = (oldest + 1) % nv;
    return oldest;
  }

  void TriggerVoice(int voiceIdx, int note, int velocity)
  {
    auto& v = *mVoices[voiceIdx];
    v.SetUnisonPitch(MidiToPitch(note));
    v.Trigger(velocity / 127.f, v.GetBusy());
    mVoiceNote[voiceIdx] = note;
    mVoiceAge[voiceIdx] = ++mVoiceAgeCounter;
  }

  void SendToSynth(int note, bool noteOn, int velocity, int offset = 0)
  {
    (void)offset;
    if (mPortaMode == 0)
    {
      if (noteOn)
      {
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        mUnisonStack.push_back(note);
        if (mUnisonNote >= 0) ReleaseUnisonVoices();
        mUnisonNote = note;
        TriggerUnisonVoices(note, velocity);
      }
      else
      {
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        if (note == mUnisonNote)
        {
          if (!mUnisonStack.empty())
          { mUnisonNote = mUnisonStack.back(); TriggerUnisonVoices(mUnisonNote, 127); }
          else
          { ReleaseUnisonVoices(); mUnisonNote = -1; }
        }
      }
    }
    else if (mPortaMode == 1)
    {
      if (noteOn)
      {
        int vi = FindLowestFreeVoice();
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        int nv = static_cast<int>(NVoices());
        for (int i = 0; i < nv; i++)
          if (mVoiceNote[i] == note) { mVoices[i]->Release(); mVoiceNote[i] = -1; break; }
      }
    }
    else
    {
      if (noteOn)
      {
        int vi = FindRoundRobinVoice();
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        int nv = static_cast<int>(NVoices());
        for (int i = 0; i < nv; i++)
          if (mVoiceNote[i] == note) { mVoices[i]->Release(); mVoiceNote[i] = -1; break; }
      }
    }
  }

  void ProcessBlock(T** inputs, T** outputs, int nOutputs, int nFrames)
  {
    (void)inputs;
    for (int c = 0; c < nOutputs; c++)
      memset(outputs[c], 0, nFrames * sizeof(T));

    if (static_cast<int>(mLFOBuffer.size()) < nFrames)
    {
      mLFOBuffer.resize(nFrames, T(0));
      mSyncBuffer.resize(nFrames, T(0));
      mModulations.resize(kNumModulations, nullptr);
    }

    memset(mSyncBuffer.data(), 0, nFrames * sizeof(T));
    int nv = static_cast<int>(NVoices());
    int scopeVoice = -1;
    int64_t oldestAge = INT64_MAX;
    for (int i = 0; i < nv; i++)
      if (mVoices[i]->GetBusy() && mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; scopeVoice = i; }
    for (int i = 0; i < nv; i++)
      mVoices[i]->mSyncOut = (i == scopeVoice) ? mSyncBuffer.data() : nullptr;

    bool anyActive = false;
    ForEachVoice([&](kr106::Voice<T>& v) { anyActive |= v.GetBusy(); });
    mLFO.SetVoiceActive(anyActive);

    for (int s = 0; s < nFrames; s++)
      mLFOBuffer[s] = static_cast<T>(mLFO.Process());

    mModulations[kModLFO] = mLFOBuffer.data();

    mArp.Process(nFrames,
      [this](int note, int offset) { SendToSynth(note, true,  127, offset); },
      [this](int note, int offset) { SendToSynth(note, false, 0,   offset); });

    for (auto& v : mVoices)
      if (v->GetBusy())
        v->ProcessSamplesAccumulating(mModulations.data(), outputs, kNumModulations, nOutputs, 0, nFrames);

    for (int s = 0; s < nFrames; s++)
      outputs[0][s] = static_cast<T>(mHPF.Process(static_cast<float>(outputs[0][s])));

    if (mChorus.mMode == 0 && mChorus.mFade <= 0.f)
    {
      if (nOutputs > 1)
        memcpy(outputs[1], outputs[0], nFrames * sizeof(T));
    }
    else
    {
      for (int s = 0; s < nFrames; s++)
      {
        float mono = static_cast<float>(outputs[0][s]);
        float L, R;
        mChorus.Process(mono, L, R);
        outputs[0][s] = static_cast<T>(L);
        if (nOutputs > 1) outputs[1][s] = static_cast<T>(R);
      }
    }

    for (int s = 0; s < nFrames; s++)
    {
      outputs[0][s] *= static_cast<T>(mVcaLevel);
      if (nOutputs > 1) outputs[1][s] *= static_cast<T>(mVcaLevel);
    }
  }

  T* GetSyncBuffer() { return mSyncBuffer.data(); }

  void Reset(double sampleRate, int blockSize)
  {
    mSampleRate = static_cast<float>(sampleRate);
    ForEachVoice([sampleRate, blockSize](kr106::Voice<T>& v) {
      v.SetSampleRateAndBlockSize(sampleRate, blockSize);
    });
    mHPF.SetSampleRate(mSampleRate);
    mHPF.Init();
    mHPF.SetMode(1);
    mChorus.Init(mSampleRate);
    mArp.SetSampleRate(mSampleRate);
    mLFOBuffer.resize(blockSize);
    mSyncBuffer.resize(blockSize);
    mModulations.resize(kNumModulations);
  }

  void NoteOn(int note, int velocity)
  {
    mKeysDown.set(note);
    if (mArp.mEnabled) { mArp.NoteOn(note); return; }
    SendToSynth(note, true, velocity, 0);
  }

  void NoteOff(int note)
  {
    mKeysDown.reset(note);
    if (mArp.mEnabled)
    {
      if (mHold) mHeldNotes.set(note);
      else mArp.NoteOff(note);
      return;
    }
    if (mHold) { mHeldNotes.set(note); return; }
    SendToSynth(note, false, 0, 0);
  }

  void ControlChange(int cc, float value)
  {
    if (cc == 1) {
      bool on = value > 0.0f;
      mLFO.SetTrigger(on);
      float mod = on ? 1.f : 0.f;
      ForEachVoice([mod](kr106::Voice<T>& v) { v.mBenderModAmt = mod; });
      return;
    }
  }

  void SetParam(int paramIdx, double value);

public:
  std::vector<std::unique_ptr<kr106::Voice<T>>> mVoices;
  kr106::LFO mLFO;
  kr106::HPF mHPF;
  kr106::Chorus mChorus;
  kr106::Arpeggiator mArp;

  float mVcaLevel = 1.f;
  float mSampleRate = 44100.f;
  int mOctaveTranspose = 0;
  double mTuning = 0.;
  int mKeyTranspose = 0;
  bool mHold = false;
  bool mTranspose = false;
  int mPortaMode = 2;
  int mUnisonNote = -1;
  std::vector<int> mUnisonStack;
  int mVoiceNote[6] = {-1,-1,-1,-1,-1,-1};
  int64_t mVoiceAge[6] = {0,0,0,0,0,0};
  int64_t mVoiceAgeCounter = 0;
  int mRoundRobinNext = 0;
  bool mChorusI = false;
  bool mChorusII = false;
  int mAdsrMode = 0;
  float mSliderA = 0.25f;
  float mSliderD = 0.25f;
  float mSliderR = 0.25f;
  float mSliderLfoRate = 0.24f;
  float mSliderArpRate = 0.06f;
  float mSliderDcoLfo = 0.f;
  float mSliderVcfLfo = 0.f;

  std::bitset<128> mHeldNotes;
  std::bitset<128> mKeysDown;
  bool mSuppressHoldRelease = false;
  std::vector<T> mLFOBuffer;
  std::vector<T> mSyncBuffer;
  std::vector<T*> mModulations;

  void SetKeyTranspose(int semitones)
  {
    if (semitones == mKeyTranspose) return;
    mKeyTranspose = semitones;
    float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
    ForEachVoice([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
  }

  void PowerOff()
  {
    mArp.Reset();
    mKeysDown.reset();
    mHeldNotes.reset();
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
    for (int i = 0; i < 6; i++) mVoiceNote[i] = -1;
    mUnisonNote = -1;
    mUnisonStack.clear();
  }

  void ForceRelease(int noteNum)
  {
    mHeldNotes.reset(noteNum);
    if (mArp.mEnabled)
      mArp.NoteOff(noteNum);
    else
      SendToSynth(noteNum, false, 0);
  }

private:
  void UpdateChorusMode()
  {
    int mode = 0;
    if (mChorusI) mode |= 1;
    if (mChorusII) mode |= 2;
    mChorus.SetMode(mode);
  }

  void ReleaseHeldNotes()
  {
    if (mPortaMode == 0)
    {
      for (int i = 0; i < 128; i++)
      {
        if (mHeldNotes.test(i))
        {
          auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), i);
          if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        }
      }
      if (mUnisonNote >= 0 && mUnisonStack.empty())
      { ReleaseUnisonVoices(); mUnisonNote = -1; }
      else if (mUnisonNote >= 0 && !mHeldNotes.test(mUnisonNote))
      { /* Current note wasn't held — keep playing it */ }
      else if (!mUnisonStack.empty())
      { mUnisonNote = mUnisonStack.back(); TriggerUnisonVoices(mUnisonNote, 127); }
    }
    else
    {
      for (int i = 0; i < 128; i++)
      {
        if (mHeldNotes.test(i))
        {
          if (mArp.mEnabled) mArp.NoteOff(i);
          else SendToSynth(i, false, 0);
        }
      }
    }
    mHeldNotes.reset();
  }
};

// SetParam is defined out-of-line but still in the header (template)
#include "KR106_DSP_SetParam.h"
