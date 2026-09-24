// sherpa-onnx/csrc/offline-tts-kokoro-model-config.cc
//
// Copyright (c)  2025  Xiaomi Corporation

#include "sherpa-onnx/csrc/offline-tts-kokoro-model-config.h"

#include <string>

#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/macros.h"

namespace sherpa_onnx {

void OfflineTtsKokoroModelConfig::Register(ParseOptions *po) {
  po->Register("kokoro-model", &model, "Path to Kokoro model");
  po->Register("kokoro-voices", &voices,
               "Path to voices.bin for Kokoro models");
  po->Register("kokoro-tokens", &tokens,
               "Path to tokens.txt for Kokoro models");
  po->Register("kokoro-length-scale", &length_scale,
               "Speech speed. Larger->Slower; Smaller->faster.");
}

bool OfflineTtsKokoroModelConfig::Validate() const {
  if (model.empty()) {
    SHERPA_ONNX_LOGE("Please provide --kokoro-model");
    return false;
  }

  if (!FileExists(model)) {
    SHERPA_ONNX_LOGE("--kokoro-model: '%s' does not exist", model.c_str());
    return false;
  }

  if (tokens.empty()) {
    SHERPA_ONNX_LOGE("Please provide --kokoro-tokens");
    return false;
  }

  if (!FileExists(tokens)) {
    SHERPA_ONNX_LOGE("--kokoro-tokens: '%s' does not exist", tokens.c_str());
    return false;
  }

  if (voices.empty()) {
    SHERPA_ONNX_LOGE("Please provide --kokoro-voices");
    return false;
  }

  if (!FileExists(voices)) {
    SHERPA_ONNX_LOGE("--kokoro-voices: '%s' does not exist", voices.c_str());
    return false;
  }

  return true;
}

std::string OfflineTtsKokoroModelConfig::ToString() const {
  std::ostringstream os;

  os << "OfflineTtsKokoroModelConfig(";
  os << "model=\"" << model << "\", ";
  os << "voices=\"" << voices << "\", ";
  os << "tokens=\"" << tokens << "\", ";
  os << "length_scale=" << length_scale << ")";

  return os.str();
}

}  // namespace sherpa_onnx
