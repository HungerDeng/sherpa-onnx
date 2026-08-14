// sherpa-onnx/csrc/kokoro-multi-lang-lexicon.cc
//
// Copyright (c)  2025  Xiaomi Corporation

#include "sherpa-onnx/csrc/kokoro-multi-lang-lexicon.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
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
#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/onnx-utils.h"
#include "sherpa-onnx/csrc/phrase-matcher.h"
#include "sherpa-onnx/csrc/symbol-table.h"
#include "sherpa-onnx/csrc/text-utils.h"

namespace sherpa_onnx {

void CallPhonemizeEspeak(const std::string &text,
                         piper::eSpeakPhonemeConfig &config,  // NOLINT
                         std::vector<std::vector<piper::Phoneme>> *phonemes);

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

    std::vector<SplitSentence> ans;

    // HasUnsegmentedScript() checks only the voice prefix and Unicode script
    // ranges. SplitTextIntoLanguageChunks() changes punctuation and
    // whitespace but never those script characters, so _text and the joined
    // normalized chunks produce the same result here.
    bool supports_term_alignments = !HasUnsegmentedScript(_text, voice);

    auto append_sentences = [&](std::vector<SplitSentence> sentences) {
      // ------------------------------------------------------------------
      // Merge the per-chunk token IDs into sentence units. Every chunk
      // returned above is already wrapped in BOS/EOS zeros:
      //   - chunk with > 10+2 tokens      -> keep as its own sentence;
      //   - ans empty                     -> push as-is;
      //   - last sentence + chunk < 50    -> stitch into the last sentence
      //     tokens, or chunk has < 5        (always true for punctuation
      //     tokens                          chunks like "," -> {0, comma, 0});
      //   - otherwise                     -> start a new sentence.
      // Stitching replaces the last sentence's trailing EOS 0 with the
      // chunk's first token (ids[1]) and appends ids[2..] (skipping the
      // chunk's leading BOS 0), so boundaries are not duplicated.
      for (auto &sentence : sentences) {
        const auto &ids = sentence.token_ids.tokens;
        if (ids.size() > 10 + 2) {
          ans.push_back(std::move(sentence));
        } else {
          if (ans.empty()) {
            ans.push_back(std::move(sentence));
          } else {
            size_t merged_size =
                ans.back().token_ids.tokens.size() + ids.size() - 2;
            bool fits_model = merged_size <=
                              static_cast<size_t>(meta_data_.max_token_len + 1);
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
      }
    };

    for (const auto &chunk : text_chunks) {
      std::vector<SplitSentence> sentences;
      if (chunk.language == LanguageType::kChinese) {
        sentences = ConvertChineseToSplitSentences(chunk.text);
      } else {
        sentences = ConvertNonChineseToSplitSentences(chunk.text, voice);
      }
      append_sentences(std::move(sentences));
    }

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
        {":", ","}, {"\\s+", " "},
    };
    for (auto &chunk : normalized_chunks) {
      if (chunk.is_full_width_punctuation) {
        continue;
      }
      for (const auto &replacement : replacements) {
        std::regex re(replacement.first);
        chunk.text =
            std::regex_replace(chunk.text, re, replacement.second);
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
        SHERPA_ONNX_LOGE(
            "%s: %s",
            chunk.language == LanguageType::kChinese ? "Chinese"
                                                     : "Non-Chinese",
            chunk.text.c_str());
      }
    }

    return text_chunks;
  }

  struct PhonemeToken {
    int64_t id;
  };

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
      bool is_ascii =
          c <= 0x7f && std::ispunct(static_cast<unsigned char>(c));
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

  std::vector<SplitSentence> ConvertChineseToSplitSentences(
      const std::string &text) const {
    // Split the run into one UTF-8 character per element, e.g.
    // 中國人民不信邪 → [中, 國, 人, 民, 不, 信, 邪]. PhraseMatcher then groups
    // adjacent characters into the longest lexicon phrases (max 10 chars),
    // falling back to single characters for unmatched ones.
    std::vector<std::string> words = SplitUtf8(text);

    if (debug_) {
      std::ostringstream os;
      std::string sep = "";
      for (const auto &w : words) {
        os << sep << w;
        sep = "_";
      }

#if __OHOS__
      SHERPA_ONNX_LOGE("after splitting into UTF8:\n%{public}s",
                       os.str().c_str());
#else
      SHERPA_ONNX_LOGE("after splitting into UTF8:\n%s", os.str().c_str());
#endif
    }

    std::vector<Term> terms;

    // Walk the whole Chinese run phrase by phrase. PackTerms keeps every
    // phrase whole at sentence boundaries unless one phrase alone is larger
    // than the model budget, in which case it emits repeated-text fragments.
    PhraseMatcher matcher(&all_words_, words, debug_);

    for (const std::string &w : matcher) {
      auto ids = ConvertWordToIds(w);
      if (ids.empty()) {
#if __OHOS__
        SHERPA_ONNX_LOGE("Ignore OOV '%{public}s'", w.c_str());
#else
        SHERPA_ONNX_LOGE("Ignore OOV '%s'", w.c_str());
#endif
        continue;
      }

      Term term;
      term.text = w;
      term.token_ids.tokens.assign(ids.begin(), ids.end());
      term.phoneme = IdsToPhoneme(term.token_ids.tokens);
      terms.push_back(std::move(term));
    }  // for (const std::string &w : matcher)

    auto ans = PackTerms(std::move(terms));

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

  std::vector<PhonemeToken> ConvertPhonemes(
      const std::vector<piper::Phoneme> &phonemes) const {
    std::vector<PhonemeToken> ans;
    for (auto p : phonemes) {
      auto iter = phoneme2id_.find(p);
      if (iter == phoneme2id_.end()) {
        SHERPA_ONNX_LOGE("Skip unknown phonemes. Unicode codepoint: \\U+%04x.",
                         static_cast<uint32_t>(p));
        continue;
      }
      ans.push_back({iter->second});
      if (p == U'.') {
        ans.push_back({token2id_.at(" ")});
      }
    }
    return ans;
  }

  std::vector<PhonemeToken> Phonemize(const std::string &text,
                                      const std::string &voice) const {
    piper::eSpeakPhonemeConfig config;
    config.voice = voice;
    std::vector<std::vector<piper::Phoneme>> phonemes;
    CallPhonemizeEspeak(text, config, &phonemes);
    std::vector<PhonemeToken> ans;
    for (const auto &p : phonemes) {
      auto tokens = ConvertPhonemes(p);
      ans.insert(ans.end(), std::make_move_iterator(tokens.begin()),
                 std::make_move_iterator(tokens.end()));
    }
    return ans;
  }

  size_t BestPrefixLength(const std::vector<PhonemeToken> &full, size_t begin,
                          size_t available,
                          const std::vector<PhonemeToken> &hint) const {
    if (available == 0 || hint.empty()) {
      return 0;
    }

    size_t max_candidate =
        std::min(available, std::max<size_t>(32, hint.size() * 2 + 8));
    std::vector<int32_t> previous(hint.size() + 1);
    std::vector<int32_t> current(hint.size() + 1);
    for (size_t i = 0; i <= hint.size(); ++i) {
      previous[i] = static_cast<int32_t>(i);
    }

    size_t best_len = 0;
    int32_t best_score = previous.back() * 4;
    int32_t space_id = token2id_.at(" ");
    for (size_t len = 1; len <= max_candidate; ++len) {
      current[0] = static_cast<int32_t>(len);
      for (size_t j = 1; j <= hint.size(); ++j) {
        int32_t substitution =
            previous[j - 1] +
            (full[begin + len - 1].id == hint[j - 1].id ? 0 : 1);
        current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                               substitution});
      }
      int32_t score = current.back() * 4 +
                      std::abs(static_cast<int32_t>(len) -
                               static_cast<int32_t>(hint.size()));
      if (full[begin + len - 1].id == space_id) {
        --score;
      }
      if (score < best_score) {
        best_score = score;
        best_len = len;
      }
      previous.swap(current);
    }
    return best_len == 0 ? 1 : best_len;
  }

  std::vector<size_t> AlignTermBoundaries(
      const std::vector<PhonemeToken> &full,
      const std::vector<std::vector<PhonemeToken>> &term_hints) const {
    std::vector<PhonemeToken> hints;
    std::vector<size_t> hint_ends;
    for (const auto &term : term_hints) {
      hints.insert(hints.end(), term.begin(), term.end());
      hint_ends.push_back(hints.size());
    }

    std::vector<size_t> ans(term_hints.size());
    if (term_hints.empty()) {
      return ans;
    }
    if (hints.empty()) {
      ans.back() = full.size();
      return ans;
    }

    const size_t num_rows = hints.size() + 1;
    const size_t num_cols = full.size() + 1;
    constexpr size_t kMaxAlignmentCells = 32 * 1024 * 1024;
    bool can_use_global_alignment =
        num_cols != 0 && num_rows <= kMaxAlignmentCells / num_cols;

    if (!can_use_global_alignment) {
      size_t cursor = 0;
      for (size_t i = 0; i != term_hints.size(); ++i) {
        size_t len = full.size() - cursor;
        if (i + 1 != term_hints.size()) {
          len = BestPrefixLength(full, cursor, full.size() - cursor,
                                 term_hints[i]);
        }
        cursor += len;
        ans[i] = cursor;
      }
      return ans;
    }

    // Globally align the contextual one-pass espeak output with the
    // concatenated per-term hints. This prevents a locally greedy match from
    // shifting every following word when contextual output contains extra
    // phonemes, for example when a leading period is spoken as "dot".
    // Direction: 0 consumes both sequences, 1 consumes a hint, and 2 consumes
    // a contextual token.
    std::vector<uint8_t> directions(num_rows * num_cols);
    std::vector<int32_t> previous(num_cols);
    std::vector<int32_t> current(num_cols);
    for (size_t j = 0; j != num_cols; ++j) {
      previous[j] = static_cast<int32_t>(j);
      if (j != 0) {
        directions[j] = 2;
      }
    }

    for (size_t i = 1; i != num_rows; ++i) {
      current[0] = static_cast<int32_t>(i);
      directions[i * num_cols] = 1;
      for (size_t j = 1; j != num_cols; ++j) {
        bool is_match = hints[i - 1].id == full[j - 1].id;
        // A mismatch costs the same as deleting and inserting. Prefer those
        // operations on ties so an unrelated contextual prefix such as the
        // spoken word "dot" cannot consume the first phonemes of the next
        // written word through cheap substitutions.
        int32_t diagonal = previous[j - 1] + (is_match ? 0 : 2);
        int32_t remove_hint = previous[j] + 1;
        int32_t insert_contextual = current[j - 1] + 1;

        int32_t value = insert_contextual;
        uint8_t direction = 2;
        if (remove_hint < value) {
          value = remove_hint;
          direction = 1;
        }
        if (diagonal < value || (is_match && diagonal == value)) {
          value = diagonal;
          direction = 0;
        }
        current[j] = value;
        directions[i * num_cols + j] = direction;
      }
      previous.swap(current);
    }

    std::vector<uint8_t> reversed_operations;
    reversed_operations.reserve(hints.size() + full.size());
    size_t i = hints.size();
    size_t j = full.size();
    while (i != 0 || j != 0) {
      uint8_t direction = directions[i * num_cols + j];
      reversed_operations.push_back(direction);
      if (direction == 0) {
        --i;
        --j;
      } else if (direction == 1) {
        --i;
      } else {
        --j;
      }
    }
    std::reverse(reversed_operations.begin(), reversed_operations.end());

    size_t hint_cursor = 0;
    size_t full_cursor = 0;
    size_t operation_index = 0;
    auto apply_operation = [&]() {
      uint8_t operation = reversed_operations[operation_index++];
      if (operation == 0) {
        ++hint_cursor;
        ++full_cursor;
      } else if (operation == 1) {
        ++hint_cursor;
      } else {
        ++full_cursor;
      }
    };

    for (size_t term_index = 0; term_index != hint_ends.size();
         ++term_index) {
      size_t hint_end = hint_ends[term_index];
      while (operation_index != reversed_operations.size() &&
             hint_cursor < hint_end) {
        apply_operation();
      }

      // Insertions at a term boundary belong to the preceding term. When an
      // empty-hint term follows at the same boundary (usually punctuation),
      // leave them for that term instead.
      bool followed_by_empty_term =
          term_index + 1 != hint_ends.size() &&
          hint_ends[term_index + 1] == hint_end;
      if (!followed_by_empty_term) {
        while (operation_index != reversed_operations.size() &&
               hint_cursor == hint_end &&
               reversed_operations[operation_index] == 2) {
          apply_operation();
        }
      }
      ans[term_index] = full_cursor;
    }

    return ans;
  }

  std::vector<SplitSentence> ConvertTextToSplitSentencesWithEspeak(
      const std::string &text, const std::string &voice) const {
    piper::eSpeakPhonemeConfig config;
    config.voice = voice;
    std::vector<std::vector<piper::Phoneme>> phoneme_groups;
    CallPhonemizeEspeak(text, config, &phoneme_groups);

    std::vector<PhonemeToken> full;
    std::vector<size_t> group_ends;
    for (const auto &group : phoneme_groups) {
      auto tokens = ConvertPhonemes(group);
      full.insert(full.end(), std::make_move_iterator(tokens.begin()),
                  std::make_move_iterator(tokens.end()));
      group_ends.push_back(full.size());
    }
    if (full.empty()) {
      return {};
    }

    auto written_terms = SplitWrittenTerms(text);
    if (written_terms.empty()) {
      written_terms.push_back(text);
    }

    std::vector<std::vector<PhonemeToken>> term_hints;
    term_hints.reserve(written_terms.size());
    for (const auto &term : written_terms) {
      term_hints.push_back(Phonemize(term, voice));
    }
    if (debug_) {
      std::vector<int64_t> full_ids;
      for (const auto &token : full) {
        full_ids.push_back(token.id);
      }
      SHERPA_ONNX_LOGE("Contextual phonemes: %s",
                       IdsToPhoneme(full_ids).c_str());
      for (size_t i = 0; i != term_hints.size(); ++i) {
        std::vector<int64_t> hint_ids;
        for (const auto &token : term_hints[i]) {
          hint_ids.push_back(token.id);
        }
        SHERPA_ONNX_LOGE("Term hint '%s': %s", written_terms[i].c_str(),
                         IdsToPhoneme(hint_ids).c_str());
      }
    }
    auto term_ends = AlignTermBoundaries(full, term_hints);

    // A punctuation mark can be silent in isolation but spoken in context;
    // for example, a period immediately before an English word may become
    // "dot". If global alignment leaves such punctuation empty, anchor the
    // following term's exact hint and assign the contextual prefix to the
    // punctuation term.
    for (size_t i = 0; i + 1 < written_terms.size(); ++i) {
      size_t begin = i == 0 ? 0 : term_ends[i - 1];
      if (!IsPunctuation(written_terms[i]) || term_ends[i] != begin ||
          term_hints[i + 1].empty()) {
        continue;
      }

      size_t search_end = term_ends[i + 1];
      const auto &next_hint = term_hints[i + 1];
      if (search_end == begin) {
        continue;
      }

      size_t best_candidate = begin;
      size_t best_length = 0;
      int32_t best_distance = std::numeric_limits<int32_t>::max();
      int32_t best_score = std::numeric_limits<int32_t>::max();
      for (size_t candidate = begin;
           candidate < search_end; ++candidate) {
        size_t max_length = std::min(
            search_end - candidate,
            std::max<size_t>(8, next_hint.size() * 2 + 4));
        std::vector<int32_t> previous(next_hint.size() + 1);
        std::vector<int32_t> current(next_hint.size() + 1);
        for (size_t k = 0; k <= next_hint.size(); ++k) {
          previous[k] = static_cast<int32_t>(k);
        }
        for (size_t length = 1; length <= max_length; ++length) {
          current[0] = static_cast<int32_t>(length);
          for (size_t k = 1; k <= next_hint.size(); ++k) {
            int32_t substitution =
                previous[k - 1] +
                (full[candidate + length - 1].id == next_hint[k - 1].id ? 0
                                                                        : 1);
            current[k] =
                std::min({previous[k] + 1, current[k - 1] + 1, substitution});
          }
          int32_t distance = current.back();
          int32_t score =
              distance * 4 +
              std::abs(static_cast<int32_t>(length) -
                       static_cast<int32_t>(next_hint.size()));
          if (score < best_score) {
            best_score = score;
            best_distance = distance;
            best_candidate = candidate;
            best_length = length;
          }
          previous.swap(current);
        }
      }

      size_t comparison_length = std::max(best_length, next_hint.size());
      if (best_candidate != begin &&
          static_cast<size_t>(best_distance * 2) <= comparison_length + 1) {
        term_ends[i] = best_candidate;
      }
    }

    std::vector<Term> aligned;
    size_t cursor = 0;
    for (size_t i = 0; i != written_terms.size(); ++i) {
      size_t end = term_ends[i];
      Term term;
      term.text = written_terms[i];
      for (size_t j = cursor; j != end; ++j) {
        term.token_ids.tokens.push_back(full[j].id);
      }
      term.phoneme = IdsToPhoneme(term.token_ids.tokens);
      aligned.push_back(std::move(term));
      cursor = end;
    }

    // Respect sentence boundaries returned by espeak. If a frontend term
    // crosses one, repeat its written text and expose the corresponding
    // partial phoneme span on each side.
    std::vector<SplitSentence> ans;
    std::vector<Term> group_terms;
    std::vector<Term> pending_terms;
    size_t absolute = 0;
    size_t group_index = 0;
    for (const auto &term : aligned) {
      if (term.token_ids.tokens.empty()) {
        if (!group_terms.empty()) {
          group_terms.push_back(term);
        } else {
          pending_terms.push_back(term);
        }
        continue;
      }
      size_t local = 0;
      while (local < term.token_ids.tokens.size()) {
        while (group_index < group_ends.size() &&
               absolute == group_ends[group_index]) {
          auto packed = PackTerms(std::move(group_terms));
          ans.insert(ans.end(), std::make_move_iterator(packed.begin()),
                     std::make_move_iterator(packed.end()));
          group_terms.clear();
          ++group_index;
        }
        size_t boundary = group_index < group_ends.size()
                              ? group_ends[group_index]
                              : full.size();
        size_t count = std::min(term.token_ids.tokens.size() - local,
                                boundary - absolute);
        Term fragment;
        fragment.text = term.text;
        fragment.token_ids.tokens.assign(term.token_ids.tokens.begin() + local,
                                         term.token_ids.tokens.begin() + local +
                                             count);
        fragment.phoneme = IdsToPhoneme(fragment.token_ids.tokens);
        if (!pending_terms.empty()) {
          group_terms.insert(group_terms.end(),
                             std::make_move_iterator(pending_terms.begin()),
                             std::make_move_iterator(pending_terms.end()));
          pending_terms.clear();
        }
        group_terms.push_back(std::move(fragment));
        local += count;
        absolute += count;
      }
    }
    if (!pending_terms.empty()) {
      if (!group_terms.empty()) {
        group_terms.insert(group_terms.end(),
                           std::make_move_iterator(pending_terms.begin()),
                           std::make_move_iterator(pending_terms.end()));
      } else if (!ans.empty()) {
        ans.back().terms.insert(
            ans.back().terms.end(),
            std::make_move_iterator(pending_terms.begin()),
            std::make_move_iterator(pending_terms.end()));
      }
    }
    auto packed = PackTerms(std::move(group_terms));
    ans.insert(ans.end(), std::make_move_iterator(packed.begin()),
               std::make_move_iterator(packed.end()));
    return ans;
  }

  std::vector<SplitSentence> ConvertNonChineseToSplitSentences(
      const std::string &text, const std::string &voice) const {
    if (IsPunctuation(text)) {
      Term term;
      term.text = text;
      term.phoneme = text;
      term.token_ids.tokens = {token2id_.at(text)};
      return {MakeSentence({std::move(term)})};
    }

    if (!voice.empty() && HasUnsegmentedScript(text, voice)) {
      auto token_ids = ConvertTextToTokenIdsKokoroOrKitten(
          phoneme2id_, meta_data_.max_token_len, text, voice);
      std::vector<SplitSentence> ans;
      ans.reserve(token_ids.size());
      for (auto &ids : token_ids) {
        SplitSentence sentence;
        sentence.token_ids = std::move(ids);
        ans.push_back(std::move(sentence));
      }
      return ans;
    }

    // Long non-Chinese runs are packed at term boundaries after a contextual,
    // one-pass espeak-ng conversion.
    if (!voice.empty()) {
      return ConvertTextToSplitSentencesWithEspeak(text, voice);
    }

    // If voice is empty, we split the text into words and use the lexicon
    // to lookup the pronunciation of each word, fallback to espeak if
    // a word is not in the lexicon.

    std::vector<std::string> words = SplitWrittenTerms(text);
    if (debug_) {
      std::ostringstream os;
      os << "After splitting to words: ";
      std::string sep;
      for (const auto &w : words) {
        os << sep << w;
        sep = "_";
      }
      SHERPA_ONNX_LOGE("%s", os.str().c_str());
    }

    std::vector<SplitSentence> ans;
    // The voice-empty path also packs complete word terms. Punctuation
    // ". ! ? ;" ends a sentence.
    int32_t space_id = token2id_.at(" ");

    std::vector<Term> terms;

    auto flush = [&]() {
      auto packed = PackTerms(std::move(terms));
      ans.insert(ans.end(), std::make_move_iterator(packed.begin()),
                 std::make_move_iterator(packed.end()));
      terms.clear();
    };

    for (const auto &_word : words) {
      auto word = ToLowerCase(_word);
      if (IsPunctuation(word)) {
        Term term;
        term.text = _word;
        term.phoneme = word;
        term.token_ids.tokens = {token2id_.at(word)};
        terms.push_back(std::move(term));

        if (word == "." || word == "!" || word == "?" || word == ";") {
          // Note: You can add more punctuations here to split the text
          // into sentences. We just use four here: .!?;
          flush();
        }
      } else if (word2ids_.count(word)) {
        const auto &ids = word2ids_.at(word);
        Term term;
        term.text = _word;
        term.token_ids.tokens.assign(ids.begin(), ids.end());
        term.phoneme = IdsToPhoneme(term.token_ids.tokens);
        term.token_ids.tokens.push_back(space_id);
        terms.push_back(std::move(term));
      } else {
        if (debug_) {
          SHERPA_ONNX_LOGE("Use espeak-ng to handle the OOV: '%s'",
                           word.c_str());
        }

        piper::eSpeakPhonemeConfig config;

        config.voice = meta_data_.voice;

        std::vector<std::vector<piper::Phoneme>> phonemes;

        CallPhonemizeEspeak(word, config, &phonemes);
        // Note phonemes[i] contains a vector of unicode codepoints;
        // we need to convert them to utf8

        std::vector<int64_t> ids;
        for (const auto &v : phonemes) {
          for (const auto p : v) {
            auto token = Utf32ToUtf8(p);
            if (token2id_.count(token)) {
              ids.push_back(token2id_.at(token));
            } else {
              if (debug_) {
                SHERPA_ONNX_LOGE("Skip OOV token '%s' from '%s'", token.c_str(),
                                 word.c_str());
              }
            }
          }
        }

        Term term;
        term.text = _word;
        term.token_ids.tokens = std::move(ids);
        term.phoneme = IdsToPhoneme(term.token_ids.tokens);
        term.token_ids.tokens.push_back(space_id);
        terms.push_back(std::move(term));
      }
    }

    flush();

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

std::vector<SplitSentence>
KokoroMultiLangLexicon::ConvertTextToSplitSentences(
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
