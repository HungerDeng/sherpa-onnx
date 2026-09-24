// sherpa-onnx/csrc/offline-tts-kokoro-impl.h
//
// Copyright (c)  2025  Xiaomi Corporation
#ifndef SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_IMPL_H_
#define SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_IMPL_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/offline-tts-impl.h"
#include "sherpa-onnx/csrc/offline-tts-kokoro-model.h"
#include "sherpa-onnx/csrc/symbol-table.h"
#include "sherpa-onnx/csrc/text-utils.h"

namespace sherpa_onnx {

class OfflineTtsKokoroImpl : public OfflineTtsImpl {
 public:
  explicit OfflineTtsKokoroImpl(const OfflineTtsConfig &config)
      : config_(config),
        model_(std::make_unique<OfflineTtsKokoroModel>(config.model)) {
    ValidateConfig();
    auto is = OpenInputFile(config.model.kokoro.tokens);
    InitTokens(is);
  }

  template <typename Manager>
  OfflineTtsKokoroImpl(Manager *mgr, const OfflineTtsConfig &config)
      : config_(config),
        model_(std::make_unique<OfflineTtsKokoroModel>(mgr, config.model)) {
    ValidateConfig();
    auto buf = ReadFile(mgr, config.model.kokoro.tokens);
    std::istringstream is(std::string(buf.data(), buf.size()));
    InitTokens(is);
  }

  int32_t SampleRate() const override { return model_->GetMetaData().sample_rate; }

  int32_t NumSpeakers() const override {
    return model_->GetMetaData().num_speakers;
  }

  GeneratedAudio Generate(const std::string &, const GenerationConfig &,
                          GeneratedAudioCallback = nullptr) const override {
    throw std::invalid_argument(
        "Kokoro requires PhonemeInput; use GenerateFromPhonemes");
  }

  [[deprecated("Use GenerateFromPhonemes instead")]]
  GeneratedAudio Generate(const std::string &, int64_t = 0, float = 1.0,
                          GeneratedAudioCallback = nullptr) const override {
    throw std::invalid_argument(
        "Kokoro requires PhonemeInput; use GenerateFromPhonemes");
  }

  GeneratedAudio GenerateFromPhonemes(
      const PhonemeInput &input, const GenerationConfig &gen_config,
      GeneratedAudioCallback callback = nullptr) const override {
    if (!IsUtf8(input.phonemes)) {
      throw std::invalid_argument("Kokoro phonemes must be valid UTF-8");
    }
    if (!std::isfinite(gen_config.speed) || gen_config.speed <= 0) {
      throw std::invalid_argument("Kokoro speed must be finite and positive");
    }
    if (gen_config.extra.count("lang")) {
      throw std::invalid_argument(
          "Kokoro accepts phonemes directly and does not use lang");
    }

    int32_t sid = gen_config.sid;
    int32_t num_speakers = model_->GetMetaData().num_speakers;
    if (sid < 0 || (num_speakers == 0 && sid != 0) ||
        (num_speakers > 0 && sid >= num_speakers)) {
      throw std::invalid_argument("Kokoro speaker ID is out of range");
    }

    float silence_scale = gen_config.silence_scale;
    if (silence_scale == 0.2f) {
      silence_scale = config_.silence_scale;
    }
    if (!std::isfinite(silence_scale) || silence_scale < 0.01f ||
        silence_scale > 10.0f) {
      throw std::invalid_argument("Kokoro silence scale is out of range");
    }

    GeneratedAudio result;
    result.sample_rate = SampleRate();
    result.span_alignments.emplace(input.spans.size());

    // The complete phoneme string is the source of truth for synthesis.
    std::vector<std::pair<size_t, size_t>> span_ranges;
    span_ranges.reserve(input.spans.size());
    size_t search_from = 0;
    for (size_t i = 0; i < input.spans.size(); ++i) {
      const std::string &phonemes = input.spans[i].phonemes;
      if (!IsUtf8(phonemes)) {
        throw std::invalid_argument("Kokoro span phonemes must be valid UTF-8");
      }
      size_t start = phonemes.empty()
                         ? search_from
                         : input.phonemes.find(phonemes, search_from);
      if (start == std::string::npos) {
        throw std::invalid_argument(
            "Kokoro span phonemes are not in aggregate phonemes in order");
      }
      span_ranges.emplace_back(start, start + phonemes.size());
      search_from = start + phonemes.size();
      (*result.span_alignments)[i].original_phonemes = phonemes;
    }

    std::vector<Token> tokens = Tokenize(input.phonemes, span_ranges);
    if (tokens.empty() ||
        std::all_of(tokens.begin(), tokens.end(),
                    [](const Token &token) { return token.is_space; })) {
      return result;
    }

    int32_t max_payload = model_->GetMetaData().max_token_len - 1;
    if (max_payload < 1) {
      throw std::runtime_error("Kokoro style table is too short");
    }

    // Prefer span boundaries, but split oversized spans at token boundaries.
    std::vector<std::pair<size_t, size_t>> chunks;
    for (size_t begin = 0; begin < tokens.size();) {
      size_t end = std::min(begin + static_cast<size_t>(max_payload),
                            tokens.size());
      if (end < tokens.size() && tokens[end - 1].owner >= 0 &&
          tokens[end - 1].owner == tokens[end].owner) {
        size_t boundary = end;
        while (boundary > begin &&
               tokens[boundary - 1].owner == tokens[end].owner) {
          --boundary;
        }
        if (boundary > begin) {
          end = boundary;
        }
      }
      chunks.emplace_back(begin, end);
      begin = end;
    }

    for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
      auto [begin, end] = chunks[chunk_index];
      std::vector<Token> chunk(tokens.begin() + begin, tokens.begin() + end);
      GeneratedAudio audio = Process(chunk, input.spans.size(), sid,
                                     gen_config.speed, silence_scale);
      float offset = static_cast<float>(result.samples.size()) /
                     result.sample_rate;
      for (size_t i = 0; i < input.spans.size(); ++i) {
        const SpanAlignment &part = (*audio.span_alignments)[i];
        SpanAlignment &whole = (*result.span_alignments)[i];
        whole.inferred_phonemes += part.inferred_phonemes;
        if (part.start_ts >= 0) {
          if (whole.start_ts < 0) {
            whole.start_ts = offset + part.start_ts;
          }
          whole.end_ts = offset + part.end_ts;
        }
      }
      result.samples.insert(result.samples.end(), audio.samples.begin(),
                            audio.samples.end());
      if (callback &&
          callback(audio.samples.data(), static_cast<int32_t>(audio.samples.size()),
                   static_cast<float>(chunk_index + 1) / chunks.size()) == 0) {
        break;
      }
    }

    return result;
  }

 private:
  struct Token {
    int64_t id;
    std::string canonical;
    int32_t owner;
    bool is_space;
  };

  void ValidateConfig() const {
    if (!config_.rule_fsts.empty() || !config_.rule_fars.empty()) {
      throw std::invalid_argument(
          "Kokoro does not accept text normalization rules");
    }
  }

  void InitTokens(std::istream &is) {
    token2id_ = ReadTokens(is, &id2token_);
    if (token2id_.empty()) {
      throw std::runtime_error("Kokoro tokens table is empty");
    }
  }

  std::vector<Token> Tokenize(
      const std::string &phonemes,
      const std::vector<std::pair<size_t, size_t>> &span_ranges) const {
    std::vector<Token> result;
    size_t span_index = 0;
    for (size_t pos = 0; pos < phonemes.size();) {
      unsigned char first = static_cast<unsigned char>(phonemes[pos]);
      size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
      std::string symbol = phonemes.substr(pos, width);
      while (span_index < span_ranges.size() &&
             span_ranges[span_index].second <= pos) {
        ++span_index;
      }
      int32_t owner = -1;
      if (span_index < span_ranges.size() &&
          span_ranges[span_index].first <= pos &&
          pos + width <= span_ranges[span_index].second) {
        owner = static_cast<int32_t>(span_index);
      }
      auto it = token2id_.find(symbol);
      if (it != token2id_.end()) {
        result.push_back(
            {it->second, id2token_.at(it->second), owner, symbol == " "});
      }
      pos += width;
    }
    return result;
  }

  GeneratedAudio Process(const std::vector<Token> &tokens, size_t num_spans,
                         int32_t sid, float speed,
                         float silence_scale) const {
    std::vector<int64_t> ids;
    ids.reserve(tokens.size() + 2);
    ids.push_back(0);  // BOS
    for (const auto &t : tokens) {
      ids.push_back(t.id);
    }
    ids.push_back(0);  // EOS

    auto memory_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
    std::array<int64_t, 2> shape = {1, static_cast<int64_t>(ids.size())};
    Ort::Value tensor = Ort::Value::CreateTensor(
        memory_info, ids.data(), ids.size(), shape.data(), shape.size());
    KokoroModelOutput output = model_->Run(std::move(tensor), sid, speed);

    auto audio_info = output.audio.GetTensorTypeAndShapeInfo();
    auto dur_info = output.pred_dur.GetTensorTypeAndShapeInfo();
    auto dur_shape = dur_info.GetShape();
    if (audio_info.GetElementCount() == 0 ||
        audio_info.GetElementType() !=
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        dur_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ||
        dur_info.GetElementCount() != ids.size() ||
        !(dur_shape.size() == 1 ||
          (dur_shape.size() == 2 && dur_shape[0] == 1))) {
      throw std::runtime_error(
          "Kokoro ONNX outputs have invalid audio or pred_dur types/shapes");
    }
    const int64_t *dur = output.pred_dur.GetTensorData<int64_t>();
    for (size_t i = 0; i < ids.size(); ++i) {
      if (dur[i] < 1) {
        throw std::runtime_error("Kokoro pred_dur must be positive");
      }
    }

    size_t num_samples = audio_info.GetElementCount();
    const float *samples = output.audio.GetTensorData<float>();
    GeneratedAudio audio;
    audio.sample_rate = SampleRate();
    audio.samples.assign(samples, samples + num_samples);
    audio.span_alignments.emplace(num_spans);

    // The reference pipeline counts half frames and trims three BOS frames.
    // A separator space gives half its duration to each adjacent span.
    int64_t half_frame = 2 * std::max<int64_t>(0, dur[0] - 3);
    std::vector<int64_t> token_starts(tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i) {
      token_starts[i] = half_frame;
      half_frame += 2 * dur[i + 1];
    }
    std::vector<int64_t> first(num_spans, -1), last(num_spans, -1);
    for (size_t i = 0; i < tokens.size(); ++i) {
      int32_t owner = tokens[i].owner;
      if (owner < 0) {
        continue;
      }
      if (first[owner] < 0) {
        first[owner] = static_cast<int64_t>(i);
      }
      last[owner] = static_cast<int64_t>(i);
      (*audio.span_alignments)[owner].inferred_phonemes += tokens[i].canonical;
    }

    auto to_seconds = [&](int64_t half_frames) -> float {
      // Kokoro generates 40 duration frames per second. Clamp to actual
      // audio length because the decoder can trim a small final tail.
      float samples_at_boundary =
          std::min(static_cast<float>(num_samples),
                   static_cast<float>(half_frames) * audio.sample_rate / 80);
      return samples_at_boundary / audio.sample_rate;
    };
    for (size_t i = 0; i < num_spans; ++i) {
      if (first[i] < 0) {
        continue;
      }
      size_t start = static_cast<size_t>(first[i]);
      size_t end = static_cast<size_t>(last[i]);
      int64_t start_half = token_starts[start];
      int64_t end_half = token_starts[end] + 2 * dur[end + 1];
      if (start > 0 && tokens[start - 1].is_space &&
          tokens[start - 1].owner != static_cast<int32_t>(i)) {
        start_half -= dur[start];
      }
      if (end + 1 < tokens.size() && tokens[end + 1].is_space &&
          tokens[end + 1].owner != static_cast<int32_t>(i)) {
        end_half += dur[end + 2];
      }
      auto &alignment = (*audio.span_alignments)[i];
      alignment.start_ts = to_seconds(std::max<int64_t>(0, start_half));
      alignment.end_ts = to_seconds(std::max(start_half, end_half));
    }

    if (silence_scale != 1) {
      audio = audio.ScaleSilence(silence_scale);
    }
    return audio;
  }

 private:
  OfflineTtsConfig config_;
  std::unique_ptr<OfflineTtsKokoroModel> model_;
  std::unordered_map<std::string, int32_t> token2id_;
  std::unordered_map<int32_t, std::string> id2token_;
};

}  // namespace sherpa_onnx
#endif  // SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_IMPL_H_
