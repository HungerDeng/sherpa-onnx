// Copyright (c) 2026 Xiaomi Corporation
'use strict';

const fs = require('fs');
const path = require('path');

// phonemes and span.phonemes from the two supplied misaki-rs G2pOutput values.
const priceInput = {
  phonemes: 'ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.',
  spans: [
    'ðə', 'pɹˈaɪs', 'ɪz', 'wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz',
    '.', 'ɐ', 'bˈɪt', 'ɛkspˈɛnsɪv', '.',
  ].map(phonemes => ({phonemes})),
};

const aiInput = {
  phonemes: 'ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm. aɪ kˈænt lˈɪv wɪðˈaʊt ɪt.',
  spans: [
    'ˌeɪˈaɪ', 'ɪz', 'sˌoʊ', 'ˈɔːsʌm', '.', 'aɪ', 'kˈænt', 'lˈɪv',
    'wɪðˈaʊt', 'ɪt', '.',
  ].map(phonemes => ({phonemes})),
};

function loadInput(example) {
  const value = process.argv[2] ?
      JSON.parse(fs.readFileSync(process.argv[2], 'utf8')) : example;
  if (!value || typeof value.phonemes !== 'string' ||
      !Array.isArray(value.spans) ||
      value.spans.some(span => !span || typeof span.phonemes !== 'string')) {
    throw new TypeError('Expected G2pOutput phonemes and spans[].phonemes');
  }
  return {
    phonemes: value.phonemes,
    spans: value.spans.map(span => ({phonemes: span.phonemes})),
  };
}

function modelFiles(version) {
  const dir = path.join(__dirname, '..', 'scripts', 'kokoro', version);
  const files = {
    model: path.join(dir, 'kokoro.onnx'),
    voices: path.join(dir, 'voices.bin'),
    tokens: path.join(dir, 'tokens.txt'),
  };
  for (const filename of Object.values(files)) {
    if (!fs.existsSync(filename)) {
      throw new Error(`Export scripts/kokoro/${version}/run.sh first ` +
          `(model must expose pred_dur): ${filename} is missing`);
    }
  }
  return files;
}

function printAlignments(audio) {
  console.log(`Sample rate: ${audio.sampleRate}; samples: ${audio.samples.length}`);
  for (const span of audio.spanAlignments ?? []) {
    console.log(`${span.startTs.toFixed(3)}-${span.endTs.toFixed(3)} ` +
        `${span.originalPhonemes} -> ${span.inferredPhonemes}`);
  }
}

module.exports = {priceInput, aiInput, loadInput, modelFiles, printAlignments};
