// KR-106 WASM interface — thin C API around the DSP engine for
// use from JavaScript AudioWorkletProcessor.
//
// Exported functions (extern "C"):
//   kr106_create(sampleRate)  → pointer to DSP instance
//   kr106_destroy(ptr)
//   kr106_process(ptr, outL, outR, nFrames)
//   kr106_note_on(ptr, note, velocity)
//   kr106_note_off(ptr, note)
//   kr106_set_param(ptr, paramIdx, value)
//   kr106_load_preset(ptr, presetIdx)
//   kr106_get_num_presets() → int
//   kr106_get_preset_name(presetIdx) → char*

#include <emscripten.h>
#include <cstring>

#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"
#include "../../Source/DSP/KR106ParamValue.h"
#include "../../Source/KR106_Presets_JUCE.h"

// EParams enum (same as PluginProcessor.h, no JUCE dependency)
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

static void setParam(KR106DSP<float>& dsp, int param, float value)
{
  dsp.SetParam(param, static_cast<double>(value));
}

static void loadPreset(KR106DSP<float>& dsp, int presetIdx)
{
  if (presetIdx < 0 || presetIdx >= kNumFactoryPresets) return;
  const auto& p = kFactoryPresets[presetIdx];

  // Live performance params — excluded from presets (matches native PluginProcessor.cpp)
  // Sliders: kBenderDco(0), kBenderVcf(1), kArpRate(2), kPortaRate(40)
  // Switches: kTranspose(20), kHold(21), kArpeggio(22), kArpMode(30), kArpRange(31),
  //           kBender(36), kTuning(37), kPower(38), kPortaMode(39),
  //           kTransposeOffset(41), kBenderLfo(42), kMasterVol(44)

  // Sliders (indices 0-19 + 40): raw 7-bit values, convert to 0-1
  for (int i = 0; i <= 19; i++)
  {
    if (i == kBenderDco || i == kBenderVcf || i == kArpRate) continue;
    if (i == kHpfFreq) { setParam(dsp, i, static_cast<float>(p.values[i])); continue; } // HPF is 0-3 integer
    setParam(dsp, i, p.values[i] / 127.f);
  }
  // kPortaRate(40) is a live param — skip

  // Switches (indices 20-39, 41-43): integer values
  for (int i = 20; i <= 39; i++)
  {
    if (i == kTranspose || i == kHold || i == kArpeggio ||
        i == kArpMode || i == kArpRange || i == kBender ||
        i == kTuning || i == kPower || i == kPortaMode) continue;
    setParam(dsp, i, static_cast<float>(p.values[i]));
  }
  // kTransposeOffset(41) and kBenderLfo(42) are live params
  // Only load kAdsrMode(43)
  setParam(dsp, kAdsrMode, static_cast<float>(p.values[43]));
}

// Scope double buffer: audio fills back buffer, UI reads front buffer.
// When audio fills the back buffer, it swaps if the UI has consumed the front.
// No race condition since each side owns one buffer at a time.
static constexpr int kScopeRingSize = 4096;
static float sScopeRing[2][kScopeRingSize] = {};
static float sScopeRingR[2][kScopeRingSize] = {};
static float sScopeSyncRing[2][kScopeRingSize] = {};
static int sScopeWritePos = 0;
static int sScopeBackBuf = 0;   // which buffer audio is writing to (0 or 1)
static int sScopeFrontBuf = 1;  // which buffer UI is reading from
static int sScopeFrontLen = 0;  // how many samples in front buffer
static bool sScopeFrontReady = false; // true when front buffer has data for UI
static bool sScopeFrontConsumed = true; // true when UI has read the front buffer
static bool sPowerOn = true;

extern "C" {

EMSCRIPTEN_KEEPALIVE
void* kr106_create(float sampleRate)
{
  auto* dsp = new KR106DSP<float>(6);
  dsp->Reset(static_cast<double>(sampleRate), 128);

  // Defaults matching the plugin
  setParam(*dsp, kPower, 1.f);
  setParam(*dsp, kAdsrMode, 1.f);     // J106 mode
  dsp->mMasterVol = 0.5f * 0.5f;      // squared taper default
  setParam(*dsp, kPortaMode, 1.f);     // Poly I (hardware default)
  dsp->SetOversample(2);
  dsp->ForEachVoice([](kr106::Voice<float>& v) { v.mOscMode = 1; });

  // Load first preset
  loadPreset(*dsp, 0);

  return dsp;
}

EMSCRIPTEN_KEEPALIVE
void kr106_destroy(void* ptr)
{
  delete static_cast<KR106DSP<float>*>(ptr);
}

EMSCRIPTEN_KEEPALIVE
void kr106_process(void* ptr, float* outL, float* outR, int nFrames)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  memset(outL, 0, nFrames * sizeof(float));
  memset(outR, 0, nFrames * sizeof(float));
  if (!sPowerOn) return;
  float* outputs[2] = { outL, outR };
  dsp->ProcessBlock(nullptr, outputs, 2, nFrames);

  // Fill scope back buffer
  float* syncBuf = dsp->GetSyncBuffer();
  int bb = sScopeBackBuf;
  for (int i = 0; i < nFrames; i++)
  {
    sScopeRing[bb][sScopeWritePos] = outL[i];
    sScopeRingR[bb][sScopeWritePos] = outR[i];
    sScopeSyncRing[bb][sScopeWritePos] = syncBuf ? syncBuf[i] : 0.f;
    sScopeWritePos++;
    if (sScopeWritePos >= kScopeRingSize)
    {
      // Back buffer full -- swap if UI consumed the front buffer
      if (sScopeFrontConsumed)
      {
        sScopeFrontLen = kScopeRingSize;
        sScopeFrontReady = true;
        sScopeFrontConsumed = false;
        sScopeFrontBuf = bb;
        sScopeBackBuf = 1 - bb;
        bb = sScopeBackBuf;
      }
      sScopeWritePos = 0;
    }
  }
}

EMSCRIPTEN_KEEPALIVE
void kr106_note_on(void* ptr, int note, int velocity)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  dsp->NoteOn(note, velocity);
}

EMSCRIPTEN_KEEPALIVE
void kr106_force_release(void* ptr, int note)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  dsp->ForceRelease(note);
}

EMSCRIPTEN_KEEPALIVE
void kr106_note_off(void* ptr, int note)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  dsp->NoteOff(note);
}

EMSCRIPTEN_KEEPALIVE
void kr106_control_change(void* ptr, int cc, float value)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  dsp->ControlChange(cc, value);
}

EMSCRIPTEN_KEEPALIVE
void kr106_set_voices(void* ptr, int n)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  dsp->SetActiveVoices(n);
}

EMSCRIPTEN_KEEPALIVE
void kr106_set_ignore_velocity(void* ptr, int b)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  dsp->mIgnoreVelocity = (b != 0);
}

EMSCRIPTEN_KEEPALIVE
// Bank offset: J6 mode (adsrMode=0) -> presets 0-127, J106 mode -> 128-255
static int sBankOffset = 128; // default J106

void kr106_set_param(void* ptr, int paramIdx, float value)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  if (paramIdx == kPower)
  {
    sPowerOn = value > 0.5f;
    if (!sPowerOn) dsp->PowerOff();
    return;
  }
  if (paramIdx == kMasterVol)
  {
    dsp->mMasterVol = value * value;
    return;
  }
  if (paramIdx == kTransposeOffset)
  {
    dsp->SetKeyTranspose(static_cast<int>(value));
    return;
  }

  // Model switch: remap VCF freq and HPF to preserve Hz
  if (paramIdx == kAdsrMode)
  {
    bool wasJ106 = (dsp->mSynthModel == kr106::kJ106);
    bool toJ106 = (value > 0.5f);

    if (toJ106 != wasJ106)
    {
      // VCF Freq: preserve Hz across model curves
      float vcfSlider = dsp->mSliderVcfFreq;
      float srcHz;
      if (toJ106)
        srcHz = kr106::j60_vcf_freq_from_slider(vcfSlider);
      else
        srcHz = kr106::dacToHz(static_cast<uint16_t>(vcfSlider * 0x3F80));

      // Set model first so DSP re-dispatches all sliders
      dsp->SetParam(paramIdx, static_cast<double>(value));

      // Binary search for slider position on target curve
      auto targetFn = toJ106
        ? [](float v) { return kr106::dacToHz(static_cast<uint16_t>(v * 0x3F80)); }
        : [](float v) { return kr106::j60_vcf_freq_from_slider(v); };

      float lo = 0.f, hi = 1.f;
      for (int i = 0; i < 32; i++)
      {
        float mid = (lo + hi) * 0.5f;
        if (targetFn(mid) < srcHz) lo = mid; else hi = mid;
      }
      dsp->SetParam(kVcfFreq, static_cast<double>((lo + hi) * 0.5f));

      // HPF: remap positions
      int hpf = static_cast<int>(dsp->mSliderHpf + 0.5f);
      if (!toJ106 && hpf <= 1)
        dsp->SetParam(kHpfFreq, 0.0);
      else if (toJ106 && hpf == 0)
        dsp->SetParam(kHpfFreq, 1.0);

      // Update bank offset
      sBankOffset = toJ106 ? 128 : 0;
      return;
    }
  }

  dsp->SetParam(paramIdx, static_cast<double>(value));
}

// (sBankOffset moved above kr106_set_param)

EMSCRIPTEN_KEEPALIVE
void kr106_set_bank_offset(int offset)
{
  sBankOffset = (offset == 0) ? 0 : 128;
}

EMSCRIPTEN_KEEPALIVE
int kr106_get_bank_offset()
{
  return sBankOffset;
}

EMSCRIPTEN_KEEPALIVE
void kr106_load_preset(void* ptr, int presetIdx)
{
  auto* dsp = static_cast<KR106DSP<float>*>(ptr);
  loadPreset(*dsp, presetIdx + sBankOffset);
}

EMSCRIPTEN_KEEPALIVE
int kr106_get_num_presets()
{
  return 128;
}

EMSCRIPTEN_KEEPALIVE
const char* kr106_get_preset_name(int presetIdx)
{
  int abs = presetIdx + sBankOffset;
  if (abs < 0 || abs >= kNumFactoryPresets) return "";
  return kFactoryPresets[abs].name;
}

EMSCRIPTEN_KEEPALIVE
float* kr106_get_scope_ring() { return sScopeRing[sScopeFrontBuf]; }

EMSCRIPTEN_KEEPALIVE
float* kr106_get_scope_ring_r() { return sScopeRingR[sScopeFrontBuf]; }

EMSCRIPTEN_KEEPALIVE
float* kr106_get_scope_sync_ring() { return sScopeSyncRing[sScopeFrontBuf]; }

EMSCRIPTEN_KEEPALIVE
int kr106_get_scope_write_pos() { return sScopeFrontReady ? sScopeFrontLen : 0; }

EMSCRIPTEN_KEEPALIVE
int kr106_get_scope_ring_size() { return kScopeRingSize; }

EMSCRIPTEN_KEEPALIVE
void kr106_scope_consumed() { sScopeFrontConsumed = true; sScopeFrontReady = false; }

EMSCRIPTEN_KEEPALIVE
int kr106_get_preset_value(int presetIdx, int paramIdx)
{
  int abs = presetIdx + sBankOffset;
  if (abs < 0 || abs >= kNumFactoryPresets) return 0;
  if (paramIdx < 0 || paramIdx >= 44) return 0;
  return kFactoryPresets[abs].values[paramIdx];
}

// --- Live slider readback (for JS UI sync after model switch remap) ---

EMSCRIPTEN_KEEPALIVE
float kr106_get_vcf_slider(void* ptr)
{
  return static_cast<KR106DSP<float>*>(ptr)->mSliderVcfFreq;
}

EMSCRIPTEN_KEEPALIVE
float kr106_get_hpf_slider(void* ptr)
{
  return static_cast<KR106DSP<float>*>(ptr)->mSliderHpf;
}

// --- Parameter value queries (for tooltips, shared with JUCE) ---

EMSCRIPTEN_KEEPALIVE
float kr106_vcf_freq_hz(float slider, int j6)
{ return kr106::ParamValue::vcfFreqHz(slider, j6 != 0); }

EMSCRIPTEN_KEEPALIVE
float kr106_lfo_rate_hz(float slider, int j6)
{ return kr106::ParamValue::lfoRateHz(slider, j6 != 0); }

EMSCRIPTEN_KEEPALIVE
float kr106_lfo_delay_ms(float slider)
{ return kr106::ParamValue::lfoDelayMs(slider); }

EMSCRIPTEN_KEEPALIVE
float kr106_dco_lfo_semitones(float slider, int j6)
{ return kr106::ParamValue::dcoLfoSemitones(slider, j6 != 0); }

EMSCRIPTEN_KEEPALIVE
float kr106_vcf_lfo_semitones(float slider, int j6)
{ return kr106::ParamValue::vcfLfoSemitones(slider, j6 != 0); }

EMSCRIPTEN_KEEPALIVE
float kr106_vca_level_db(float slider)
{ return kr106::ParamValue::vcaLevelDb(slider); }

EMSCRIPTEN_KEEPALIVE
float kr106_master_vol_db(float slider)
{ return kr106::ParamValue::masterVolDb(slider); }

EMSCRIPTEN_KEEPALIVE
float kr106_tuning_cents(float slider)
{ return kr106::ParamValue::tuningCents(slider); }

EMSCRIPTEN_KEEPALIVE
float kr106_arp_rate_bpm(float slider)
{ return kr106::ParamValue::arpRateBpm(slider); }

EMSCRIPTEN_KEEPALIVE
float kr106_attack_ms(float slider, int j6)
{ return kr106::ParamValue::attackMs(slider, j6 != 0); }

EMSCRIPTEN_KEEPALIVE
float kr106_dec_rel_ms(float slider, int j6)
{ return kr106::ParamValue::decRelMs(slider, j6 != 0); }

EMSCRIPTEN_KEEPALIVE
float kr106_porta_ms_per_oct(float slider)
{ return kr106::ParamValue::portaMsPerOct(slider); }

} // extern "C"
