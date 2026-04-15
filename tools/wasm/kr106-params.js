// kr106-params.js — Parameter definitions and value formatters
// All globals are shared with kr106-controls.js, kr106-scope.js, kr106-ui.js
//
// Tooltip values come from the DSP via WASM (kr106::ParamValue).
// Only string formatting lives here.

const W=940,H=224,S=Math.min(2,window.devicePixelRatio||1);
const synth=new KR106Audio();
let currentPreset=0;
let presetDirty=false;

const P={benderDco:0,benderVcf:1,arpRate:2,lfoRate:3,lfoDelay:4,dcoLfo:5,dcoPwm:6,dcoSub:7,dcoNoise:8,hpfFreq:9,vcfFreq:10,vcfRes:11,vcfEnv:12,vcfLfo:13,vcfKbd:14,vcaLevel:15,envA:16,envD:17,envS:18,envR:19,transpose:20,hold:21,arpeggio:22,dcoPulse:23,dcoSaw:24,dcoSubSw:25,chorusOff:26,chorusI:27,chorusII:28,octTranspose:29,arpMode:30,arpRange:31,lfoMode:32,pwmMode:33,vcfEnvInv:34,vcaMode:35,bender:36,tuning:37,power:38,portaMode:39,portaRate:40,transposeOfs:41,benderLfo:42,adsrMode:43,masterVol:44};
const paramValues=new Float32Array(45);

const paramNames={
[P.benderDco]:'Bender DCO',[P.benderVcf]:'Bender VCF',[P.benderLfo]:'Bender LFO',
[P.arpRate]:'Arp Rate',[P.lfoRate]:'LFO Rate',[P.lfoDelay]:'LFO Delay',
[P.dcoLfo]:'DCO LFO',[P.dcoPwm]:'DCO PWM',[P.dcoSub]:'DCO Sub',[P.dcoNoise]:'DCO Noise',
[P.hpfFreq]:'HPF',[P.vcfFreq]:'VCF Freq',[P.vcfRes]:'VCF Res',
[P.vcfEnv]:'VCF Env',[P.vcfLfo]:'VCF LFO',[P.vcfKbd]:'VCF Kbd',
[P.vcaLevel]:'Volume',[P.envA]:'Attack',[P.envD]:'Decay',[P.envS]:'Sustain',[P.envR]:'Release',
[P.arpMode]:'Arp Mode',[P.arpRange]:'Arp Range',[P.lfoMode]:'LFO Mode',
[P.pwmMode]:'PWM Mode',[P.vcfEnvInv]:'VCF Env Inv',[P.vcaMode]:'VCA Mode',
[P.portaMode]:'Porta Mode',[P.portaRate]:'Porta Rate',
[P.octTranspose]:'Octave',[P.adsrMode]:'J60 / J106',
[P.tuning]:'Tuning',[P.masterVol]:'Master Volume',[P.bender]:'Bender',
[P.transpose]:'Transpose',[P.hold]:'Hold',[P.arpeggio]:'Arpeggio',
[P.dcoPulse]:'Pulse',[P.dcoSaw]:'Saw',[P.dcoSubSw]:'Sub',
[P.chorusI]:'Chorus I',[P.chorusII]:'Chorus II',[P.power]:'Power'};

const switchLabels={
[P.arpMode]:['Up','Down','Up/Down'],
[P.arpRange]:['1 Oct','2 Oct','3 Oct'],
[P.lfoMode]:['Triangle','Square'],
[P.pwmMode]:['LFO','Manual','Env'],
[P.vcfEnvInv]:['Normal','Inverted'],
[P.vcaMode]:['Gate','Env'],
[P.portaMode]:['Off','On','Key'],
[P.octTranspose]:["4'","8'","16'"],
[P.adsrMode]:['J60','J106']};

// String formatting helpers (UI only)
function fmtPct(v){return Math.round(v*100)+'% ['+Math.round(v*127)+']'}
function fmtMs(ms){return ms>=1000?(ms/1000).toFixed(2)+' s':Math.round(ms)+' ms'}
function fmtHz(hz){return hz>=1000?(hz/1000).toFixed(1)+' kHz':hz.toFixed(1)+' Hz'}

// WASM param value helpers (call into kr106::ParamValue via C API)
function _pv(fn,v,j6){return synth.mod?synth.mod[fn](v,j6?1:0):0}
function _pv1(fn,v){return synth.mod?synth.mod[fn](v):0}

function paramValueText(p,v){
  const j6=paramValues[P.adsrMode]<0.5;

  if(p===P.vcfFreq)return fmtHz(_pv('_kr106_vcf_freq_hz',v,j6))
  if(p===P.vcfRes)return fmtPct(v)
  if(p===P.vcfEnv||p===P.vcfKbd)return fmtPct(v)
  if(p===P.vcfLfo){const st=_pv('_kr106_vcf_lfo_semitones',v,j6);return st<0.5?'Off':st.toFixed(1)+' st'}
  if(p===P.dcoLfo){const st=_pv('_kr106_dco_lfo_semitones',v,j6);return st<0.05?'Off':st.toFixed(1)+' st'}
  if(p===P.lfoRate)return _pv('_kr106_lfo_rate_hz',v,j6).toFixed(1)+' Hz'
  if(p===P.lfoDelay){const ms=_pv1('_kr106_lfo_delay_ms',v);return ms<=0?'Off':Math.round(ms)+' ms'}
  if(p===P.vcaLevel){const dB=_pv1('_kr106_vca_level_db',v);return(dB>=0?'+':'')+dB.toFixed(1)+' dB'}
  if(p===P.masterVol){const dB=_pv1('_kr106_master_vol_db',v);return dB<=-200?'-inf dB':(dB>=0?'+':'')+dB.toFixed(1)+' dB'}
  if(p===P.tuning){const c=Math.round(_pv1('_kr106_tuning_cents',v*2-1));return(c>=0?'+':'')+c+' cents'}
  if(p===P.arpRate)return Math.round(_pv1('_kr106_arp_rate_bpm',v))+' bpm'
  if(p===P.envA)return fmtMs(_pv('_kr106_attack_ms',v,j6))
  if(p===P.envD||p===P.envR)return fmtMs(_pv('_kr106_dec_rel_ms',v,j6))
  if(p===P.envS)return fmtPct(v)
  if(p===P.hpfFreq){
    const pos=Math.min(3,Math.round(v*3));
    if(j6){const labels=['Flat','122 Hz','269 Hz','571 Hz'];return labels[pos]}
    const labels=['Bass Boost','Flat','236 Hz','754 Hz'];return labels[pos]}
  if(p===P.portaRate){const ms=_pv1('_kr106_porta_ms_per_oct',v);return ms<=0?'Off':fmtMs(ms)+'/oct'}
  return fmtPct(v)
}
