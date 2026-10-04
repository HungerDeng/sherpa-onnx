// TTS Web Worker — runs WASM generation off the main thread.
//
// Config helpers (initSherpaOnnxOfflineTtsConfig, freeConfig,
// initSherpaOnnxGenerationConfig, freeSherpaOnnxGenerationConfig)
// are loaded from sherpa-onnx-tts.js via eval at init time.
//
// ── Messages: Main Thread → Worker ─────────────────────────────────────────
//
// init — Initialize the WASM module and create a TTS instance.
//   {
//     type:          'init',
//     jsGlueSource:  String,       // sherpa-onnx-wasm-web.js (defines SherpaOnnx factory)
//     ttsJsSource:   String,       // sherpa-onnx-tts.js (config/generation helpers)
//     wasmBinary:    ArrayBuffer,  // compiled .wasm module
//     modelFiles:    Object,       // { "relative/path": ArrayBuffer, ... }
//     config:        Object,       // OfflineTtsConfig JSON (from toJson())
//   }
//
// generate — Synthesize speech from text.
//   {
//     type:                 'generate',
//     text:                 String,     // text to synthesize
//     sid:                  Number,     // speaker ID (default 0)
//     speed:                Number,     // speech rate (default 1.0)
//     generationId:         Number,     // id for matching chunks/done (default 0)
//     referenceAudio:       ArrayBuffer, // optional: Float32 PCM samples for voice cloning
//     referenceSampleRate:  Number,     // sample rate of reference audio
//     numSteps:             Number,     // diffusion steps (default 5)
//   }
//
// cancel — Abort the current generation.
//   { type: 'cancel' }
//
// dispose — Destroy the TTS instance and close the worker.
//   { type: 'dispose' }
//
// ── Messages: Worker → Main Thread ─────────────────────────────────────────
//
// ready — TTS initialized successfully.
//   { type: 'ready', numSpeakers: Number, sampleRate: Number }
//
// chunk — Streaming audio chunk (sent during generation).
//   {
//     type:         'chunk',
//     samples:      ArrayBuffer,  // Float32 PCM (transferred, not copied)
//     progress:     Number,       // 0.0–1.0
//     sampleRate:   Number,
//     generationId: Number,
//   }
//
// done — Generation complete.
//   {
//     type:         'done',
//     samples:      ArrayBuffer,  // Float32 PCM (transferred)
//     sampleRate:   Number,
//     duration:     Number,       // audio duration in seconds
//     elapsed:      Number,       // wall-clock time in seconds
//     generationId: Number,
//   }
//
// log — Debug/info message from the WASM module (stdout/stderr).
//   { type: 'log', message: String }
//
// error — Error message.
//   { type: 'error', message: String }

let Module = null;
let tts = null;
let _cancelled = false;

// ── Emscripten FS helpers ────────────────────────────────────────────────

function getFS() {
  if (Module && Module.FS) return Module.FS;
  if (typeof FS !== 'undefined') return FS;
  throw new Error('FS not found');
}

function mkdirTree(path) {
  const fs = getFS();
  const parts = path.split('/');
  let current = '';
  for (const part of parts) {
    if (!part) continue;
    current = current + '/' + part;
    try { fs.mkdir(current); } catch (_) {}
  }
}

function writeFile(path, data) {
  getFS().writeFile(path, data);
}

// ── Message handler ──────────────────────────────────────────────────────

self.onmessage = async function(e) {
  const msg = e.data;

  if (msg.type === 'init') {
    try {
      // 1. Load Emscripten JS glue (defines SherpaOnnx factory).
      if (msg.jsGlueSource) {
        self.eval(msg.jsGlueSource);
      }

      // 2. Load sherpa-onnx-tts.js helpers (defines initSherpaOnnxOfflineTtsConfig,
      //    freeConfig, initSherpaOnnxGenerationConfig, freeSherpaOnnxGenerationConfig, etc.)
      if (msg.ttsJsSource) {
        self.eval(msg.ttsJsSource);
      }

      // 3. Compile WASM module.
      const wasmBytes = new Uint8Array(msg.wasmBinary);
      Module = await SherpaOnnx({
        wasmBinary: wasmBytes,
        print: (text) => self.postMessage({ type: 'log', message: text }),
        printErr: (text) => self.postMessage({ type: 'log', message: '[stderr] ' + text }),
      });

      // 4. Write model files to WASM FS.
      const modelFiles = msg.modelFiles;
      for (const [path, bytes] of Object.entries(modelFiles)) {
        const dir = path.substring(0, path.lastIndexOf('/'));
        if (dir) mkdirTree(dir);
        writeFile(path, new Uint8Array(bytes));
      }

      // 5. Create TTS instance using sherpa-onnx-tts.js helper.
      tts = createOfflineTts(Module, msg.config);
      if (!tts || !tts.handle) {
        self.postMessage({ type: 'error', message: 'Failed to create TTS (null handle)' });
        return;
      }
      self.postMessage({ type: 'ready', numSpeakers: tts.numSpeakers,
        sampleRate: tts.sampleRate });
    } catch (e) {
      self.postMessage({ type: 'error', message: e.message || String(e) });
    }
  }

  else if (msg.type === 'generate' && tts) {
    try {
      const startTime = performance.now();

      const genCfg = {
        silenceScale: 0.2,
        speed: msg.speed || 1.0,
        sid: msg.sid || 0,
      };

      // Reference audio for voice cloning (e.g. Pocket TTS).
      if (msg.referenceAudio) {
        genCfg.referenceAudio = new Float32Array(msg.referenceAudio);
        genCfg.referenceSampleRate = msg.referenceSampleRate || 0;
        genCfg.numSteps = msg.numSteps || 5;
      }

      // Set up callback for streaming chunks.
      _cancelled = false;
      const genId = msg.generationId || 0;
      genCfg.callback = (samples, n, progress, arg) => {
        if (_cancelled) return 0;
        samples = new Float32Array(samples);
        self.postMessage({
          type: 'chunk',
          samples: samples.buffer,
          progress: progress,
          sampleRate: tts.sampleRate,
          generationId: genId,
        }, [samples.buffer]);
        return 1;
      };

      const output = msg.phonemeInput
          ? tts.generateFromPhonemes(msg.phonemeInput, genCfg)
          : tts.generateWithConfig(msg.text, genCfg);
      const samples = output.samples;
      const sampleRateOut = output.sampleRate;

      const elapsed = (performance.now() - startTime) / 1000;
      const duration = samples.length / sampleRateOut;

      self.postMessage({
        type: 'done',
        samples: samples.buffer,
        sampleRate: sampleRateOut,
        duration: duration,
        elapsed: elapsed,
        generationId: genId,
        spanAlignments: output.spanAlignments,
      }, [samples.buffer]);
    } catch (e) {
      self.postMessage({ type: 'error', message: e.message || String(e) });
    }
  }

  else if (msg.type === 'cancel') {
    _cancelled = true;
  }

  else if (msg.type === 'dispose') {
    if (tts) {
      tts.free();
      tts = null;
    }
    self.close();
  }
};
