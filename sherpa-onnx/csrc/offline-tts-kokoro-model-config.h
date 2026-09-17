// sherpa-onnx/csrc/offline-tts-kokoro-model-config.h
//
// Copyright (c)  2025  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_MODEL_CONFIG_H_
#define SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_MODEL_CONFIG_H_

#include <string>

#include "sherpa-onnx/csrc/parse-options.h"

namespace sherpa_onnx {

struct OfflineTtsKokoroModelConfig {
  std::string model;
  std::string voices;
  std::string tokens;

  // Note: You can pass multiple files, separated by ",", to lexicon
  // Example: lexicon = "./lexicon-gb-en.txt,./lexicon-cmn.txt";
  std::string lexicon;

  std::string data_dir;

  std::string dict_dir;

  // speed = 1 / length_scale
  float length_scale = 1.0;

  // Required at generation time unless GenerationConfig.extra["lang"] is
  // provided. A per-generation value takes precedence over this field.
  // Supported values: en-us, en-gb, ja-cutlet, ja-jtalk, cmn, es, fr, hi, it,
  // and pt-br.
  // See https://hf-mirror.com/hexgrad/Kokoro-82M/blob/main/VOICES.md
  std::string lang;

  OfflineTtsKokoroModelConfig() = default;

  OfflineTtsKokoroModelConfig(const std::string &model,
                              const std::string &voices,
                              const std::string &tokens,
                              const std::string &lexicon,
                              const std::string &data_dir,
                              const std::string &dict_dir, float length_scale,
                              const std::string &lang)
      : model(model),
        voices(voices),
        tokens(tokens),
        lexicon(lexicon),
        data_dir(data_dir),
        dict_dir(dict_dir),
        length_scale(length_scale),
        lang(lang) {}

  void Register(ParseOptions *po);
  bool Validate() const;

  std::string ToString() const;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_MODEL_CONFIG_H_
