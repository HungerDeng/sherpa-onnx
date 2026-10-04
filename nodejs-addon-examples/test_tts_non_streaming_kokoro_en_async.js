// Copyright (c)  2026  Xiaomi Corporation
//
// Asynchronous model creation with Kokoro phoneme inference.
//
const sherpa_onnx = require('sherpa-onnx-node');
const {priceInput, loadInput, modelFiles, printAlignments} =
    require('./kokoro-input');

async function createOfflineTts() {
  const config = {
    model: {
      kokoro: {
        ...modelFiles('v1.0'),
      },
      debug: false,
      numThreads: 1,
      provider: 'cpu',
    },
    maxNumSentences: 1,
  };
  return await sherpa_onnx.OfflineTts.createAsync(config);
}

async function main() {
  const tts = await createOfflineTts();

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

  const filename = 'test-kokoro-en-async.wav';
  sherpa_onnx.writeWave(
      filename, {samples: audio.samples, sampleRate: audio.sampleRate});
  console.log(`Saved to ${filename}`);
}

main().catch((err) => {
  console.error('Error:', err);
  process.exitCode = 1;
});
