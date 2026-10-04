// sherpa-onnx/csrc/offline-tts-kokoro-model.h
//
// Copyright (c)  2025  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_MODEL_H_
#define SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_MODEL_H_

#include <memory>
#include <string>

#include "onnxruntime_cxx_api.h"  // NOLINT
#include "sherpa-onnx/csrc/offline-tts-kokoro-model-meta-data.h"
#include "sherpa-onnx/csrc/offline-tts-model-config.h"

namespace sherpa_onnx {

struct KokoroModelOutput {
  Ort::Value audio{nullptr};
  Ort::Value pred_dur{nullptr};
};

class OfflineTtsKokoroModel {
 public:
  ~OfflineTtsKokoroModel();

  explicit OfflineTtsKokoroModel(const OfflineTtsModelConfig &config);

  template <typename Manager>
  OfflineTtsKokoroModel(Manager *mgr, const OfflineTtsModelConfig &config);

  // Return audio samples and one int64 predicted duration per input token.
  KokoroModelOutput Run(Ort::Value x, int64_t sid = 0,
                        float speed = 1.0) const;

  const OfflineTtsKokoroModelMetaData &GetMetaData() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_MODEL_H_
