// Copyright (c)  2025  Xiaomi Corporation
const sherpa_onnx = require('sherpa-onnx-node');
const {priceInput, loadInput, modelFiles, printAlignments} =
    require('./kokoro-input');

function createOfflineTts() {
  const config = {
    model: {
      kokoro: {
        ...modelFiles('v1.0'),
      },
      debug: true,
      numThreads: 1,
      provider: 'cpu',
    },
    maxNumSentences: 1,
  };
  return new sherpa_onnx.OfflineTts(config);
}

const tts = createOfflineTts();

const generationConfig = new sherpa_onnx.GenerationConfig({
  sid: 9,
  speed: 1.0,
  silenceScale: 0.2,
});


const input = loadInput(priceInput);
const start = Date.now();
const audio = tts.generateFromPhonemes({...input, generationConfig});
const stop = Date.now();
const elapsed_seconds = (stop - start) / 1000;
const duration = audio.samples.length / audio.sampleRate;
const real_time_factor = elapsed_seconds / duration;
console.log('Wave duration', duration.toFixed(3), 'seconds');
console.log('Elapsed', elapsed_seconds.toFixed(3), 'seconds');
console.log(
    `RTF = ${elapsed_seconds.toFixed(3)}/${duration.toFixed(3)} =`,
    real_time_factor.toFixed(3));
printAlignments(audio);

const filename = 'test-kokoro-en-9.wav';
sherpa_onnx.writeWave(
    filename, {samples: audio.samples, sampleRate: audio.sampleRate});

console.log(`Saved to ${filename}`);
