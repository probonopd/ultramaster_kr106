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
      case kVcfFreq:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfFreq = static_cast<float>(value); });
        break;
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

      // --- ADSR ---
      case kEnvA:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mADSR.SetAttack(static_cast<float>(value)); });
        break;
      case kEnvD:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mADSR.SetDecay(static_cast<float>(value)); });
        break;
      case kEnvS:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mADSR.SetSustain(static_cast<float>(value)); });
        break;
      case kEnvR:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mADSR.SetRelease(static_cast<float>(value)); });
        break;

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
      case kLfoRate:
        mLFO.SetRate(static_cast<float>(value), mSampleRate);
        break;
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

      // --- Global: VCA level ---
      case kVcaLevel:
        mVcaLevel = static_cast<float>(value);
        break;

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

  float mVcaLevel = 1.f;
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
