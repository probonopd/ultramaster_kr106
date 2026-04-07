// KR-106 AudioWorkletProcessor
//
// Runs on the audio thread. Loads the Emscripten WASM module and
// processes 128-sample blocks. Receives MIDI and parameter messages
// from the main thread via MessagePort.
//
// The Emscripten glue JS (kr106_dsp.js) must be imported before
// this processor is registered. The main thread handles this by
// creating a combined blob that imports both.

class KR106Processor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.dsp = 0;
    this.mod = null;
    this.ready = false;
    this.midiQueue = [];
    this.ptrL = 0;
    this.ptrR = 0;
    this.allocatedFrames = 0;

    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  onMessage(msg) {
    if (msg.type === 'init') {
      this.initWasm(msg.wasmUrl);
      return;
    }
    if (!this.ready) return;

    if (msg.type === 'midi') {
      this.midiQueue.push(msg.data);
    } else if (msg.type === 'param') {
      this.mod._kr106_set_param(this.dsp, msg.index, msg.value);
    } else if (msg.type === 'preset') {
      this.mod._kr106_load_preset(this.dsp, msg.index);
      // Send preset name back to main thread
      const namePtr = this.mod._kr106_get_preset_name(msg.index);
      const name = this.mod.UTF8ToString(namePtr);
      this.port.postMessage({ type: 'presetName', index: msg.index, name });
    }
  }

  async initWasm(wasmUrl) {
    try {
      // Import the Emscripten glue (makes createKR106 available)
      importScripts(wasmUrl);

      // Call the factory — it fetches the .wasm file relative to the glue JS
      this.mod = await createKR106();

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

    // Drain MIDI queue
    for (const midi of this.midiQueue) {
      const status = midi[0] & 0xF0;
      if (status === 0x90 && midi[2] > 0)
        this.mod._kr106_note_on(this.dsp, midi[1], midi[2]);
      else if (status === 0x80 || (status === 0x90 && midi[2] === 0))
        this.mod._kr106_note_off(this.dsp, midi[1]);
    }
    this.midiQueue.length = 0;

    const output = outputs[0];
    const nFrames = output[0].length;

    this.ensureBuffers(nFrames);

    this.mod._kr106_process(this.dsp, this.ptrL, this.ptrR, nFrames);

    // Copy from WASM heap to output
    output[0].set(this.mod.HEAPF32.subarray(this.ptrL >> 2, (this.ptrL >> 2) + nFrames));
    if (output.length > 1)
      output[1].set(this.mod.HEAPF32.subarray(this.ptrR >> 2, (this.ptrR >> 2) + nFrames));

    return true;
  }
}

registerProcessor('kr106-processor', KR106Processor);
