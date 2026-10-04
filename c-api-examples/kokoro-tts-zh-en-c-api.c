// c-api-examples/kokoro-tts-zh-en-c-api.c
//
// Copyright (c)  2025  Xiaomi Corporation

// This file shows how to use sherpa-onnx C API
// for English + Chinese TTS with Kokoro.
//
// clang-format off
/*
Usage


Export Kokoro v1.0 with pred_dur using scripts/kokoro/v1.0/run.sh.
Place model.onnx, voices.bin, and tokens.txt in kokoro-multi-lang-v1_0.

./kokoro-tts-zh-en-c-api

 */
// clang-format on

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherpa-onnx/c-api/c-api.h"

static int32_t ProgressCallback(const float *samples, int32_t num_samples,
                                float progress, void *arg) {
  fprintf(stderr, "Progress: %.3f%%\n", progress * 100);
  // return 1 to continue generating
  // return 0 to stop generating
  return 1;
}

int32_t main(int32_t argc, char *argv[]) {
  SherpaOnnxOfflineTtsConfig config;
  memset(&config, 0, sizeof(config));
  config.model.kokoro.model = "./kokoro-multi-lang-v1_0/model.onnx";
  config.model.kokoro.voices = "./kokoro-multi-lang-v1_0/voices.bin";
  config.model.kokoro.tokens = "./kokoro-multi-lang-v1_0/tokens.txt";

  config.model.num_threads = 2;

  // If you don't want to see debug messages, please set it to 0
  config.model.debug = 1;

  const char *filename = "./generated-kokoro-zh-en.wav";
  const SherpaOnnxPhonemeSpan spans[] = {
      {"ˌeɪˈaɪ"}, {"ɪz"}, {"sˌoʊ"}, {"ˈɔːsʌm"}, {"."}, {"aɪ"},
      {"kˈænt"}, {"lˈɪv"}, {"wɪðˈaʊt"}, {"ɪt"}, {"."}};
  const SherpaOnnxPhonemeInput input = {
      "ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm. aɪ kˈænt lˈɪv wɪðˈaʊt ɪt.",
      spans, sizeof(spans) / sizeof(spans[0])};

  const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);
  int32_t sid = 0;    // there are 53 speakers
  float speed = 1.0;  // larger -> faster in speech speed
  SherpaOnnxGenerationConfig cfg = {0};
  cfg.silence_scale = 0.2f;
  cfg.sid = sid;
  cfg.speed = speed;

#if 0
  // If you don't want to use a callback, then please enable this branch
  const SherpaOnnxGeneratedAudio *audio =
      SherpaOnnxOfflineTtsGenerateFromPhonemesWithConfig(tts, &input, &cfg,
                                                         NULL, NULL);
#else
  const SherpaOnnxGeneratedAudio *audio =
      SherpaOnnxOfflineTtsGenerateFromPhonemesWithConfig(
          tts, &input, &cfg, ProgressCallback, NULL);
#endif

  if (!audio) {
    fprintf(stderr, "Kokoro generation failed\n");
    SherpaOnnxDestroyOfflineTts(tts);
    return 1;
  }

  for (int32_t i = 0; i < audio->num_span_alignments; ++i) {
    const SherpaOnnxSpanAlignment *a = &audio->span_alignments[i];
    fprintf(stderr, "%s -> %s: %.3f to %.3f s\n", a->original_phonemes,
            a->inferred_phonemes, a->start_ts, a->end_ts);
  }

  SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, filename);

  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  SherpaOnnxDestroyOfflineTts(tts);

  fprintf(stderr, "Input phonemes: %s\n", input.phonemes);
  fprintf(stderr, "Speaker ID is: %d\n", sid);
  fprintf(stderr, "Saved to: %s\n", filename);

  return 0;
}
