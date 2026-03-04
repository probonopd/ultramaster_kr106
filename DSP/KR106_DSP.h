#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <bitset>

#include "MidiSynth.h"
#include "ISender.h"

#include "KR106Voice.h"
#include "KR106LFO.h"
#include "KR106Arpeggiator.h"
#include "KR106Chorus.h"

// Top-level KR-106 DSP orchestrator
// Pattern follows IPlugInstrument_DSP.h

using namespace iplug;

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
//
// Reference: https://electricdruid.net/juno-106-highpass-filter/
struct HPF
{
  static constexpr float kShelfFreqHz  = 150.f;
  static constexpr float kShelfGainLin = 3.162f; // ~+10 dB
  static constexpr float kHPFFreqs[4] = { 0.f, 0.f, 240.f, 720.f };

  int   mMode = 1;
  float mSampleRate = 44100.f;
  float mG   = 0.f;
  float mHpS = 0.f; // HPF integrator state
  float mLpS = 0.f; // shelf lowpass integrator state

  void Init()       { mHpS = 0.f; mLpS = 0.f; }
  void SetSampleRate(float sr) { mSampleRate = sr; Recalc(); }

  void SetMode(int mode)
  {
    int newMode = std::clamp(mode, 0, 3);
    if (newMode == mMode) return;
    mMode = newMode;
    // Don't reset integrator state — TPT state variables are
    // coefficient-independent, so the filter converges naturally
    // from any prior state. Resetting to 0 would cause a click
    // (the state tracks the signal's DC/LF content).
    Recalc();
  }

  void Recalc()
  {
    if (mMode == 1) { mG = 0.f; return; }
    float fc = (mMode == 0) ? kShelfFreqHz : kHPFFreqs[mMode];
    float frq = std::clamp(fc / (mSampleRate * 0.5f), 0.001f, 0.9f);
    mG = tanf(frq * static_cast<float>(M_PI) * 0.5f);
  }

  float Process(float input)
  {
    if (mMode == 1) return input;

    if (mMode == 0)
    {
      // Low-shelf boost: TPT lowpass, add scaled copy back
      float v = (input - mLpS) * mG / (1.f + mG);
      float lp = mLpS + v;
      mLpS = lp + v;
      return input + (kShelfGainLin - 1.f) * lp;
    }

    // Modes 2 & 3: 1-pole TPT highpass
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
      auto* voice = new kr106::Voice<T>();
      voice->InitVariance(i);
      mSynth.AddVoice(voice, 0);
    }

    mSynth.SetPitchBendRange(12); // ±12 semitones for external MIDI

    // Pre-allocate audio buffers so ProcessBlock never triggers heap allocation
    mLFOBuffer.reserve(4096);
    mSyncBuffer.reserve(4096);
    mModulations.resize(kNumModulations, nullptr);
  }

  static double MidiToPitch(int note) { return (note - 69) / 12.0; }

  void TriggerUnisonVoices(int note, int velocity)
  {
    double pitch = MidiToPitch(note);
    float vel = velocity / 127.f;
    bool anyBusy = false;
    mSynth.ForEachVoice([&](SynthVoice& sv) { anyBusy |= sv.GetBusy(); });

    mSynth.ForEachVoice([pitch, vel, anyBusy](SynthVoice& sv) {
      auto& v = dynamic_cast<kr106::Voice<T>&>(sv);
      v.SetUnisonPitch(pitch);
      v.Trigger(vel, anyBusy);
    });

    mSynth.SetVoicesActive(true);
  }

  void ReleaseUnisonVoices()
  {
    mSynth.ForEachVoice([](SynthVoice& sv) { sv.Release(); });
  }

  // Lowest-first poly: prefer releasing, then idle, then steal oldest held
  int FindLowestFreeVoice()
  {
    int nv = static_cast<int>(mSynth.NVoices());

    // 1. Lowest releasing voice (NoteOff sent, still in release tail) — reuse same voice
    for (int i = 0; i < nv; i++)
      if (mVoiceNote[i] < 0 && mSynth.GetVoice(i)->GetBusy()) return i;

    // 2. Lowest idle voice (envelope finished)
    for (int i = 0; i < nv; i++)
      if (!mSynth.GetVoice(i)->GetBusy()) return i;

    // 3. All voices actively held — steal the oldest
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
    {
      if (mVoiceAge[i] < oldestAge)
      {
        oldestAge = mVoiceAge[i];
        oldest = i;
      }
    }
    return oldest;
  }

  void TriggerVoice(int voiceIdx, int note, int velocity)
  {
    auto& v = dynamic_cast<kr106::Voice<T>&>(*mSynth.GetVoice(voiceIdx));
    double pitch = MidiToPitch(note);
    v.SetUnisonPitch(pitch);
    v.Trigger(velocity / 127.f, v.GetBusy());
    mVoiceNote[voiceIdx] = note;
    mVoiceAge[voiceIdx] = ++mVoiceAgeCounter;

    mSynth.SetVoicesActive(true);
  }

  void SendToSynth(int note, bool noteOn, int velocity, int offset = 0)
  {
    if (mPortaMode == 0) // Unison — last-note priority
    {
      if (noteOn)
      {
        // Remove duplicate if already in stack, then push to top
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        mUnisonStack.push_back(note);

        if (mUnisonNote >= 0)
          ReleaseUnisonVoices();
        mUnisonNote = note;
        TriggerUnisonVoices(note, velocity);
      }
      else
      {
        // Remove from stack
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);

        if (note == mUnisonNote)
        {
          if (!mUnisonStack.empty())
          {
            // Return to previous held note
            mUnisonNote = mUnisonStack.back();
            TriggerUnisonVoices(mUnisonNote, 127);
          }
          else
          {
            ReleaseUnisonVoices();
            mUnisonNote = -1;
          }
        }
      }
    }
    else if (mPortaMode == 1) // Poly + Porta — lowest-voice-first
    {
      if (noteOn)
      {
        int vi = FindLowestFreeVoice();
        // If stealing, clear old mapping
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        // Find the voice playing this note and release it
        int nv = static_cast<int>(mSynth.NVoices());
        for (int i = 0; i < nv; i++)
        {
          if (mVoiceNote[i] == note)
          {
            mSynth.GetVoice(i)->Release();
            mVoiceNote[i] = -1;
            break;
          }
        }
      }
    }
    else // Poly round-robin (mode 2) — stock MidiSynth allocator
    {
      IMidiMsg msg;
      if (noteOn)
        msg.MakeNoteOnMsg(note, velocity, offset);
      else
        msg.MakeNoteOffMsg(note, offset);
      mSynth.AddMidiMsgToQueue(msg);
    }
  }

  void ProcessBlock(T** inputs, T** outputs, int nOutputs, int nFrames)
  {
    // 1. Clear outputs
    for (int c = 0; c < nOutputs; c++)
      memset(outputs[c], 0, nFrames * sizeof(T));

    // Safety: ensure buffers are allocated (in case ProcessBlock runs before Reset)
    if (static_cast<int>(mLFOBuffer.size()) < nFrames)
    {
      mLFOBuffer.resize(nFrames, T(0));
      mSyncBuffer.resize(nFrames, T(0));
      mModulations.resize(kNumModulations, nullptr);
    }

    // 2. Clear sync buffer and assign to first active voice only (for scope)
    memset(mSyncBuffer.data(), 0, nFrames * sizeof(T));
    bool syncAssigned = false;
    mSynth.ForEachVoice([this, &syncAssigned](SynthVoice& v) {
      auto& jv = dynamic_cast<kr106::Voice<T>&>(v);
      if (!syncAssigned && v.GetBusy())
      {
        jv.mSyncOut = mSyncBuffer.data();
        syncAssigned = true;
      }
      else
      {
        jv.mSyncOut = nullptr;
      }
    });

    // 3. Check if any voice is active (for LFO delay reset)
    bool anyActive = false;
    mSynth.ForEachVoice([&](SynthVoice& v) {
      anyActive |= v.GetBusy();
    });
    mLFO.SetVoiceActive(anyActive);

    // 3. Fill LFO buffer
    for (int s = 0; s < nFrames; s++)
      mLFOBuffer[s] = static_cast<T>(mLFO.Process());

    // 4. Set modulation pointers
    mModulations[kModLFO] = mLFOBuffer.data();

    // 5. Process arpeggiator — generates note events for this block
    mArp.Process(nFrames,
      [this](int note, int offset) { SendToSynth(note, true,  127, offset); },
      [this](int note, int offset) { SendToSynth(note, false, 0,   offset); });

    // 6. Process voices through MidiSynth
    mSynth.ProcessBlock(mModulations.data(), outputs,
      kNumModulations, nOutputs, nFrames);

    // 6. HPF (in-place on mono sum in channel 0)
    for (int s = 0; s < nFrames; s++)
      outputs[0][s] = static_cast<T>(mHPF.Process(static_cast<float>(outputs[0][s])));

    // 7. Stereo chorus
    if (mChorus.mMode == 0)
    {
      // No chorus: copy L to R
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

    // 8. Master volume
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
    mSynth.SetSampleRateAndBlockSize(sampleRate, blockSize);
    mSynth.Reset();

    mHPF.SetSampleRate(mSampleRate);
    mHPF.Init();
    mHPF.SetMode(1); // default: flat

    mChorus.Init(mSampleRate);

    mArp.SetSampleRate(mSampleRate);

    mLFOBuffer.resize(blockSize);
    mSyncBuffer.resize(blockSize);
    mModulations.resize(kNumModulations);
  }

  void ProcessMidiMsg(const IMidiMsg& msg)
  {
    // LFO trigger button sends CC1 (mod wheel)
    if (msg.StatusMsg() == IMidiMsg::kControlChange &&
        msg.ControlChangeIdx() == IMidiMsg::kModWheel)
    {
      mLFO.SetTrigger(msg.ControlChange(IMidiMsg::kModWheel) > 0.0);
      return;
    }

    auto status = msg.StatusMsg();
    bool isNoteOn = (status == IMidiMsg::kNoteOn && msg.Velocity() > 0);
    bool isNoteOff = (status == IMidiMsg::kNoteOff ||
                      (status == IMidiMsg::kNoteOn && msg.Velocity() == 0));

    // Track physical key state (for seeding arp on enable)
    if (isNoteOn) mKeysDown.set(msg.NoteNumber());
    else if (isNoteOff) mKeysDown.reset(msg.NoteNumber());

    // Arpeggiator intercepts note events when enabled
    if (mArp.mEnabled && (isNoteOn || isNoteOff))
    {
      if (isNoteOn)
        mArp.NoteOn(msg.NoteNumber());
      else if (mHold)
        mHeldNotes.set(msg.NoteNumber()); // track for Hold release
      else
        mArp.NoteOff(msg.NoteNumber());
      return;
    }

    // Hold logic: suppress note-off when hold is active
    if (mHold && isNoteOff)
    {
      mHeldNotes.set(msg.NoteNumber());
      return;
    }

    SendToSynth(msg.NoteNumber(), isNoteOn, msg.Velocity(), 0);
  }

  // ADSR lookup tables — measured from real Juno-106 factory preset recordings.
  // Generated by tools/adsr-calibration/generate_lut.py via PCHIP interpolation
  // in log space between curated anchor points.
  // Attack: 1ms–3000ms, Decay/Release: 2ms–12000ms (published Roland specs).
  static constexpr float kAttackLUT[128] = {
    1.0f, 1.4f, 2.9f, 17.4f, 24.9f, 31.5f, 47.2f, 63.1f,
    67.9f, 71.0f, 74.8f, 82.2f, 92.0f, 100.3f, 106.3f, 112.1f,
    117.6f, 122.7f, 127.7f, 132.3f, 136.9f, 141.3f, 145.6f, 150.1f,
    154.4f, 158.5f, 162.3f, 165.8f, 169.2f, 172.5f, 175.6f, 178.7f,
    181.8f, 184.9f, 188.2f, 191.6f, 195.3f, 199.3f, 203.8f, 208.7f,
    214.2f, 220.4f, 227.5f, 235.5f, 246.4f, 262.1f, 282.7f, 308.6f,
    340.0f, 376.9f, 419.5f, 467.5f, 520.4f, 577.0f, 635.7f, 694.1f,
    749.0f, 797.0f, 833.8f, 863.6f, 894.0f, 925.1f, 956.8f, 989.2f,
    1022.2f, 1055.9f, 1090.2f, 1125.1f, 1160.6f, 1196.6f, 1233.3f, 1270.4f,
    1308.1f, 1346.3f, 1384.9f, 1424.0f, 1463.5f, 1503.4f, 1543.7f, 1584.3f,
    1625.1f, 1666.2f, 1707.5f, 1749.0f, 1790.7f, 1832.4f, 1874.2f, 1915.9f,
    1957.7f, 1999.3f, 2040.8f, 2082.1f, 2123.2f, 2163.9f, 2204.4f, 2244.4f,
    2284.0f, 2323.0f, 2361.5f, 2399.4f, 2436.6f, 2473.1f, 2508.8f, 2543.6f,
    2577.6f, 2610.6f, 2642.6f, 2673.5f, 2703.3f, 2732.0f, 2759.4f, 2785.6f,
    2810.5f, 2834.0f, 2856.1f, 2876.8f, 2896.0f, 2913.7f, 2929.8f, 2944.3f,
    2957.2f, 2968.5f, 2978.0f, 2985.9f, 2992.1f, 2996.5f, 2999.1f, 3000.0f
  };
  static constexpr float kDecayLUT[128] = {
    2.0f, 2.5f, 3.0f, 3.7f, 4.6f, 5.6f, 6.8f, 8.2f,
    9.9f, 11.9f, 14.2f, 16.9f, 20.0f, 23.8f, 28.4f, 33.9f,
    40.5f, 48.2f, 57.3f, 67.9f, 80.1f, 94.0f, 109.6f, 127.0f,
    145.9f, 166.2f, 187.4f, 209.0f, 230.5f, 250.9f, 270.8f, 291.2f,
    311.8f, 332.7f, 353.8f, 375.0f, 396.3f, 417.6f, 438.9f, 460.2f,
    481.4f, 502.6f, 523.8f, 545.0f, 566.2f, 586.5f, 605.1f, 622.2f,
    638.4f, 654.3f, 669.8f, 685.1f, 700.1f, 715.0f, 729.7f, 744.3f,
    758.9f, 773.6f, 788.4f, 803.5f, 818.9f, 834.7f, 851.0f, 868.0f,
    885.8f, 904.5f, 924.2f, 944.3f, 964.0f, 983.4f, 1002.9f, 1022.6f,
    1042.9f, 1064.1f, 1086.4f, 1110.2f, 1135.9f, 1163.9f, 1194.7f, 1228.7f,
    1266.5f, 1308.8f, 1362.4f, 1434.6f, 1526.7f, 1640.3f, 1777.6f, 1940.8f,
    2132.8f, 2356.8f, 2615.9f, 2913.5f, 3252.9f, 3636.9f, 4067.9f, 4547.1f,
    5074.5f, 5648.0f, 6263.3f, 6913.0f, 7586.7f, 8270.2f, 8945.6f, 9591.9f,
    10184.6f, 10697.8f, 11104.8f, 11380.3f, 11502.4f, 11548.9f, 11593.6f, 11636.5f,
    11677.4f, 11716.3f, 11753.1f, 11787.7f, 11820.0f, 11849.9f, 11877.4f, 11902.3f,
    11924.6f, 11944.1f, 11960.9f, 11974.8f, 11985.7f, 11993.6f, 11998.4f, 12000.0f
  };
  static constexpr float kReleaseLUT[128] = {
    2.0f, 6.3f, 12.0f, 12.0f, 12.0f, 12.0f, 12.0f, 12.0f,
    12.0f, 12.0f, 12.0f, 12.0f, 13.0f, 16.2f, 22.1f, 31.4f,
    43.9f, 57.4f, 66.8f, 71.1f, 74.6f, 77.2f, 79.4f, 81.1f,
    82.7f, 84.4f, 86.4f, 89.1f, 92.9f, 98.1f, 105.3f, 132.5f,
    174.9f, 207.0f, 239.2f, 271.7f, 305.3f, 341.6f, 382.5f, 428.0f,
    476.9f, 527.6f, 577.6f, 623.6f, 662.0f, 695.9f, 729.8f, 763.8f,
    797.7f, 831.5f, 865.1f, 898.4f, 931.4f, 964.1f, 996.5f, 1028.5f,
    1060.1f, 1091.4f, 1122.3f, 1152.9f, 1183.2f, 1213.4f, 1243.3f, 1273.2f,
    1303.0f, 1333.0f, 1363.1f, 1393.5f, 1424.3f, 1455.7f, 1487.8f, 1520.8f,
    1554.8f, 1590.0f, 1626.6f, 1664.9f, 1705.1f, 1747.4f, 1792.2f, 1839.6f,
    1890.1f, 1944.1f, 2001.9f, 2063.9f, 2130.7f, 2202.8f, 2279.3f, 2359.3f,
    2442.9f, 2530.3f, 2621.7f, 2717.3f, 2817.3f, 2922.0f, 3031.5f, 3146.1f,
    3266.1f, 3391.7f, 3523.2f, 3661.0f, 3805.3f, 3956.4f, 4114.9f, 4280.9f,
    4454.9f, 4637.3f, 4828.6f, 5029.3f, 5239.7f, 5460.5f, 5692.1f, 5935.2f,
    6190.3f, 6458.2f, 6739.4f, 7034.7f, 7344.9f, 7670.6f, 8012.9f, 8372.6f,
    8750.5f, 9147.8f, 9565.4f, 10004.5f, 10466.3f, 10951.9f, 11462.6f, 12000.0f
  };

  // Linearly interpolate a 128-entry LUT from a 0-1 slider value
  static float LookupADSR(const float* lut, float s)
  {
    float pos = s * 127.0f;
    int idx = static_cast<int>(pos);
    if (idx >= 127) return lut[127];
    float frac = pos - static_cast<float>(idx);
    return lut[idx] + frac * (lut[idx + 1] - lut[idx]);
  }

  void SetParam(int paramIdx, double value)
  {
    // Import param enum values
    // These must match the EParams enum in KR106.h
    enum {
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
      kTransposeOffset, kBenderLfo
    };

    switch (paramIdx)
    {
      // --- Per-voice continuous params ---
      case kDcoLfo:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoLfo = static_cast<float>(value); });
        break;
      case kDcoPwm:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoPwm = static_cast<float>(value); });
        break;
      case kDcoSub:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoSub = static_cast<float>(value); });
        break;
      case kDcoNoise:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoNoise = static_cast<float>(value); });
        break;
      case kVcfFreq: {
        float hz = 20.f * std::pow(900.f, static_cast<float>(value));  // 20–18000 Hz exponential
        SetVoiceParam([hz](kr106::Voice<T>& v) { v.mVcfFreq = hz; });
        break;
      }
      case kVcfRes:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfRes = static_cast<float>(value); });
        break;
      case kVcfEnv:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfEnv = static_cast<float>(value); });
        break;
      case kVcfLfo:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfLfo = static_cast<float>(value); });
        break;
      case kVcfKbd:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfKbd = static_cast<float>(value); });
        break;
      case kBenderDco:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mBendDco = static_cast<float>(value); });
        break;
      case kBenderVcf:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mBendVcf = static_cast<float>(value); });
        break;
      case kBenderLfo:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mBendLfo = static_cast<float>(value); });
        break;

      // --- ADSR (slider 0-1 → ms via measured LUT, Juno-106 official ranges) ---
      case kEnvA: {
        float ms = LookupADSR(kAttackLUT, static_cast<float>(value));
        SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetAttack(ms); });
        break;
      }
      case kEnvD: {
        float ms = LookupADSR(kDecayLUT, static_cast<float>(value));
        SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetDecay(ms); });
        break;
      }
      case kEnvS: {
        float s = std::max(static_cast<float>(value), 0.001f);  // floor at -60dB
        SetVoiceParam([s](kr106::Voice<T>& v) { v.mADSR.SetSustain(s); });
        break;
      }
      case kEnvR: {
        float ms = LookupADSR(kReleaseLUT, static_cast<float>(value));
        SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetRelease(ms); });
        break;
      }

      // --- Per-voice switches ---
      case kDcoPulse:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mPulseOn = value > 0.5; });
        break;
      case kDcoSaw:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mSawOn = value > 0.5; });
        break;
      case kDcoSubSw:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mSubOn = value > 0.5; });
        break;
      case kPwmMode:
        SetVoiceParam([value](kr106::Voice<T>& v) {
          v.mPwmMode = static_cast<int>(value) - 1; // 0->-1(LFO), 1->0(MAN), 2->1(ENV)
        });
        break;
      case kVcfEnvInv:
        SetVoiceParam([value](kr106::Voice<T>& v) {
          v.mVcfEnvInvert = (static_cast<int>(value) != 0) ? -1 : 1;
        });
        break;
      case kVcaMode:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcaMode = static_cast<int>(value); });
        break;
      case kBender:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mRawBend = static_cast<float>(value); });
        break;

      // --- Global: LFO ---
      case kLfoRate: {
        float bpm = 18.f + static_cast<float>(value) * 1182.f;  // 18–1200 BPM
        mLFO.SetRate(bpm, mSampleRate);
        break;
      }
      case kLfoDelay:
        mLFO.SetDelay(static_cast<float>(value));
        break;
      case kLfoMode:
        mLFO.SetMode(static_cast<int>(value));
        break;

      // --- Global: HPF ---
      case kHpfFreq:
        mHPF.SetMode(static_cast<int>(value));
        break;

      // --- Global: VCA level (+/-6 dB around unity, 0.5 = 0 dB) ---
      case kVcaLevel:
      {
        float bias = static_cast<float>(value) * 2.f - 1.f; // -1..+1
        mVcaLevel = std::pow(10.f, bias * 6.f / 20.f);      // +/-6 dB
        break;
      }

      // --- Global: Chorus ---
      case kChorusOff:
        // Handled via chorus I/II buttons
        break;
      case kChorusI:
        mChorusI = value > 0.5;
        UpdateChorusMode();
        break;
      case kChorusII:
        mChorusII = value > 0.5;
        UpdateChorusMode();
        break;

      // --- Global: Octave / Tuning ---
      case kOctTranspose:
      {
        mOctaveTranspose = 1 - static_cast<int>(value); // 0=up(+1), 1=normal(0), 2=down(-1)
        float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
        SetVoiceParam([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
        break;
      }
      case kTuning:
      {
        mTuning = value; // -1..+1 semitones
        float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
        SetVoiceParam([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
        break;
      }

      // --- Portamento mode ---
      case kPortaMode:
      {
        int prevMode = mPortaMode;
        mPortaMode = static_cast<int>(value); // 0=Unison(up), 1=Porta(mid), 2=Poly(down)

        // Release all voices when switching modes
        if (prevMode == 0 && mPortaMode != 0)
        {
          ReleaseUnisonVoices();
          mUnisonNote = -1;
          mUnisonStack.clear();
        }
        else if (prevMode == 1 && mPortaMode != 1)
        {
          // Release lowest-first poly voices
          int nv = static_cast<int>(mSynth.NVoices());
          for (int i = 0; i < nv; i++)
          {
            if (mVoiceNote[i] >= 0) { mSynth.GetVoice(i)->Release(); mVoiceNote[i] = -1; }
          }
        }
        else if (prevMode == 2 && mPortaMode != 2)
        {
          // Release round-robin poly voices
          for (int i = 0; i < 128; i++)
          {
            IMidiMsg msg; msg.MakeNoteOffMsg(i, 0);
            mSynth.AddMidiMsgToQueue(msg);
          }
        }
        mHeldNotes.reset();

        bool portaOn = (mPortaMode <= 1);
        SetVoiceParam([portaOn](kr106::Voice<T>& v) { v.mPortaEnabled = portaOn; });
        break;
      }
      case kPortaRate:
      {
        float rate = static_cast<float>(value);
        SetVoiceParam([rate](kr106::Voice<T>& v) {
          v.mPortaRateParam = rate;
          v.UpdatePortaCoeff();
        });
        break;
      }

      // --- Arpeggiator ---
      case kArpRate:
        mArp.mRate = static_cast<float>(value);
        break;
      case kArpMode:
        mArp.mMode = static_cast<int>(value);
        mArp.mStepIndex = 0;
        mArp.mDirection = 1;
        break;
      case kArpRange:
        mArp.mRange = static_cast<int>(value);
        break;
      case kArpeggio:
      {
        bool wasEnabled = mArp.mEnabled;
        mArp.mEnabled = value > 0.5;
        if (mSuppressHoldRelease)
          break; // preserve note state during preset load; arp will resume with existing notes
        if (mArp.mEnabled && !wasEnabled)
        {
          // Seed arp with all currently sounding notes:
          // physically held keys (mKeysDown) + hold-held notes (mHeldNotes)
          std::bitset<128> toSeed = mKeysDown | mHeldNotes;
          for (int i = 0; i < 128; i++)
          {
            if (toSeed.test(i))
            {
              mArp.NoteOn(i);
              IMidiMsg off;
              off.MakeNoteOffMsg(i, 0);
              mSynth.AddMidiMsgToQueue(off);
            }
          }
          // Hold-held notes are now owned by the arp
          mHeldNotes.reset();
        }
        else if (!mArp.mEnabled && wasEnabled)
        {
          // Release the currently sounding arp note immediately
          if (mArp.mLastNote >= 0)
          {
            IMidiMsg off;
            off.MakeNoteOffMsg(mArp.mLastNote, 0);
            mSynth.AddMidiMsgToQueue(off);
            mArp.mLastNote = -1;
          }
          // If hold is active, restore the arp's notes as held notes
          if (mHold)
          {
            mHeldNotes.reset();
            for (int n : mArp.mHeldNotes)
            {
              IMidiMsg on;
              on.MakeNoteOnMsg(n, 127, 0);
              mSynth.AddMidiMsgToQueue(on);
              mHeldNotes.set(n);
            }
          }
          mArp.Reset();
        }
        break;
      }

      // --- Hold ---
      case kHold:
        mHold = value > 0.5;
        if (!mHold && !mSuppressHoldRelease)
          ReleaseHeldNotes();
        break;

      // --- Transpose (key transpose mode) ---
      case kTranspose:
        mTranspose = value > 0.5;
        break;

      default:
        break;
    }
  }

public:
  MidiSynth mSynth { VoiceAllocator::kPolyModePoly, MidiSynth::kDefaultBlockSize };
  kr106::LFO mLFO;
  kr106::HPF mHPF;
  kr106::Chorus mChorus;
  kr106::Arpeggiator mArp;

  float mVcaLevel = 1.f;  // unity = param 0.5 (0 dB)
  float mSampleRate = 44100.f;
  int mOctaveTranspose = 0;
  double mTuning = 0.;
  int mKeyTranspose = 0;  // semitone offset from keyboard transpose mode
  bool mHold = false;
  bool mTranspose = false;
  int mPortaMode = 2;     // 0=Unison(up), 1=Poly+Porta(mid), 2=Poly(down)
  int mUnisonNote = -1;   // currently sounding unison note (-1 = none)
  std::vector<int> mUnisonStack; // held notes for last-note priority (unison mode)
  int mVoiceNote[6] = {-1,-1,-1,-1,-1,-1}; // note-to-voice map for lowest-first poly (mode 1)
  int64_t mVoiceAge[6] = {0,0,0,0,0,0};   // assignment order counter for voice stealing
  int64_t mVoiceAgeCounter = 0;
  bool mChorusI = false;
  bool mChorusII = false;

  std::bitset<128> mHeldNotes;   // for Hold button release tracking
  std::bitset<128> mKeysDown;    // physical key state (for arp seeding)
  bool mSuppressHoldRelease = false; // true during preset load to keep held notes alive
  std::vector<T> mLFOBuffer;
  std::vector<T> mSyncBuffer;   // oscillator sync pulses for scope trigger
  std::vector<T*> mModulations;

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
    if (mPortaMode == 0) // Unison: release all held notes from stack
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
      {
        ReleaseUnisonVoices();
        mUnisonNote = -1;
      }
      else if (mUnisonNote >= 0 && !mHeldNotes.test(mUnisonNote))
      {
        // Current note wasn't held — keep playing it
      }
      else if (!mUnisonStack.empty())
      {
        // Current note was held, fall back to top of stack
        mUnisonNote = mUnisonStack.back();
        TriggerUnisonVoices(mUnisonNote, 127);
      }
    }
    else
    {
      for (int i = 0; i < 128; i++)
      {
        if (mHeldNotes.test(i))
        {
          if (mArp.mEnabled)
            mArp.NoteOff(i);
          else
          {
            IMidiMsg msg;
            msg.MakeNoteOffMsg(i, 0);
            mSynth.AddMidiMsgToQueue(msg);
          }
        }
      }
    }
    mHeldNotes.reset();
  }

public:
  // Set keyboard transpose offset (semitones). Called from audio thread each block.
  void SetKeyTranspose(int semitones)
  {
    if (semitones == mKeyTranspose) return;
    mKeyTranspose = semitones;
    float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
    SetVoiceParam([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
  }

  // Force-release a single note, bypassing hold suppression.
  // Called from the plugin's audio block when the UI explicitly toggles a key off.
  // The note may or may not be in mHeldNotes (since OnMouseUp skips NoteOff when hold is on).
  void ForceRelease(int noteNum)
  {
    mHeldNotes.reset(noteNum);
    if (mPortaMode == 0) // Unison
    {
      // Remove from stack and let SendToSynth handle last-note priority
      SendToSynth(noteNum, false, 0);
    }
    else if (mArp.mEnabled)
      mArp.NoteOff(noteNum);
    else
    {
      IMidiMsg msg;
      msg.MakeNoteOffMsg(noteNum, 0);
      mSynth.AddMidiMsgToQueue(msg);
    }
  }

  template <typename F>
  void SetVoiceParam(F func)
  {
    mSynth.ForEachVoice([&func](SynthVoice& v) {
      func(dynamic_cast<kr106::Voice<T>&>(v));
    });
  }
};
