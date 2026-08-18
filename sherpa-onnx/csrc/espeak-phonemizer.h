// sherpa-onnx/csrc/espeak-phonemizer.h
//
// Copyright (c)  2026  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_ESPEAK_PHONEMIZER_H_
#define SHERPA_ONNX_CSRC_ESPEAK_PHONEMIZER_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "phonemize.hpp"  // NOLINT

namespace sherpa_onnx {

struct EspeakWordPhonemes {
  std::string text;
  std::vector<piper::Phoneme> phonemes;
  // Character span normalized to the text passed to this wrapper call.
  int32_t position = 0;
  int32_t length = 0;
};

struct EspeakClausePhonemes {
  std::vector<EspeakWordPhonemes> words;
  int32_t terminator = 0;
};

// All espeak-ng entry points use this mutex because the selected voice and
// translation state are process-global.
std::mutex &GetEspeakPhonemizerMutex();

void CallPhonemizeEspeak(const std::string &text,
                         piper::eSpeakPhonemeConfig &config,  // NOLINT
                         std::vector<std::vector<piper::Phoneme>> *phonemes);

// Kokoro multi-lang variant of piper::phonemize_eSpeak(). It retains the
// contextual word pairs returned by
// espeak_TextToWordPhonemePairsWithTerminator(). Punctuation is represented by
// clause.terminator rather than as a word.
bool CallPhonemizeEspeakWithWordPhonemes(
    const std::string &text, piper::eSpeakPhonemeConfig &config,  // NOLINT
    std::vector<EspeakClausePhonemes> *clauses);

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_ESPEAK_PHONEMIZER_H_
