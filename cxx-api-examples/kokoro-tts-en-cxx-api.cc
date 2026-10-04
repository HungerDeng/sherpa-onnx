// cxx-api-examples/kokoro-tts-en-cxx-api.cc
//
// Copyright (c)  2025  Xiaomi Corporation

// This file shows how to use sherpa-onnx CXX API
// for English TTS with Kokoro.
//
// clang-format off
/*
Usage

Use a Kokoro v1.0 model exported with the pred_dur output.

./kokoro-tts-en-cxx-api

 */
// clang-format on

#include <cstdint>
#include <cstdio>
#include <string>

#include "sherpa-onnx/c-api/cxx-api.h"

static int32_t ProgressCallback(const float *samples, int32_t num_samples,
                                float progress, void *arg) {
  fprintf(stderr, "Progress: %.3f%%\n", progress * 100);
  // return 1 to continue generating
  // return 0 to stop generating
  return 1;
}

int32_t main(int32_t argc, char *argv[]) {
  using namespace sherpa_onnx::cxx;  // NOLINT
  OfflineTtsConfig config;

  config.model.kokoro.model = "./kokoro-multi-lang-v1_0/model.onnx";
  config.model.kokoro.voices = "./kokoro-multi-lang-v1_0/voices.bin";
  config.model.kokoro.tokens = "./kokoro-multi-lang-v1_0/tokens.txt";

  config.model.num_threads = 2;

  // If you don't want to see debug messages, please set it to 0
  config.model.debug = 1;

  std::string filename = "./generated-kokoro-en-cxx.wav";
  PhonemeInput input{
      "ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.",
      {{"ðə"}, {"pɹˈaɪs"}, {"ɪz"},
       {"wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz"}, {"."},
       {"ɐ"}, {"bˈɪt"}, {"ɛkspˈɛnsɪv"}, {"."}}};

  auto tts = OfflineTts::Create(config);
  int32_t sid = 0;
  float speed = 1.0;  // larger -> faster in speech speed
  GenerationConfig gen_config;
  gen_config.sid = sid;
  gen_config.speed = speed;
  gen_config.silence_scale = 0.2f;

#if 0
  // If you don't want to use a callback, then please enable this branch
  GeneratedAudio audio = tts.GenerateFromPhonemes(input, gen_config);
#else
  GeneratedAudio audio =
      tts.GenerateFromPhonemes(input, gen_config, ProgressCallback);
#endif

  if (audio.samples.empty()) return 1;
  if (audio.span_alignments) {
    for (const auto &a : *audio.span_alignments) {
      fprintf(stderr, "%s -> %s: %.3f to %.3f s\n",
              a.original_phonemes.c_str(), a.inferred_phonemes.c_str(),
              a.start_ts, a.end_ts);
    }
  }

  WriteWave(filename, {audio.samples, audio.sample_rate});

  fprintf(stderr, "Input phonemes: %s\n", input.phonemes.c_str());
  fprintf(stderr, "Speaker ID is: %d\n", sid);
  fprintf(stderr, "Saved to: %s\n", filename.c_str());

  return 0;
}
