// Copyright (c)  2025  Xiaomi Corporation (authors: Fangjun Kuang)

const sherpa_onnx = require('sherpa-onnx');
const {priceInput, loadInput, modelFiles, printAlignments} =
    require('./kokoro-input');

function createOfflineTts() {
  const offlineTtsKokoroModelConfig = {
    ...modelFiles('v1.0'),
    lengthScale: 1.0,
  };
  let offlineTtsModelConfig = {
    offlineTtsKokoroModelConfig: offlineTtsKokoroModelConfig,
    numThreads: 1,
    debug: 1,
    provider: 'cpu',
  };

  let offlineTtsConfig = {
    offlineTtsModelConfig: offlineTtsModelConfig,
    maxNumSentences: 1,
  };

  return sherpa_onnx.createOfflineTts(offlineTtsConfig);
}

const tts = createOfflineTts();
const speakerId = 9;
const speed = 1.0;
const generationConfig = {
  sid: speakerId,
  speed: speed,
  silenceScale: 0.2,
};
const input = loadInput(priceInput);
const audio = tts.generateFromPhonemes(input, generationConfig);
tts.save('./test-kokoro-en.wav', audio);
printAlignments(audio);
console.log('Saved to test-kokoro-en.wav successfully.');
tts.free();
