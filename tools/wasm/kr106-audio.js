// KR-106 Web Audio loader
//
// Loads the Emscripten WASM module on the main thread and processes
// audio via ScriptProcessorNode. Simple and compatible.
//
// Usage:
//   const synth = new KR106Audio();
//   await synth.init();
//   synth.noteOn(60, 127);
//   synth.noteOff(60);
//   synth.setParam(10, 0.5);
//   synth.loadPreset(0);
//   await synth.initMidi();

class KR106Audio {
  constructor() {
    this.ctx = null;
    this.node = null;
    this.mod = null;
    this.dsp = 0;
    this.ready = false;
    this.ptrL = 0;
    this.ptrR = 0;
  }

  async init() {
    this.ctx = new AudioContext();
    const sr = this.ctx.sampleRate;

    // Load Emscripten module
    const script = document.createElement('script');
    script.src = 'dist/kr106_dsp.js';
    await new Promise((resolve, reject) => {
      script.onload = resolve;
      script.onerror = reject;
      document.head.appendChild(script);
    });

    this.mod = await createKR106();
    this.dsp = this.mod._kr106_create(sr);

    // Pre-allocate WASM heap buffers (max 4096 frames for adaptive sizing)
    const maxFrames = 4096;
    this.ptrL = this.mod._malloc(maxFrames * 4);
    this.ptrR = this.mod._malloc(maxFrames * 4);

    // Adaptive buffer: start at 1024, grow on underruns, up to 4096
    this._bufferSize = 1024;
    this._underrunCount = 0;
    this._lastCallbackTime = 0;
    this._createNode();
    this.ready = true;
    console.log('KR-106 ready: ' + sr + ' Hz, buffer=' + this._bufferSize);
  }

  _createNode() {
    if (this.node) { this.node.disconnect(); this.node = null; }
    this.node = this.ctx.createScriptProcessor(this._bufferSize, 0, 2);
    const self = this;
    this.node.onaudioprocess = (e) => {
      const outL = e.outputBuffer.getChannelData(0);
      const outR = e.outputBuffer.getChannelData(1);
      const n = outL.length;

      self.mod._kr106_process(self.dsp, self.ptrL, self.ptrR, n);

      const heap = self.mod.HEAPF32;
      const offL = self.ptrL >> 2;
      const offR = self.ptrR >> 2;
      outL.set(heap.subarray(offL, offL + n));
      outR.set(heap.subarray(offR, offR + n));

      // Detect underruns: if the gap between callbacks is > 2x expected, count it
      const now = performance.now();
      const expectedMs = (self._bufferSize / self.ctx.sampleRate) * 1000;
      if (self._lastCallbackTime > 0) {
        const gap = now - self._lastCallbackTime;
        if (gap > expectedMs * 2) {
          self._underrunCount++;
          if (self._underrunCount >= 3 && self._bufferSize < 4096) {
            self._bufferSize *= 2;
            self._underrunCount = 0;
            console.log('KR-106: buffer underrun, increasing to ' + self._bufferSize);
            setTimeout(() => self._createNode(), 0);
          }
        }
      }
      self._lastCallbackTime = now;
    };
    this.node.connect(this.ctx.destination);
  }

  noteOn(note, velocity = 127) {
    if (this.ready) this.mod._kr106_note_on(this.dsp, note, velocity);
  }

  noteOff(note) {
    if (this.ready) this.mod._kr106_note_off(this.dsp, note);
  }

  forceRelease(note) {
    if (this.ready) this.mod._kr106_force_release(this.dsp, note);
  }

  controlChange(cc, value) {
    if (this.ready) this.mod._kr106_control_change(this.dsp, cc, value);
  }

  setParam(index, value) {
    if (this.ready) this.mod._kr106_set_param(this.dsp, index, value);
  }

  loadPreset(index) {
    if (this.ready) this.mod._kr106_load_preset(this.dsp, index);
  }

  setBankOffset(offset) {
    if (this.mod && this.mod._kr106_set_bank_offset) this.mod._kr106_set_bank_offset(offset);
  }

  getPresetName(index) {
    if (!this.mod) return '';
    const ptr = this.mod._kr106_get_preset_name(index);
    return this.mod.UTF8ToString(ptr);
  }

  getNumPresets() {
    if (!this.mod) return 0;
    return this.mod._kr106_get_num_presets();
  }

  getPresetValue(presetIdx, paramIdx) {
    if (!this.mod) return 0;
    return this.mod._kr106_get_preset_value(presetIdx, paramIdx);
  }

  getVcfSlider() {
    return this.ready ? this.mod._kr106_get_vcf_slider(this.dsp) : 0;
  }

  getHpfSlider() {
    return this.ready ? this.mod._kr106_get_hpf_slider(this.dsp) : 0;
  }

  setVoices(n) {
    if (this.ready) this.mod._kr106_set_voices(this.dsp, n);
  }

  setIgnoreVelocity(b) {
    if (this.ready) this.mod._kr106_set_ignore_velocity(this.dsp, b ? 1 : 0);
  }

  getScopeData(outArray) {
    if (!this.mod) return 0;
    const ringPtr = this.mod._kr106_get_scope_ring();
    const ringRPtr = this.mod._kr106_get_scope_ring_r();
    const syncPtr = this.mod._kr106_get_scope_sync_ring();
    const writePos = this.mod._kr106_get_scope_write_pos();
    const ringSize = this.mod._kr106_get_scope_ring_size();
    const heap = this.mod.HEAPF32;
    const ringOff = ringPtr >> 2;
    const ringROff = ringRPtr >> 2;
    const syncOff = syncPtr >> 2;
    return { heap, ringOff, ringROff, syncOff, writePos, ringSize };
  }

  async initMidi() {
    if (!navigator.requestMIDIAccess) {
      console.warn('Web MIDI not available');
      return;
    }
    const midi = await navigator.requestMIDIAccess();
    const connect = (input) => {
      console.log('MIDI: ' + input.name);
      input.onmidimessage = (e) => {
        const d = e.data;
        const st = d[0] & 0xF0;
        if (st === 0x90 && d[2] > 0) {
          this.noteOn(d[1], d[2]);
          if (this.onMidiNote) this.onMidiNote(d[1], true);
        }
        else if (st === 0x80 || (st === 0x90 && d[2] === 0)) {
          this.noteOff(d[1]);
          if (this.onMidiNote) this.onMidiNote(d[1], false);
        }
        else if (st === 0xE0) {
          const bend = ((d[2] << 7) | d[1]) / 8192 - 1; // -1..+1
          this.setParam(36, bend); // kBender
        }
        else if (st === 0xB0) {
          if (d[1] === 1) this.controlChange(1, d[2] / 127); // mod wheel -> LFO trigger
        }
      };
    };
    for (const input of midi.inputs.values()) connect(input);
    midi.onstatechange = (e) => {
      if (e.port.type === 'input' && e.port.state === 'connected') connect(e.port);
    };
  }

  async resume() {
    if (this.ctx && this.ctx.state === 'suspended') await this.ctx.resume();
  }
}

if (typeof module !== 'undefined') module.exports = KR106Audio;
if (typeof window !== 'undefined') window.KR106Audio = KR106Audio;
