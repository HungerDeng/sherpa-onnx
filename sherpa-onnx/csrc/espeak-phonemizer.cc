// sherpa-onnx/csrc/espeak-phonemizer.cc
//
// Copyright (c)  2026  Xiaomi Corporation

#include "sherpa-onnx/csrc/espeak-phonemizer.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "espeak-ng/speak_lib.h"
#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/text-utils.h"
#include "uni_algo.h"  // NOLINT

namespace sherpa_onnx {

std::mutex &GetEspeakPhonemizerMutex() {
  static std::mutex espeak_mutex;
  return espeak_mutex;
}

namespace {

std::vector<piper::Phoneme> NormalizeEspeakPhonemes(
    const char *text, const piper::eSpeakPhonemeConfig &config) {
  auto normalized = una::norm::to_nfd_utf8(text ? text : "");
  auto range = una::ranges::utf8_view{normalized};

  std::shared_ptr<piper::PhonemeMap> phoneme_map = config.phonemeMap;
  if (!phoneme_map && config.voice == "pt-br") {
    phoneme_map =
        std::make_shared<piper::PhonemeMap>(piper::PhonemeMap{{U'c', {U'k'}}});
  }

  std::vector<piper::Phoneme> mapped;
  for (auto phoneme : range) {
    if (!phoneme_map || phoneme_map->count(phoneme) == 0) {
      mapped.push_back(phoneme);
    } else {
      const auto &replacement = phoneme_map->at(phoneme);
      mapped.insert(mapped.end(), replacement.begin(), replacement.end());
    }
  }

  if (config.keepLanguageFlags) {
    return mapped;
  }

  std::vector<piper::Phoneme> ans;
  bool in_language_flag = false;
  for (auto phoneme : mapped) {
    if (in_language_flag) {
      if (phoneme == U')') {
        in_language_flag = false;
      }
    } else if (phoneme == U'(') {
      in_language_flag = true;
    } else {
      ans.push_back(phoneme);
    }
  }
  return ans;
}

}  // namespace

bool CallPhonemizeEspeakWithWordPhonemes(
    const std::string &text, piper::eSpeakPhonemeConfig &config,  // NOLINT
    std::vector<EspeakClausePhonemes> *clauses) {
  std::lock_guard<std::mutex> lock(GetEspeakPhonemizerMutex());
  clauses->clear();

  if (espeak_SetVoiceByName(config.voice.c_str()) != EE_OK) {
    SHERPA_ONNX_LOGE("Failed to set espeak-ng voice '%s'",
                     config.voice.c_str());
    return false;
  }

  std::string text_copy(text);
  const auto source_codepoints = Utf8ToUtf32(text);
  const void *text_pointer = text_copy.c_str();

  // espeak-ng owns clause position state globally. With several independent
  // calls (for example "This" and later "Next" on opposite sides of a
  // Chinese chunk), the first pair of the later call can retain the previous
  // message's offset. Anchor the first word to this call's source text, then
  // validate every later span and fall back to an ordered exact-text search.
  bool has_position_offset = false;
  int32_t position_offset = 0;
  size_t source_search_position = 0;
  while (text_pointer != nullptr) {
    int32_t terminator = 0;
    std::unique_ptr<espeak_word_phoneme_pairs,
                    decltype(&espeak_FreeWordPhonemePairs)>
        result(espeak_TextToWordPhonemePairsWithTerminator(
                   &text_pointer, espeakCHARS_AUTO, espeakPHONEMES_IPA,
                   &terminator),
               espeak_FreeWordPhonemePairs);
    if (!result) {
      clauses->clear();
      SHERPA_ONNX_LOGE("Failed to phonemize '%s' with espeak-ng voice '%s'",
                       text.c_str(), config.voice.c_str());
      return false;
    }

    EspeakClausePhonemes clause;
    clause.terminator = terminator;
    clause.words.reserve(result->size_pairs);
    for (int32_t i = 0; i != result->size_pairs; ++i) {
      const auto &pair = result->pairs[i];
      EspeakWordPhonemes word;
      word.text = pair.word ? pair.word : "";
      word.phonemes = NormalizeEspeakPhonemes(pair.phonemes, config);
      auto word_codepoints = Utf8ToUtf32(word.text);
      if (!has_position_offset && !word_codepoints.empty()) {
        auto position =
            source_codepoints.find(word_codepoints, source_search_position);
        if (position != std::u32string::npos) {
          position_offset = pair.word_position - static_cast<int32_t>(position);
          has_position_offset = true;
        }
      }

      int32_t position = pair.word_position - position_offset;
      bool position_is_exact =
          position >= 0 &&
          static_cast<size_t>(position) + word_codepoints.size() <=
              source_codepoints.size() &&
          source_codepoints.compare(static_cast<size_t>(position),
                                    word_codepoints.size(),
                                    word_codepoints) == 0;
      if (!position_is_exact) {
        auto found =
            source_codepoints.find(word_codepoints, source_search_position);
        if (found != std::u32string::npos) {
          position = static_cast<int32_t>(found);
        }
      }

      word.position = position;
      // pair.word is copied from the exact source span. Its length avoids the
      // API's documented 31-character packed-length limit.
      word.length = static_cast<int32_t>(word_codepoints.size());
      if (position >= 0) {
        source_search_position =
            static_cast<size_t>(position) + word_codepoints.size();
      }
      clause.words.push_back(std::move(word));
    }
    clauses->push_back(std::move(clause));
  }

  return true;
}

}  // namespace sherpa_onnx
