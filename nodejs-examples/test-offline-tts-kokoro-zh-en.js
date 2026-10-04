// Copyright (c)  2025  Xiaomi Corporation (authors: Fangjun Kuang)

const sherpa_onnx = require('sherpa-onnx');
const {aiInput, loadInput, modelFiles, printAlignments} =
    require('./kokoro-input');

function createOfflineTts() {
  const offlineTtsKokoroModelConfig = {
    ...modelFiles('v1.1-zh'),
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
const speakerId = 45;
const speed = 1.0;
const generationConfig = {
  sid: speakerId,
  speed: speed,
  silenceScale: 0.2,
};
const input = loadInput(aiInput);
const audio = tts.generateFromPhonemes(input, generationConfig);
tts.save('./test-kokoro-zh-en-45.wav', audio);
printAlignments(audio);
console.log('Saved to test-kokoro-zh-en-45.wav successfully.');
tts.free();
