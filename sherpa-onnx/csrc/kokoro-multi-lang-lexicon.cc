// sherpa-onnx/csrc/kokoro-multi-lang-lexicon.cc
//
// Copyright (c)  2025  Xiaomi Corporation

#include "sherpa-onnx/csrc/kokoro-multi-lang-lexicon.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sherpa-onnx/csrc/macros.h"

#if __ANDROID_API__ >= 9
#include "android/asset_manager.h"
#include "android/asset_manager_jni.h"
#endif

#if __OHOS__
#include "rawfile/raw_file_manager.h"
#endif

#include "espeak-ng/speak_lib.h"
#include "phoneme_ids.hpp"  // NOLINT
#include "phonemize.hpp"    // NOLINT
#include "sherpa-onnx/csrc/espeak-phonemizer.h"
#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/onnx-utils.h"
#include "sherpa-onnx/csrc/phrase-matcher.h"
#include "sherpa-onnx/csrc/symbol-table.h"
#include "sherpa-onnx/csrc/text-utils.h"

namespace sherpa_onnx {

class KokoroMultiLangLexicon::Impl {
 private:
  enum class LanguageType {
    kChinese,
    kNonChinese,
  };

  struct TextChunk {
    std::string text;
    LanguageType language = LanguageType::kNonChinese;
  };

  // G2P implementations populate only text and phonemes. model_suffix holds
  // phonemes that affect model input but are not part of the term alignment,
  // such as the separator between two espeak words or the pause after a
  // clause comma.
  struct G2pTerm {
    std::string text;
    std::string phoneme;
    std::string model_suffix;

    // True when this term was reconstructed from a gap between espeak's word
    // spans rather than returned by espeak as a word or clause terminator.
    //
    // For example, espeak may return only "record" and "doesn't" for the
    // source "record. doesn't". We reconstruct the missing period as
    // {text: ".", phoneme: "."} and intentionally tokenize it for Kokoro.
    // The flag records its origin so ApplyEspeakTerminator() can distinguish
    // such recovered punctuation from punctuation that espeak did emit.
    bool is_omitted_by_espeak = false;
  };

  // Preserve frontend sentence boundaries until the shared packing stage.
  // A language implementation never creates TokenIDs or SplitSentence.
  struct G2pSentence {
    std::vector<G2pTerm> terms;
  };

 public:
  Impl(const std::string &tokens, const std::string &lexicon,
       const std::string &data_dir,
       const OfflineTtsKokoroModelMetaData &meta_data, bool debug)
      : meta_data_(meta_data), debug_(debug) {
    InitTokens(tokens);

    InitLexicon(lexicon);

    InitEspeak(data_dir);  // See ./piper-phonemize-lexicon.cc
  }

  template <typename Manager>
  Impl(Manager *mgr, const std::string &tokens, const std::string &lexicon,
       const std::string &data_dir,
       const OfflineTtsKokoroModelMetaData &meta_data, bool debug)
      : meta_data_(meta_data), debug_(debug) {
    InitTokens(mgr, tokens);

    InitLexicon(mgr, lexicon);

    // we assume you have copied data_dir from assets to some path

    InitEspeak(data_dir);  // See ./piper-phonemize-lexicon.cc
  }

  std::vector<SplitSentence> ConvertTextToSplitSentences(
      const std::string &_text, const std::string &voice) const {
    auto text_chunks = SplitTextIntoLanguageChunks(_text);

    // HasUnsegmentedScript() checks only the voice prefix and Unicode script
    // ranges. SplitTextIntoLanguageChunks() changes punctuation and
    // whitespace but never those script characters, so _text and the joined
    // normalized chunks produce the same result here.
    bool supports_term_alignments = !HasUnsegmentedScript(_text, voice);

    std::vector<G2pSentence> g2p_sentences;
    for (const auto &chunk : text_chunks) {
      std::vector<G2pSentence> sentences;
      if (chunk.language == LanguageType::kChinese) {
        sentences = G2pChinese(chunk.text);
      } else {
        sentences = G2pNonChinese(chunk.text, voice);
      }
      g2p_sentences.insert(g2p_sentences.end(),
                           std::make_move_iterator(sentences.begin()),
                           std::make_move_iterator(sentences.end()));
    }

    // Language-specific work ends above. Tokenization, token-budget packing,
    // and merging short sentences are shared by every current and future G2P.
    auto ans = TokenizePackAndMerge(std::move(g2p_sentences));

    if (!supports_term_alignments) {
      for (auto &sentence : ans) {
        sentence.terms.clear();
      }
    }

    if (debug_) {
      for (const auto &v : ans) {
        std::ostringstream os;
        os << "\n";
        std::string sep;
        for (auto i : v.token_ids.tokens) {
          os << sep << i;
          sep = " ";
        }
        os << "\n";
        SHERPA_ONNX_LOGE("%s", os.str().c_str());
      }
    }

    return ans;
  }

  std::vector<TokenIDs> ConvertTextToTokenIds(const std::string &text,
                                              const std::string &voice) const {
    auto sentences = ConvertTextToSplitSentences(text, voice);
    std::vector<TokenIDs> ans;
    ans.reserve(sentences.size());
    for (auto &sentence : sentences) {
      ans.push_back(std::move(sentence.token_ids));
    }
    return ans;
  }

 private:
  std::vector<TextChunk> SplitTextIntoLanguageChunks(
      const std::string &text) const {
    struct NormalizedChunk {
      std::string text;
      bool is_full_width_punctuation = false;
    };

    // This function splits text in two stages.
    //
    // Stage 1 preserves every original full-width punctuation mark as one
    // independent chunk and maps it to the corresponding ASCII model token.
    // For example:
    //
    //   "欢迎你。This"  -> ["欢迎你", "." (punctuation), "This"]
    //   "欢迎你。中国"  -> ["欢迎你", "." (punctuation), "中国"]
    //   "欢迎你。 This" -> ["欢迎你", "." (punctuation), " This"]
    //
    // Without this first stage, global replacement would produce
    // "欢迎你.This". The language regex would then group ".This" into one
    // non-Chinese run, and espeak would pronounce the leading period as the
    // English word "dot". Keeping the punctuation chunk separate also means
    // existing or repeated spaces cannot change how punctuation is routed.
    //
    // Stage 2 splits each remaining ordinary chunk into Chinese and
    // non-Chinese runs. Thus:
    //
    //   "Hello，中国。This"
    //     -> ["Hello" (non-Chinese), "," (non-Chinese),
    //         "中国" (Chinese), "." (non-Chinese), "This" (non-Chinese)]
    std::vector<NormalizedChunk> normalized_chunks;
    std::string current;
    auto flush_current = [&]() {
      if (!current.empty()) {
        normalized_chunks.push_back({std::move(current), false});
        current.clear();
      }
    };

    for (char32_t c : Utf8ToUtf32(text)) {
      const char *punctuation = nullptr;
      switch (c) {
        case U'，':
        case U'、':
          punctuation = ",";
          break;
        case U'；':
          punctuation = ";";
          break;
        case U'：':
          punctuation = ":";
          break;
        case U'。':
          punctuation = ".";
          break;
        case U'？':
          punctuation = "?";
          break;
        case U'！':
          punctuation = "!";
          break;
        default:
          break;
      }

      if (punctuation) {
        flush_current();
        normalized_chunks.push_back({punctuation, true});
      } else {
        current += Utf32ToUtf8(c);
      }
    }
    flush_current();

    // Retain the existing normalization for ordinary text. Full-width
    // punctuation has already been mapped above so its origin is not lost.
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {":", ","},
        {"\\s+", " "},
    };
    for (auto &chunk : normalized_chunks) {
      if (chunk.is_full_width_punctuation) {
        continue;
      }
      for (const auto &replacement : replacements) {
        std::regex re(replacement.first);
        chunk.text = std::regex_replace(chunk.text, re, replacement.second);
      }
    }

    // Partition ordinary normalized text into alternating Chinese
    // ([一-龥]+) and non-Chinese ([^一-龥]+) runs. Full-width punctuation is
    // already an independent non-Chinese chunk.
    const std::string expr_chinese = "([\\u4e00-\\u9fff]+)";
    const std::string expr_not_chinese = "([^\\u4e00-\\u9fff]+)";
    const std::wstring expr_both =
        ToWideString(expr_chinese + "|" + expr_not_chinese);
    const std::wregex re_both(expr_both);
    const std::wregex re_chinese(ToWideString(expr_chinese));

    std::vector<TextChunk> text_chunks;
    for (const auto &chunk : normalized_chunks) {
      if (chunk.is_full_width_punctuation) {
        text_chunks.push_back({chunk.text, LanguageType::kNonChinese});
        continue;
      }

      auto ws = ToWideString(chunk.text);
      auto begin = std::wsregex_iterator(ws.begin(), ws.end(), re_both);
      auto end = std::wsregex_iterator();
      for (auto i = begin; i != end; ++i) {
        std::wstring match = i->str();
        LanguageType language = std::regex_match(match, re_chinese)
                                    ? LanguageType::kChinese
                                    : LanguageType::kNonChinese;
        text_chunks.push_back({ToString(match), language});
      }
    }

    if (debug_) {
      std::string normalized_text;
      for (const auto &chunk : text_chunks) {
        normalized_text += chunk.text;
      }
      SHERPA_ONNX_LOGE("After replacing punctuations and merging spaces:\n%s",
                       normalized_text.c_str());
      for (const auto &chunk : text_chunks) {
        SHERPA_ONNX_LOGE("%s: %s",
                         chunk.language == LanguageType::kChinese
                             ? "Chinese"
                             : "Non-Chinese",
                         chunk.text.c_str());
      }
    }

    return text_chunks;
  }

  bool HasUnsegmentedScript(const std::string &text,
                            const std::string &voice = "") const {
    if (voice.rfind("ja", 0) == 0 || voice.rfind("th", 0) == 0 ||
        voice.rfind("lo", 0) == 0 || voice.rfind("my", 0) == 0 ||
        voice.rfind("km", 0) == 0) {
      return true;
    }
    for (char32_t c : Utf8ToUtf32(text)) {
      // Hiragana, Katakana, Thai, Lao, Myanmar, and Khmer need a word
      // segmenter that this frontend does not provide. Audio generation still
      // uses the normal frontend output, but term alignments are suppressed.
      if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x0e00 && c <= 0x0eff) ||
          (c >= 0x1000 && c <= 0x109f) || (c >= 0x1780 && c <= 0x17ff)) {
        return true;
      }
    }
    return false;
  }

  std::string IdsToPhoneme(const std::vector<int64_t> &ids) const {
    std::string ans;
    int32_t space_id = token2id_.at(" ");
    size_t begin = 0;
    size_t end = ids.size();
    while (begin < end && ids[begin] == space_id) {
      ++begin;
    }
    while (end > 0 && ids[end - 1] == space_id) {
      --end;
    }
    for (size_t i = begin; i != end; ++i) {
      auto iter = id2token_.find(static_cast<int32_t>(ids[i]));
      if (iter != id2token_.end()) {
        ans += iter->second;
      }
    }
    return ans;
  }

  SplitSentence MakeSentence(std::vector<Term> terms) const {
    SplitSentence ans;
    ans.token_ids.tokens.push_back(0);
    for (const auto &term : terms) {
      ans.token_ids.tokens.insert(ans.token_ids.tokens.end(),
                                  term.token_ids.tokens.begin(),
                                  term.token_ids.tokens.end());
    }
    ans.token_ids.tokens.push_back(0);
    ans.terms = std::move(terms);
    return ans;
  }

  std::vector<SplitSentence> PackTerms(std::vector<Term> terms) const {
    std::vector<SplitSentence> ans;
    std::vector<Term> current;
    size_t current_size = 0;
    const size_t max_content =
        static_cast<size_t>(std::max(1, meta_data_.max_token_len - 1));

    auto flush = [&]() {
      if (!current.empty()) {
        ans.push_back(MakeSentence(std::move(current)));
        current.clear();
        current_size = 0;
      }
    };

    for (auto &term : terms) {
      auto &ids = term.token_ids.tokens;
      if (ids.size() <= max_content) {
        if (current_size + ids.size() > max_content) {
          flush();
        }
        current_size += ids.size();
        current.push_back(std::move(term));
        continue;
      }

      flush();
      size_t begin = 0;
      while (begin < ids.size()) {
        size_t end = std::min(begin + max_content, ids.size());
        Term fragment;
        fragment.text = term.text;
        fragment.token_ids.tokens.assign(ids.begin() + begin,
                                         ids.begin() + end);
        fragment.phoneme = IdsToPhoneme(fragment.token_ids.tokens);
        ans.push_back(MakeSentence({std::move(fragment)}));
        begin = end;
      }
    }
    flush();
    return ans;
  }

  std::vector<int64_t> TokenizePhonemes(const std::string &phonemes,
                                        const std::string &text) const {
    std::vector<int64_t> ans;
    for (char32_t phoneme : Utf8ToUtf32(phonemes)) {
      auto iter = phoneme2id_.find(phoneme);
      if (iter == phoneme2id_.end()) {
        SHERPA_ONNX_LOGE(
            "Skip unknown phoneme from '%s'. Unicode codepoint: \\U+%04x.",
            text.c_str(), static_cast<uint32_t>(phoneme));
        continue;
      }
      ans.push_back(iter->second);
    }
    return ans;
  }

  Term TokenizeTerm(G2pTerm g2p_term) const {
    Term term;
    term.text = std::move(g2p_term.text);

    auto phoneme_ids = TokenizePhonemes(g2p_term.phoneme, term.text);
    term.phoneme = IdsToPhoneme(phoneme_ids);

    // Tokenize recovered punctuation too. Written input such as
    // "record. doesn't" therefore reaches Kokoro as "record . doesn't"
    // instead of silently dropping the user's period just because espeak did
    // not classify it as a clause terminator.
    term.token_ids.tokens = std::move(phoneme_ids);

    auto suffix_ids = TokenizePhonemes(g2p_term.model_suffix, term.text);
    term.token_ids.tokens.insert(term.token_ids.tokens.end(),
                                 suffix_ids.begin(), suffix_ids.end());
    return term;
  }

  std::vector<SplitSentence> TokenizePackAndMerge(
      std::vector<G2pSentence> g2p_sentences) const {
    std::vector<SplitSentence> ans;

    for (auto &g2p_sentence : g2p_sentences) {
      std::vector<Term> terms;
      terms.reserve(g2p_sentence.terms.size());
      for (auto &g2p_term : g2p_sentence.terms) {
        terms.push_back(TokenizeTerm(std::move(g2p_term)));
      }

      auto packed = PackTerms(std::move(terms));
      for (auto &sentence : packed) {
        const auto &ids = sentence.token_ids.tokens;

        // Keep substantial packed units independent. Short units, including
        // punctuation-only chunks, are joined when the model token budget
        // permits it. Both BOS/EOS wrappers are already present here.
        if (ids.size() > 10 + 2 || ans.empty()) {
          ans.push_back(std::move(sentence));
          continue;
        }

        size_t merged_size =
            ans.back().token_ids.tokens.size() + ids.size() - 2;
        bool fits_model =
            merged_size <= static_cast<size_t>(meta_data_.max_token_len + 1);
        if (fits_model &&
            ((ans.back().token_ids.tokens.size() + ids.size() < 50) ||
             (ids.size() < 5))) {
          auto &last_ids = ans.back().token_ids.tokens;
          last_ids.back() = ids[1];
          last_ids.insert(last_ids.end(), ids.begin() + 2, ids.end());
          ans.back().terms.insert(
              ans.back().terms.end(),
              std::make_move_iterator(sentence.terms.begin()),
              std::make_move_iterator(sentence.terms.end()));
        } else {
          ans.push_back(std::move(sentence));
        }
      }
    }

    return ans;
  }

  bool IsPunctuation(const std::string &text) const {
    if (text == ";" || text == ":" || text == "," || text == "." ||
        text == "!" || text == "?" || text == "—" || text == "…" ||
        text == "\"" || text == "(" || text == ")" || text == "“" ||
        text == "”") {
      return true;
    }

    return false;
  }

  std::vector<std::string> SplitWrittenTerms(const std::string &text) const {
    std::vector<std::string> ans;
    std::string current;
    auto flush = [&]() {
      if (!current.empty()) {
        ans.push_back(std::move(current));
        current.clear();
      }
    };

    auto codepoints = Utf8ToUtf32(text);
    auto is_space = [](char32_t c) {
      return c <= 0x7f && std::isspace(static_cast<unsigned char>(c));
    };
    auto is_punctuation = [this](char32_t c) {
      std::string s = Utf32ToUtf8(c);
      bool is_ascii = c <= 0x7f && std::ispunct(static_cast<unsigned char>(c));
      bool is_unicode =
          (c >= 0x0600 && c <= 0x061f) || (c >= 0x066a && c <= 0x066d) ||
          (c >= 0x2000 && c <= 0x206f) || (c >= 0x2e00 && c <= 0x2e7f) ||
          (c >= 0x3000 && c <= 0x303f) || (c >= 0xff00 && c <= 0xff65);
      return IsPunctuation(s) || is_ascii || is_unicode;
    };

    for (size_t i = 0; i != codepoints.size(); ++i) {
      char32_t c = codepoints[i];
      std::string s = Utf32ToUtf8(c);
      bool punctuation = is_punctuation(c);
      if ((c == U'\'' || c == 0x2019) && !current.empty() &&
          i + 1 < codepoints.size() && !is_space(codepoints[i + 1]) &&
          !is_punctuation(codepoints[i + 1])) {
        punctuation = false;
      }

      if (is_space(c)) {
        flush();
      } else if (punctuation) {
        flush();
        ans.push_back(std::move(s));
      } else {
        current += s;
      }
    }
    flush();
    return ans;
  }

  std::vector<int32_t> ConvertWordToIds(const std::string &w) const {
    std::vector<int32_t> ans;
    if (word2ids_.count(w)) {
      ans = word2ids_.at(w);
    } else {
      std::vector<std::string> words = SplitUtf8(w);
      for (const auto &word : words) {
        if (word2ids_.count(word)) {
          auto ids = ConvertWordToIds(word);
          ans.insert(ans.end(), ids.begin(), ids.end());
        } else {
          if (debug_) {
            SHERPA_ONNX_LOGE("Skip OOV: '%s'", word.c_str());
          }
        }
      }
    }

    if (debug_ && !ans.empty()) {
      std::ostringstream os;
      os << w << ": ";
      for (auto i : ans) {
        os << id2token_.at(i) << " ";
      }
      os << "\n";
#if __OHOS__
      SHERPA_ONNX_LOGE("%{public}s", os.str().c_str());
#else
      SHERPA_ONNX_LOGE("%s", os.str().c_str());
#endif
    }

    return ans;
  }

  std::string PhonemesToString(
      const std::vector<piper::Phoneme> &phonemes) const {
    std::string ans;
    for (auto phoneme : phonemes) {
      ans += Utf32ToUtf8(phoneme);
    }
    return ans;
  }

  std::vector<G2pSentence> G2pChinese(const std::string &text) const {
    // Split the run into one UTF-8 character per element, e.g.
    // 中國人民不信邪 → [中, 國, 人, 民, 不, 信, 邪]. PhraseMatcher then groups
    // adjacent characters into the longest lexicon phrases (max 10 chars),
    // falling back to single characters for unmatched ones.
    std::vector<std::string> words = SplitUtf8(text);

    if (debug_) {
      std::ostringstream os;
      std::string sep;
      for (const auto &word : words) {
        os << sep << word;
        sep = "_";
      }
#if __OHOS__
      SHERPA_ONNX_LOGE("after splitting into UTF8:\n%{public}s",
                       os.str().c_str());
#else
      SHERPA_ONNX_LOGE("after splitting into UTF8:\n%s", os.str().c_str());
#endif
    }

    G2pSentence sentence;
    PhraseMatcher matcher(&all_words_, words, debug_);
    for (const std::string &word : matcher) {
      auto ids = ConvertWordToIds(word);
      if (ids.empty()) {
#if __OHOS__
        SHERPA_ONNX_LOGE("Ignore OOV '%{public}s'", word.c_str());
#else
        SHERPA_ONNX_LOGE("Ignore OOV '%s'", word.c_str());
#endif
        continue;
      }

      G2pTerm term;
      term.text = word;
      // The lexicon is stored as token IDs, so convert its pronunciation back
      // to phonemes here. The shared stage tokenizes it after all languages
      // have finished G2P.
      term.phoneme = IdsToPhoneme(std::vector<int64_t>(ids.begin(), ids.end()));
      sentence.terms.push_back(std::move(term));
    }

    if (sentence.terms.empty()) {
      return {};
    }
    std::vector<G2pSentence> ans;
    ans.push_back(std::move(sentence));
    return ans;
  }

  void AppendSourceGap(const std::u32string &text, size_t begin, size_t end,
                       G2pSentence *sentence) const {
    // espeak's word-pair API reports spans only for spoken words. Preserve
    // every non-whitespace character between those spans as a term, and mark
    // that the term was recovered rather than emitted by espeak. Its written
    // character is also its phoneme so the shared tokenization stage sends
    // punctuation such as '.', ',' and ';' to Kokoro.
    end = std::min(end, text.size());
    for (size_t i = std::min(begin, end); i != end; ++i) {
      char32_t c = text[i];
      if (c <= 0x7f && std::isspace(static_cast<unsigned char>(c))) {
        continue;
      }
      std::string punctuation = Utf32ToUtf8(c);
      sentence->terms.push_back({punctuation, punctuation, "", true});
    }
  }

  bool ApplyEspeakTerminator(int32_t terminator,
                             const piper::eSpeakPhonemeConfig &config,
                             size_t gap_term_begin,
                             G2pSentence *sentence) const {
    int32_t punctuation = terminator & 0x000fffff;
    piper::Phoneme phoneme = 0;
    bool append_space = false;
    if (punctuation == CLAUSE_PERIOD) {
      phoneme = config.period;
      // Preserve Kokoro's existing model input: ConvertPhonemes() appended a
      // space after an espeak period, although that space is not displayed in
      // the public term alignment.
      append_space = phoneme == U'.';
    } else if (punctuation == CLAUSE_QUESTION) {
      phoneme = config.question;
    } else if (punctuation == CLAUSE_EXCLAMATION) {
      phoneme = config.exclamation;
    } else if (punctuation == CLAUSE_COMMA) {
      phoneme = config.comma;
      append_space = true;
    } else if (punctuation == CLAUSE_COLON) {
      phoneme = config.colon;
      append_space = true;
    } else if (punctuation == CLAUSE_SEMICOLON) {
      phoneme = config.semicolon;
      append_space = true;
    }

    if (phoneme != 0) {
      std::string punctuation_text = Utf32ToUtf8(phoneme);
      size_t target = sentence->terms.size();
      for (size_t i = gap_term_begin; i != sentence->terms.size(); ++i) {
        if (sentence->terms[i].is_omitted_by_espeak &&
            sentence->terms[i].text == punctuation_text) {
          target = i;
          break;
        }
      }
      if (target == sentence->terms.size()) {
        for (size_t i = gap_term_begin; i != sentence->terms.size(); ++i) {
          if (sentence->terms[i].is_omitted_by_espeak &&
              IsPunctuation(sentence->terms[i].text)) {
            target = i;
            break;
          }
        }
      }
      if (target == sentence->terms.size()) {
        sentence->terms.push_back({punctuation_text, "", ""});
        target = sentence->terms.size() - 1;
      }

      auto &term = sentence->terms[target];
      term.phoneme = punctuation_text;
      // This source-gap term corresponds to the terminator espeak did emit,
      // so it is no longer classified as omitted. Other punctuation in the
      // same gap remains marked as recovered and is still tokenized normally.
      term.is_omitted_by_espeak = false;
      if (append_space) {
        term.model_suffix = Utf32ToUtf8(config.space);
      }
    }

    return (terminator & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE;
  }

  std::vector<G2pSentence> G2pNonChineseWithEspeak(
      const std::string &text, const std::string &voice) const {
    piper::eSpeakPhonemeConfig config;
    config.voice = voice;

    std::vector<EspeakClausePhonemes> clauses;
    if (!CallPhonemizeEspeakWithWordPhonemes(text, config, &clauses)) {
      return {};
    }

    std::vector<G2pSentence> ans;
    G2pSentence sentence;
    const auto source = Utf8ToUtf32(text);
    size_t source_cursor = 0;
    auto flush = [&]() {
      if (!sentence.terms.empty()) {
        ans.push_back(std::move(sentence));
        sentence = {};
      }
    };

    for (size_t clause_index = 0; clause_index != clauses.size();
         ++clause_index) {
      const auto &clause = clauses[clause_index];
      for (size_t i = 0; i != clause.words.size(); ++i) {
        const auto &word = clause.words[i];
        size_t word_begin = static_cast<size_t>(std::max(0, word.position));
        size_t word_end =
            word_begin + static_cast<size_t>(std::max(0, word.length));

        // eSpeak returns spans only for spoken words. Recover non-whitespace
        // source characters before this word so alignment terms do not lose
        // punctuation that eSpeak kept inside the clause. For example, in
        // "stop., and", the gap between "stop" and "and" contains both '.'
        // and ',', even when neither appears in the returned word pairs.
        AppendSourceGap(source, source_cursor, word_begin, &sentence);

        G2pTerm term;
        term.text = word.text;
        term.phoneme = PhonemesToString(word.phonemes);
        if (i + 1 != clause.words.size()) {
          // espeak's flat clause output contains one space between adjacent
          // word pairs. Keep it in model input without exposing it as part of
          // either term's public phoneme string.
          term.model_suffix = Utf32ToUtf8(config.space);
        }
        sentence.terms.push_back(std::move(term));
        source_cursor = std::min(word_end, source.size());
      }

      size_t clause_end = source.size();
      for (size_t i = clause_index + 1; i != clauses.size(); ++i) {
        if (!clauses[i].words.empty()) {
          clause_end = static_cast<size_t>(
              std::max(0, clauses[i].words.front().position));
          break;
        }
      }
      size_t gap_term_begin = sentence.terms.size();

      // Recover the source tail after this clause's last returned word. This
      // is also required when eSpeak combines written sentences into one
      // clause, e.g. "record. doesn't": the period lies between word spans
      // and may not be reported as clause.terminator. ApplyEspeakTerminator()
      // below then identifies any punctuation that eSpeak did emit into its
      // phoneme stream. Other recovered punctuation remains marked as omitted
      // by espeak, but is still tokenized and sent to Kokoro.
      AppendSourceGap(source, source_cursor, clause_end, &sentence);
      source_cursor = std::min(clause_end, source.size());

      bool is_sentence = ApplyEspeakTerminator(clause.terminator, config,
                                               gap_term_begin, &sentence);
      if (is_sentence) {
        flush();
      }
    }
    flush();

    if (debug_) {
      for (const auto &group : ans) {
        for (const auto &term : group.terms) {
          SHERPA_ONNX_LOGE("G2P term '%s': %s", term.text.c_str(),
                           term.phoneme.c_str());
        }
      }
    }
    return ans;
  }

  std::vector<G2pSentence> G2pNonChineseWithoutVoice(
      const std::string &text) const {
    // Preserve the legacy no-voice behavior: use the lexicon first and invoke
    // espeak only for an OOV word.
    std::vector<std::string> words = SplitWrittenTerms(text);
    if (debug_) {
      std::ostringstream os;
      os << "After splitting to words: ";
      std::string sep;
      for (const auto &word : words) {
        os << sep << word;
        sep = "_";
      }
      SHERPA_ONNX_LOGE("%s", os.str().c_str());
    }

    std::vector<G2pSentence> ans;
    G2pSentence sentence;
    auto flush = [&]() {
      if (!sentence.terms.empty()) {
        ans.push_back(std::move(sentence));
        sentence = {};
      }
    };

    for (const auto &written_word : words) {
      auto word = ToLowerCase(written_word);
      G2pTerm term;
      term.text = written_word;

      if (IsPunctuation(word)) {
        term.phoneme = word;
        sentence.terms.push_back(std::move(term));
        if (word == "." || word == "!" || word == "?" || word == ";") {
          flush();
        }
        continue;
      }

      if (word2ids_.count(word)) {
        const auto &ids = word2ids_.at(word);
        term.phoneme =
            IdsToPhoneme(std::vector<int64_t>(ids.begin(), ids.end()));
      } else {
        if (debug_) {
          SHERPA_ONNX_LOGE("Use espeak-ng to handle the OOV: '%s'",
                           word.c_str());
        }

        piper::eSpeakPhonemeConfig config;
        config.voice = meta_data_.voice;
        std::vector<std::vector<piper::Phoneme>> phoneme_groups;
        CallPhonemizeEspeak(word, config, &phoneme_groups);
        for (const auto &phonemes : phoneme_groups) {
          term.phoneme += PhonemesToString(phonemes);
        }
      }

      // This path historically placed a space after every word, regardless of
      // whether its pronunciation came from the lexicon or espeak.
      term.model_suffix = " ";
      sentence.terms.push_back(std::move(term));
    }

    flush();
    return ans;
  }

  std::vector<G2pSentence> G2pNonChinese(const std::string &text,
                                         const std::string &voice) const {
    if (IsPunctuation(text)) {
      G2pSentence sentence;
      sentence.terms.push_back({text, text, ""});
      std::vector<G2pSentence> ans;
      ans.push_back(std::move(sentence));
      return ans;
    }

    if (!voice.empty()) {
      return G2pNonChineseWithEspeak(text, voice);
    }
    return G2pNonChineseWithoutVoice(text);
  }

  void InitTokens(const std::string &tokens) {
    auto is = OpenInputFile(tokens);
    InitTokens(is);
  }

  template <typename Manager>
  void InitTokens(Manager *mgr, const std::string &tokens) {
    auto buf = ReadFile(mgr, tokens);

    std::istringstream is(std::string(buf.data(), buf.size()));
    InitTokens(is);
  }

  void InitTokens(std::istream &is) {
    token2id_ = ReadTokens(is);  // defined in ./symbol-table.cc

    for (const auto &p : token2id_) {
      id2token_[p.second] = p.first;
    }

    std::u32string s;
    for (const auto &p : token2id_) {
      s = Utf8ToUtf32(p.first);

      if (s.size() != 1) {
        SHERPA_ONNX_LOGE("Error for token %s with id %d", p.first.c_str(),
                         p.second);
        SHERPA_ONNX_EXIT(-1);
      }

      char32_t c = s[0];
      phoneme2id_.insert({c, p.second});
    }
  }

  void InitLexicon(const std::string &lexicon) {
    if (lexicon.empty()) {
      return;
    }

    std::vector<std::string> files;
    SplitStringToVector(lexicon, ",", false, &files);
    for (const auto &f : files) {
      auto is = OpenInputFile(f);
      InitLexicon(is);
    }
  }

  template <typename Manager>
  void InitLexicon(Manager *mgr, const std::string &lexicon) {
    if (lexicon.empty()) {
      return;
    }

    std::vector<std::string> files;
    SplitStringToVector(lexicon, ",", false, &files);
    for (const auto &f : files) {
      auto buf = ReadFile(mgr, f);

      std::istringstream is(std::string(buf.data(), buf.size()));
      InitLexicon(is);
    }
  }

  void InitLexicon(std::istream &is) {
    std::string word;
    std::vector<std::string> token_list;
    std::string token;

    std::string line;
    int32_t line_num = 0;
    int32_t num_warn = 0;
    while (std::getline(is, line)) {
      ++line_num;
      std::istringstream iss(line);

      token_list.clear();
      iss >> word;
      ToLowerCase(&word);

      if (word2ids_.count(word)) {
        num_warn += 1;
        if (num_warn < 10) {
          SHERPA_ONNX_LOGE("Duplicated word: %s at line %d:%s. Ignore it.",
                           word.c_str(), line_num, line.c_str());
        }
        continue;
      }

      while (iss >> token) {
        token_list.push_back(std::move(token));
      }

      std::vector<int32_t> ids = ConvertTokensToIds(token2id_, token_list);

      if (ids.empty() && word != "呣") {
        SHERPA_ONNX_LOGE(
            "Invalid pronunciation for word '%s' at line %d:%s. Ignore it",
            word.c_str(), line_num, line.c_str());
        continue;
      }

      word2ids_.insert({std::move(word), std::move(ids)});
    }

    for (const auto &[key, _] : word2ids_) {
      all_words_.insert(key);
    }
  }

 private:
  OfflineTtsKokoroModelMetaData meta_data_;

  // word to token IDs
  std::unordered_map<std::string, std::vector<int32_t>> word2ids_;
  std::unordered_set<std::string> all_words_;

  // tokens.txt is saved in token2id_
  std::unordered_map<std::string, int32_t> token2id_;
  std::unordered_map<int32_t, std::string> id2token_;

  std::unordered_map<char32_t, int32_t> phoneme2id_;

  bool debug_ = false;
};

KokoroMultiLangLexicon::~KokoroMultiLangLexicon() = default;

KokoroMultiLangLexicon::KokoroMultiLangLexicon(
    const std::string &tokens, const std::string &lexicon,
    const std::string &data_dir, const OfflineTtsKokoroModelMetaData &meta_data,
    bool debug)
    : impl_(std::make_unique<Impl>(tokens, lexicon, data_dir, meta_data,
                                   debug)) {}  // NOLINT

template <typename Manager>
KokoroMultiLangLexicon::KokoroMultiLangLexicon(
    Manager *mgr, const std::string &tokens, const std::string &lexicon,
    const std::string &data_dir, const OfflineTtsKokoroModelMetaData &meta_data,
    bool debug)
    : impl_(std::make_unique<Impl>(mgr, tokens, lexicon, data_dir, meta_data,
                                   debug)) {}  // NOLINT

std::vector<TokenIDs> KokoroMultiLangLexicon::ConvertTextToTokenIds(
    const std::string &text, const std::string &voice /*= ""*/) const {
  return impl_->ConvertTextToTokenIds(text, voice);
}

std::vector<SplitSentence> KokoroMultiLangLexicon::ConvertTextToSplitSentences(
    const std::string &text, const std::string &voice /*= ""*/) const {
  return impl_->ConvertTextToSplitSentences(text, voice);
}

#if __ANDROID_API__ >= 9
template KokoroMultiLangLexicon::KokoroMultiLangLexicon(
    AAssetManager *mgr, const std::string &tokens, const std::string &lexicon,
    const std::string &data_dir, const OfflineTtsKokoroModelMetaData &meta_data,
    bool debug);
#endif

#if __OHOS__
template KokoroMultiLangLexicon::KokoroMultiLangLexicon(
    NativeResourceManager *mgr, const std::string &tokens,
    const std::string &lexicon, const std::string &data_dir,
    const OfflineTtsKokoroModelMetaData &meta_data, bool debug);
#endif

}  // namespace sherpa_onnx
