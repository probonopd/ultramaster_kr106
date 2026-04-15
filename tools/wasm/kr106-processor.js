// KR-106 AudioWorkletProcessor
//
// Runs on the audio thread. Loads the Emscripten WASM module and
// processes 128-sample blocks. Receives MIDI and parameter messages
// from the main thread via MessagePort.

class KR106Processor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.dsp = 0;
    this.mod = null;
    this.ready = false;
    this.ptrL = 0;
    this.ptrR = 0;
    this.allocatedFrames = 0;
    this.scopeCounter = 0;
    this.scopeInterval = 6; // send scope data every N process calls (~21ms at 128 samples/48kHz)

    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  onMessage(msg) {
    if (msg.type === 'init') {
      this.initWasm(msg.wasmText, msg.wasmBinary);
      return;
    }
    if (!this.ready) return;

    if (msg.type === 'midi') {
      const d = msg.data;
      const st = d[0] & 0xF0;
      if (st === 0x90 && d[2] > 0)
        this.mod._kr106_note_on(this.dsp, d[1], d[2]);
      else if (st === 0x80 || (st === 0x90 && d[2] === 0))
        this.mod._kr106_note_off(this.dsp, d[1]);
      else if (st === 0xB0)
        this.mod._kr106_control_change(this.dsp, d[1], d[2] / 127);
    } else if (msg.type === 'bankOffset') {
      if (this.mod._kr106_set_bank_offset) this.mod._kr106_set_bank_offset(msg.value);
    } else if (msg.type === 'param') {
      this.mod._kr106_set_param(this.dsp, msg.index, msg.value);
      // Model switch: send back remapped VCF/HPF slider positions
      if (msg.index === 43) { // kAdsrMode
        const vcf = this.mod._kr106_get_vcf_slider(this.dsp);
        const hpf = this.mod._kr106_get_hpf_slider(this.dsp);
        this.port.postMessage({ type: 'sliderUpdate', vcfSlider: vcf, hpfSlider: hpf });
      }
    } else if (msg.type === 'preset') {
      this.mod._kr106_load_preset(this.dsp, msg.index);
      const namePtr = this.mod._kr106_get_preset_name(msg.index);
      const name = this.mod.UTF8ToString(namePtr);
      this.port.postMessage({ type: 'presetName', index: msg.index, name });
      // Send back all 44 preset parameter values for UI sync
      const vals = new Float32Array(44);
      for (let i = 0; i < 44; i++)
        vals[i] = this.mod._kr106_get_preset_value(msg.index, i);
      // Also send remapped VCF/HPF slider positions
      const vcfSlider = this.mod._kr106_get_vcf_slider(this.dsp);
      const hpfSlider = this.mod._kr106_get_hpf_slider(this.dsp);
      this.port.postMessage({ type: 'presetValues', index: msg.index, values: vals, vcfSlider, hpfSlider });
    } else if (msg.type === 'preset-name-query') {
      const namePtr = this.mod._kr106_get_preset_name(msg.index);
      const name = this.mod.UTF8ToString(namePtr);
      this.port.postMessage({ type: 'presetName', index: msg.index, name });
    } else if (msg.type === 'voices') {
      this.mod._kr106_set_voices(this.dsp, msg.count);
    } else if (msg.type === 'ignoreVel') {
      this.mod._kr106_set_ignore_velocity(this.dsp, msg.value);
    }
  }

  async initWasm(wasmText, wasmBinary) {
    try {
      // AudioWorklet has no importScripts() or fetch().
      // Main thread fetches both the glue JS and .wasm binary, sends via postMessage.
      (0, eval)(wasmText); // indirect eval in global scope defines createKR106
      this.mod = await createKR106({ wasmBinary: wasmBinary });
      this.dsp = this.mod._kr106_create(sampleRate);
      this.ready = true;
      this.port.postMessage({ type: 'ready' });
    } catch (err) {
      this.port.postMessage({ type: 'error', message: err.toString() });
    }
  }

  ensureBuffers(nFrames) {
    if (this.allocatedFrames >= nFrames) return;
    if (this.ptrL) this.mod._free(this.ptrL);
    if (this.ptrR) this.mod._free(this.ptrR);
    this.ptrL = this.mod._malloc(nFrames * 4);
    this.ptrR = this.mod._malloc(nFrames * 4);
    this.allocatedFrames = nFrames;
  }

  process(inputs, outputs, parameters) {
    if (!this.ready) return true;

    const output = outputs[0];
    const nFrames = output[0].length;

    this.ensureBuffers(nFrames);

    this.mod._kr106_process(this.dsp, this.ptrL, this.ptrR, nFrames);

    const heap = this.mod.HEAPF32;
    output[0].set(heap.subarray(this.ptrL >> 2, (this.ptrL >> 2) + nFrames));
    if (output.length > 1)
      output[1].set(heap.subarray(this.ptrR >> 2, (this.ptrR >> 2) + nFrames));

    // Send scope data periodically (not every 128-sample block)
    this.scopeCounter++;
    if (this.scopeCounter >= this.scopeInterval) {
      this.scopeCounter = 0;
      const ringPtr = this.mod._kr106_get_scope_ring();
      const ringRPtr = this.mod._kr106_get_scope_ring_r();
      const syncPtr = this.mod._kr106_get_scope_sync_ring();
      const writePos = this.mod._kr106_get_scope_write_pos();
      const ringSize = this.mod._kr106_get_scope_ring_size();

      if (writePos > 0) {
        // Copy scope data to transferable arrays
        const L = new Float32Array(writePos);
        const R = new Float32Array(writePos);
        const S = new Float32Array(writePos);
        const off = ringPtr >> 2;
        const offR = ringRPtr >> 2;
        const offS = syncPtr >> 2;
        L.set(heap.subarray(off, off + writePos));
        R.set(heap.subarray(offR, offR + writePos));
        S.set(heap.subarray(offS, offS + writePos));

        this.port.postMessage({
          type: 'scope',
          ringL: L, ringR: R, sync: S,
          writePos, ringSize
        }, [L.buffer, R.buffer, S.buffer]); // transfer ownership

        if (this.mod._kr106_scope_consumed)
          this.mod._kr106_scope_consumed();
      }
    }

    return true;
  }
}

registerProcessor('kr106-processor', KR106Processor);
