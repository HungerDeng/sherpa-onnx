// sherpa-onnx/csrc/offline-tts-kokoro-impl.h
//
// Copyright (c)  2025  Xiaomi Corporation
#ifndef SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_IMPL_H_
#define SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_IMPL_H_

#include <algorithm>
#include <array>
#include <iomanip>
#include <ios>
#include <memory>
#include <numeric>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include "fst/extensions/far/far.h"
#include "kaldifst/csrc/kaldi-fst-io.h"
#include "kaldifst/csrc/text-normalizer.h"
#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/fst-utils.h"
#include "sherpa-onnx/csrc/kokoro-multi-lang-lexicon.h"
#include "sherpa-onnx/csrc/lexicon.h"
#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/offline-tts-frontend.h"
#include "sherpa-onnx/csrc/offline-tts-impl.h"
#include "sherpa-onnx/csrc/offline-tts-kokoro-model.h"
#include "sherpa-onnx/csrc/piper-phonemize-lexicon.h"
#include "sherpa-onnx/csrc/text-utils.h"

namespace sherpa_onnx {

class OfflineTtsKokoroImpl : public OfflineTtsImpl {
 public:
  explicit OfflineTtsKokoroImpl(const OfflineTtsConfig &config)
      : config_(config),
        model_(std::make_unique<OfflineTtsKokoroModel>(config.model)) {
    InitFrontend();

    if (!config.rule_fsts.empty()) {
      std::vector<std::string> files;
      SplitStringToVector(config.rule_fsts, ",", false, &files);
      tn_list_.reserve(files.size());
      for (const auto &f : files) {
        if (config.model.debug) {
#if __OHOS__
          SHERPA_ONNX_LOGE("rule fst: %{public}s", f.c_str());
#else
          SHERPA_ONNX_LOGE("rule fst: %s", f.c_str());
#endif
        }
        tn_list_.push_back(std::make_unique<kaldifst::TextNormalizer>(f));
      }
    }

    if (!config.rule_fars.empty()) {
      if (config.model.debug) {
        SHERPA_ONNX_LOGE("Loading FST archives");
      }
      std::vector<std::string> files;
      SplitStringToVector(config.rule_fars, ",", false, &files);

      tn_list_.reserve(files.size() + tn_list_.size());

      for (const auto &f : files) {
        if (config.model.debug) {
#if __OHOS__
          SHERPA_ONNX_LOGE("rule far: %{public}s", f.c_str());
#else
          SHERPA_ONNX_LOGE("rule far: %s", f.c_str());
#endif
        }
        std::unique_ptr<fst::FarReader<fst::StdArc>> reader(
            fst::FarReader<fst::StdArc>::Open(f));
        for (; !reader->Done(); reader->Next()) {
          std::unique_ptr<fst::StdConstFst> r(
              fst::CastOrConvertToConstFst(reader->GetFst()->Copy()));

          tn_list_.push_back(
              std::make_unique<kaldifst::TextNormalizer>(std::move(r)));
        }
      }

      if (config.model.debug) {
        SHERPA_ONNX_LOGE("FST archives loaded!");
      }
    }
  }

  template <typename Manager>
  OfflineTtsKokoroImpl(Manager *mgr, const OfflineTtsConfig &config)
      : config_(config),
        model_(std::make_unique<OfflineTtsKokoroModel>(mgr, config.model)) {
    InitFrontend(mgr);

    if (!config.rule_fsts.empty()) {
      std::vector<std::string> files;
      SplitStringToVector(config.rule_fsts, ",", false, &files);
      tn_list_.reserve(files.size());
      for (const auto &f : files) {
        if (config.model.debug) {
#if __OHOS__
          SHERPA_ONNX_LOGE("rule fst: %{public}s", f.c_str());
#else
          SHERPA_ONNX_LOGE("rule fst: %s", f.c_str());
#endif
        }
        auto buf = ReadFile(mgr, f);
        std::istringstream is(std::string(buf.data(), buf.size()));
        tn_list_.push_back(std::make_unique<kaldifst::TextNormalizer>(is));
      }
    }

    if (!config.rule_fars.empty()) {
      std::vector<std::string> files;
      SplitStringToVector(config.rule_fars, ",", false, &files);
      tn_list_.reserve(files.size() + tn_list_.size());

      for (const auto &f : files) {
        if (config.model.debug) {
#if __OHOS__
          SHERPA_ONNX_LOGE("rule far: %{public}s", f.c_str());
#else
          SHERPA_ONNX_LOGE("rule far: %s", f.c_str());
#endif
        }

        auto buf = ReadFile(mgr, f);

        auto fsts = ReadFstsFromFar(buf);
        for (auto &r : fsts) {
          tn_list_.push_back(
              std::make_unique<kaldifst::TextNormalizer>(std::move(r)));
        }
      }    // for (const auto &f : files)
    }      // if (!config.rule_fars.empty())
  }

  int32_t SampleRate() const override {
    return model_->GetMetaData().sample_rate;
  }

  int32_t NumSpeakers() const override {
    return model_->GetMetaData().num_speakers;
  }

  // Supported options in GenerationConfig:
  //   - sid: Speaker ID for multi-speaker models
  //   - speed: Speech speed factor. If left at 1.0, it falls back to the
  //            default implied by kokoro.length_scale.
  //   - silence_scale: Scale applied to pauses in the generated audio. If left
  //                    at 0.2, it falls back to OfflineTtsConfig.silence_scale.
  //
  // Supported extra options in config.extra:
  //   - lang: Language override for Kokoro >= 1.0. Defaults to
  //           kokoro.lang if provided, otherwise meta_data.voice.
  GeneratedAudio Generate(
      const std::string &_text, const GenerationConfig &gen_config,
      GeneratedAudioCallback callback = nullptr) const override {
    if (config_.model.debug) {
      SHERPA_ONNX_LOGE("%s", gen_config.ToString().c_str());
    }

    int64_t sid = gen_config.sid;
    float speed = gen_config.speed;
    if (speed <= 0) {
      SHERPA_ONNX_LOGE("Speed must be > 0. Given: %f", speed);
      return {};
    }

    const auto &meta_data = model_->GetMetaData();
    int32_t num_speakers = meta_data.num_speakers;

    if (num_speakers == 0 && sid != 0) {
#if __OHOS__
      SHERPA_ONNX_LOGE(
          "This is a single-speaker model and supports only sid 0. Given sid: "
          "%{public}d. sid is ignored",
          static_cast<int32_t>(sid));
#else
      SHERPA_ONNX_LOGE(
          "This is a single-speaker model and supports only sid 0. Given sid: "
          "%d. sid is ignored",
          static_cast<int32_t>(sid));
#endif
    }

    if (num_speakers != 0 && (sid >= num_speakers || sid < 0)) {
#if __OHOS__
      SHERPA_ONNX_LOGE(
          "This model contains only %{public}d speakers. sid should be in the "
          "range [%{public}d, %{public}d]. Given: %{public}d. Use sid=0",
          num_speakers, 0, num_speakers - 1, static_cast<int32_t>(sid));
#else
      SHERPA_ONNX_LOGE(
          "This model contains only %d speakers. sid should be in the range "
          "[%d, %d]. Given: %d. Use sid=0",
          num_speakers, 0, num_speakers - 1, static_cast<int32_t>(sid));
#endif
      sid = 0;
    }

    std::string text = _text;
    if (config_.model.debug) {
#if __OHOS__
      SHERPA_ONNX_LOGE("Raw text: %{public}s", text.c_str());
#else
      SHERPA_ONNX_LOGE("Raw text: %s", text.c_str());
#endif
      std::ostringstream os;
      os << "In bytes (hex):\n";
      const auto p = reinterpret_cast<const uint8_t *>(text.c_str());
      for (int32_t i = 0; i != text.size(); ++i) {
        os << std::setw(2) << std::setfill('0') << std::hex
           << static_cast<uint32_t>(p[i]) << " ";
      }
      os << "\n";

#if __OHOS__
      SHERPA_ONNX_LOGE("%{public}s", os.str().c_str());
#else
      SHERPA_ONNX_LOGE("%s", os.str().c_str());
#endif
    }

    if (!tn_list_.empty()) {
      for (const auto &tn : tn_list_) {
        text = tn->Normalize(text);
        if (config_.model.debug) {
#if __OHOS__
          SHERPA_ONNX_LOGE("After normalizing: %{public}s", text.c_str());
#else
          SHERPA_ONNX_LOGE("After normalizing: %s", text.c_str());
#endif
        }
      }
    }

    std::string lang = gen_config.GetExtraString("lang");
    if (lang.empty()) {
      lang = config_.model.kokoro.lang.empty() ? meta_data.voice
                                               : config_.model.kokoro.lang;
    }

    std::vector<SplitSentence> split_sentences;
    std::vector<TokenIDs> token_ids;
    bool can_return_term_alignments = meta_data.version >= 2;
    if (can_return_term_alignments) {
      split_sentences =
          frontend_->ConvertTextToSplitSentences(text, lang);
      token_ids.reserve(split_sentences.size());
      for (const auto &sentence : split_sentences) {
        token_ids.push_back(sentence.token_ids);
        if (sentence.terms.empty()) {
          can_return_term_alignments = false;
        }
      }
    } else {
      token_ids = frontend_->ConvertTextToTokenIds(text, lang);
    }

    if (token_ids.empty() ||
        (token_ids.size() == 1 && token_ids[0].tokens.empty())) {
#if __OHOS__
      SHERPA_ONNX_LOGE("Failed to convert '%{public}s' to token IDs",
                       text.c_str());
#else
      SHERPA_ONNX_LOGE("Failed to convert '%s' to token IDs", text.c_str());
#endif
      return {};
    }

    std::vector<std::vector<int64_t>> sentence_tokens;

    sentence_tokens.reserve(token_ids.size());

    for (auto &i : token_ids) {
      sentence_tokens.push_back(std::move(i.tokens));
    }

    int32_t num_sentences = static_cast<int32_t>(sentence_tokens.size());

    if (config_.max_num_sentences != 1) {
#if __OHOS__
      SHERPA_ONNX_LOGE(
          "max_num_sentences (%{public}d) != 1 is ignored for Kokoro TTS "
          "models",
          config_.max_num_sentences);
#else
      SHERPA_ONNX_LOGE(
          "max_num_sentences (%d) != 1 is ignored for Kokoro TTS models",
          config_.max_num_sentences);
#endif
    }

    // Kokoro returns one pred_dur sequence for one ONNX token sequence. Process
    // each split sentence independently so its durations map unambiguously to
    // the terms in the SplitSentence at the same index.
    // So we don't need batch related logic, including batch_x, batch_size, num_batchs, etc.

    if (config_.model.debug) {
#if __OHOS__
      SHERPA_ONNX_LOGE(
          "Process %{public}d sentences with one Kokoro model call per "
          "sentence",
          num_sentences);
#else
      SHERPA_ONNX_LOGE(
          "Process %d sentences with one Kokoro model call per sentence",
          num_sentences);
#endif
    }

    GeneratedAudio ans;

    if (can_return_term_alignments) {
      ans.term_alignments.emplace();
    }

    int32_t should_continue = 1;

    float silence_scale = gen_config.silence_scale;
    if (silence_scale == 0.2f) {
      silence_scale = config_.silence_scale;
    }

    for (int32_t sentence_index = 0;
         sentence_index != num_sentences && should_continue;
         ++sentence_index) {
      auto result =
          Process(std::move(sentence_tokens[sentence_index]), sid, speed);
      auto audio = std::move(result.audio);

      if (ans.term_alignments) {
        // Infer sentence-local timestamps before ScaleSilence so it can map
        // the boundaries through the same removed or inserted silence.
        auto term_alignments = InferTermAlignments(
            split_sentences[sentence_index], result.pred_dur);
        audio.term_alignments = std::move(term_alignments);
      }

      if (silence_scale != 1) {
        audio = audio.ScaleSilence(silence_scale);
      }

      // ans already contains every preceding sentence. Its current duration
      // is therefore this sentence's offset in the returned waveform.
      float sentence_offset =
          ans.samples.size() / static_cast<float>(audio.sample_rate);
      if (ans.term_alignments && audio.term_alignments) {
        for (auto &alignment : *audio.term_alignments) {
          if (alignment.start_ts >= 0) {
            alignment.start_ts += sentence_offset;
          }
          if (alignment.end_ts >= 0) {
            alignment.end_ts += sentence_offset;
          }
          ans.term_alignments->push_back(std::move(alignment));
        }
      }

      // Append the adjusted sentence to the complete utterance. Keep audio's
      // samples intact because the callback below receives this sentence only.
      ans.sample_rate = audio.sample_rate;
      ans.samples.insert(ans.samples.end(), audio.samples.begin(),
                         audio.samples.end());

      if (callback) {
        should_continue = callback(audio.samples.data(), audio.samples.size(),
                                   (sentence_index + 1) * 1.0 / num_sentences);
        // Caution(fangjun): audio is freed when the callback returns, so users
        // should copy the data if they want to access the data after
        // the callback returns to avoid segmentation fault.
      }
    }

    return ans;
  }

  [[deprecated("Use Generate(text, GenerationConfig, callback) instead")]]
  GeneratedAudio Generate(
      const std::string &text, int64_t sid = 0, float speed = 1.0,
      GeneratedAudioCallback callback = nullptr) const override {
    GenerationConfig gen_config;
    gen_config.sid = sid;
    gen_config.speed = speed;
    gen_config.silence_scale = config_.silence_scale;
    if (!config_.model.kokoro.lang.empty()) {
      gen_config.extra["lang"] = config_.model.kokoro.lang;
    }

    return Generate(text, gen_config, std::move(callback));
  }

 private:
  template <typename Manager>
  void InitFrontend(Manager *mgr) {
    const auto &meta_data = model_->GetMetaData();

    if (meta_data.version >= 2) {
      // this is a multi-lingual model, we require that you pass lexicon
      if (config_.model.kokoro.lexicon.empty() &&
          config_.model.kokoro.lang.empty()) {
        SHERPA_ONNX_LOGE("Current model version: '%d'", meta_data.version);
        SHERPA_ONNX_LOGE(
            "You are using a multi-lingual Kokoro model (e.g., Kokoro >= "
            "v1.0). Please pass --kokoro-lexicon or provide --kokoro-lang");
        SHERPA_ONNX_EXIT(-1);
      }

      frontend_ = std::make_unique<KokoroMultiLangLexicon>(
          mgr, config_.model.kokoro.tokens, config_.model.kokoro.lexicon,
          config_.model.kokoro.data_dir, meta_data, config_.model.debug);

      return;
    }

    frontend_ = std::make_unique<PiperPhonemizeLexicon>(
        mgr, config_.model.kokoro.tokens, config_.model.kokoro.data_dir,
        meta_data);
  }

  void InitFrontend() {
    const auto &meta_data = model_->GetMetaData();
    if (meta_data.version >= 2) {
      // this is a multi-lingual model, we require that you pass lexicon
      if (config_.model.kokoro.lexicon.empty() &&
          config_.model.kokoro.lang.empty()) {
        SHERPA_ONNX_LOGE("Current model version: '%d'", meta_data.version);
        SHERPA_ONNX_LOGE(
            "You are using a multi-lingual Kokoro model (e.g., Kokoro >= "
            "v1.0). please pass --kokoro-lexicon or --kokoro-lang");
        SHERPA_ONNX_EXIT(-1);
      }

      frontend_ = std::make_unique<KokoroMultiLangLexicon>(
          config_.model.kokoro.tokens, config_.model.kokoro.lexicon,
          config_.model.kokoro.data_dir, meta_data, config_.model.debug);

      return;
    }

    // this is for kokoro v0.19, which supports only English
    frontend_ = std::make_unique<PiperPhonemizeLexicon>(
        config_.model.kokoro.tokens, config_.model.kokoro.data_dir, meta_data);
  }

  struct ProcessResult {
    GeneratedAudio audio;
    std::vector<int64_t> pred_dur;
  };

  static std::vector<TermAlignment> InferTermAlignments(
      const SplitSentence &sentence, const std::vector<int64_t> &pred_dur) {
    size_t num_term_tokens = 0;
    for (const auto &term : sentence.terms) {
      if (term.num_phoneme_tokens < 0 ||
          static_cast<size_t>(term.num_phoneme_tokens) >
              term.token_ids.tokens.size()) {
        SHERPA_ONNX_LOGE(
            "Invalid Kokoro term token span for '%s': %d phoneme tokens, "
            "%d total tokens",
            term.text.c_str(), term.num_phoneme_tokens,
            static_cast<int32_t>(term.token_ids.tokens.size()));
        SHERPA_ONNX_EXIT(-1);
      }
      num_term_tokens += term.token_ids.tokens.size();
    }

    if (sentence.token_ids.tokens.size() != num_term_tokens + 2 ||
        pred_dur.size() != sentence.token_ids.tokens.size()) {
      SHERPA_ONNX_LOGE(
          "Invalid Kokoro duration alignment: sentence has %d token IDs, "
          "terms cover %d token IDs, and pred_dur has %d entries",
          static_cast<int32_t>(sentence.token_ids.tokens.size()),
          static_cast<int32_t>(num_term_tokens),
          static_cast<int32_t>(pred_dur.size()));
      SHERPA_ONNX_EXIT(-1);
    }

    std::vector<TermAlignment> ans;
    ans.reserve(sentence.terms.size());

    // This is KPipeline::join_timestamps expressed in half-frames. Kokoro
    // emits 600 samples per duration frame at 24 kHz, so dividing half-frames
    // by 80 produces seconds. Tracking both edges lets a separator's duration
    // be split evenly between the terms on either side.
    constexpr float kMagicDivisor = 80.0f;
    int64_t left = 2 * std::max<int64_t>(0, pred_dur[0] - 3);
    int64_t right = left;
    size_t token_index = 1;  // Skip BOS.

    for (const auto &term : sentence.terms) {
      size_t phoneme_end =
          token_index + static_cast<size_t>(term.num_phoneme_tokens);
      size_t term_end = token_index + term.token_ids.tokens.size();
      int64_t phoneme_duration =
          std::accumulate(pred_dur.begin() + token_index,
                          pred_dur.begin() + phoneme_end, int64_t{0});
      int64_t suffix_duration =
          std::accumulate(pred_dur.begin() + phoneme_end,
                          pred_dur.begin() + term_end, int64_t{0});

      TermAlignment alignment{term.text, term.phoneme, -1.0f, -1.0f};
      if (term.num_phoneme_tokens != 0) {
        alignment.start_ts = left / kMagicDivisor;
        left = right + 2 * phoneme_duration + suffix_duration;
        alignment.end_ts = left / kMagicDivisor;
        right = left + suffix_duration;
      } else {
        // Match KPipeline's handling of an unpronounced token: leave its
        // timestamps unset but consume its model-only separator duration.
        left = right + suffix_duration;
        right = left + suffix_duration;
      }

      ans.push_back(std::move(alignment));
      token_index = term_end;
    }

    if (token_index + 1 != pred_dur.size()) {
      SHERPA_ONNX_LOGE(
          "Invalid Kokoro duration alignment: consumed %d of %d token "
          "durations before EOS",
          static_cast<int32_t>(token_index),
          static_cast<int32_t>(pred_dur.size() - 1));
      SHERPA_ONNX_EXIT(-1);
    }

    return ans;
  }

  ProcessResult Process(std::vector<int64_t> model_tokens, int32_t sid,
                        float speed) const {
    auto memory_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    std::array<int64_t, 2> model_input_shape = {
        1, static_cast<int32_t>(model_tokens.size())};
    Ort::Value model_input = Ort::Value::CreateTensor(
        memory_info, model_tokens.data(), model_tokens.size(),
        model_input_shape.data(), model_input_shape.size());

    auto model_output = model_->Run(std::move(model_input), sid, speed);
    auto &audio = model_output.audio;

    std::vector<int64_t> audio_shape =
        audio.GetTensorTypeAndShapeInfo().GetShape();

    int64_t total = 1;
    // The output shape may be (1, 1, total) or (1, total) or (total,)
    for (auto i : audio_shape) {
      total *= i;
    }

    const float *p = audio.GetTensorData<float>();

    ProcessResult ans;
    ans.audio.sample_rate = model_->GetMetaData().sample_rate;
    ans.audio.samples = std::vector<float>(p, p + total);

    if (model_->GetMetaData().version >= 2) {
      auto pred_dur_info = model_output.pred_dur.GetTensorTypeAndShapeInfo();
      size_t num_durations = pred_dur_info.GetElementCount();
      const int64_t *durations = model_output.pred_dur.GetTensorData<int64_t>();
      ans.pred_dur.assign(durations, durations + num_durations);
    }

    return ans;
  }

 private:
  OfflineTtsConfig config_;
  std::unique_ptr<OfflineTtsKokoroModel> model_;
  std::vector<std::unique_ptr<kaldifst::TextNormalizer>> tn_list_;
  std::unique_ptr<OfflineTtsFrontend> frontend_;
};

}  // namespace sherpa_onnx
#endif  // SHERPA_ONNX_CSRC_OFFLINE_TTS_KOKORO_IMPL_H_
