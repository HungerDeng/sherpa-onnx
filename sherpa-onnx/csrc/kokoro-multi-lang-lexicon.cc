// sherpa-onnx/csrc/kokoro-multi-lang-lexicon.cc
//
// Copyright (c)  2025  Xiaomi Corporation

#include "sherpa-onnx/csrc/kokoro-multi-lang-lexicon.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unicode/brkiter.h>
#include <unicode/locid.h>
#include <unicode/unistr.h>

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
  enum class ChunkType {
    kLatin,
    kNonLatin,
  };

  struct TextChunk {
    std::string text;
    ChunkType type = ChunkType::kLatin;
  };

  struct LexiconSource {
    std::string language;
    std::string path;
    std::function<std::vector<char>()> read;
  };

  struct LexiconData {
    std::unordered_map<std::string, std::string> word2phonemes;
    std::unordered_set<std::string> all_words;
    int32_t max_phrase_len = 1;
  };

  // G2P implementations populate only text and phonemes. model_suffix holds
  // phonemes that affect model input but are not part of the term alignment,
  // such as the separator between two espeak words or the pause after a
  // clause comma.
  struct G2pTerm {
    std::string text;
    std::string phonemes;
    std::string model_suffix;

    // True when this term was reconstructed from a gap between espeak's word
    // spans rather than returned by espeak as a word or clause terminator.
    //
    // For example, espeak may return only "record" and "doesn't" for the
    // source "record. doesn't". We reconstruct the missing period as
    // {text: ".", phonemes: "."} and intentionally tokenize it for Kokoro.
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

    RegisterLexiconSources(lexicon);

    InitEspeak(data_dir);  // See ./piper-phonemize-lexicon.cc
  }

  template <typename Manager>
  Impl(Manager *mgr, const std::string &tokens, const std::string &lexicon,
       const std::string &data_dir,
       const OfflineTtsKokoroModelMetaData &meta_data, bool debug)
      : meta_data_(meta_data), debug_(debug) {
    InitTokens(mgr, tokens);

    RegisterLexiconSources(mgr, lexicon);

    // we assume you have copied data_dir from assets to some path

    InitEspeak(data_dir);  // See ./piper-phonemize-lexicon.cc
  }

  std::vector<SplitSentence> ConvertTextToSplitSentences(
      const std::string &_text, const std::string &voice) const {
    auto text_chunks = SplitTextIntoLanguageChunks(_text);

    // SplitTextIntoLanguageChunks() preserves the original text exactly, so
    // script detection can use _text directly.
    bool supports_term_alignments = !HasUnsegmentedScript(_text, voice);

    std::vector<G2pSentence> g2p_sentences;
    for (const auto &chunk : text_chunks) {
      std::vector<G2pSentence> sentences;
      if (chunk.type == ChunkType::kNonLatin) {
        sentences = G2pWithSelectedLexicon(chunk.text, voice);
      } else if (IsFullOfPunctuation(chunk.text)) {
        G2pSentence sentence;
        sentence.terms.push_back({chunk.text, chunk.text, ""});
        sentences.push_back(std::move(sentence));
      } else {
        sentences = G2pWithEspeak(chunk.text, voice);
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
  static const std::map<char32_t, char32_t> &FullWidthPunctuationAliases() {
    static const std::map<char32_t, char32_t> aliases = {
        // Full-width forms of punctuation supported by Kokoro.
        {U'；', U';'},
        {U'：', U':'},
        {U'，', U','},
        {U'．', U'.'},
        {U'！', U'!'},
        {U'？', U'?'},
        {U'＂', U'"'},
        {U'（', U'('},
        {U'）', U')'},
        {U'－', U'—'},

        // CJK punctuation.
        {U'、', U','},
        {U'。', U'.'},
        {U'「', U'“'},
        {U'」', U'”'},
        {U'『', U'“'},
        {U'』', U'”'},
        {U'《', U'“'},
        {U'》', U'”'},
        {U'〈', U'“'},
        {U'〉', U'”'},
        {U'【', U'“'},
        {U'】', U'”'},
        {U'〔', U'“'},
        {U'〕', U'”'},
        {U'・', U' '},
    };
    return aliases;
  }

  static bool IsLatinCodepoint(char32_t c) {
    return
        // Basic Latin uppercase letters: U+0041 ('A') through U+005A ('Z').
        (c >= U'A' && c <= U'Z') ||
        // Basic Latin lowercase letters: U+0061 ('a') through U+007A ('z').
        (c >= U'a' && c <= U'z') ||
        // Latin-1 Supplement and Latin Extended-B: U+00C0 through U+024F.
        // Examples include 'À' (U+00C0), 'é' (U+00E9), 'Ā' (U+0100),
        // 'č' (U+010D), 'Ǆ' (U+01C4), 'ȧ' (U+0227), and 'ɏ' (U+024F).
        (c >= 0x00c0 && c <= 0x024f) ||
        // Latin Extended Additional: U+1E00 through U+1EFF, containing
        // further precomposed Latin letters with diacritics. Examples include
        // 'Ḁ' (U+1E00), 'ḡ' (U+1E21), 'ṣ' (U+1E63), 'ẞ' (U+1E9E),
        // 'ạ' (U+1EA1), 'ế' (U+1EBF), 'ệ' (U+1EC7), 'ố' (U+1ED1),
        // and 'ỹ' (U+1EF9).
        (c >= 0x1e00 && c <= 0x1eff);
  }

  static std::string BaseLanguage(const std::string &language) {
    if (language == "ja-cutlet" || language == "ja-jtalk") {
      return "ja";
    }
    return language;
  }

  std::vector<TextChunk> SplitTextIntoLanguageChunks(
      const std::string &text) const {
    std::vector<TextChunk> text_chunks;
    std::string current;
    ChunkType current_type = ChunkType::kLatin;

    auto flush_current = [&]() {
      if (!current.empty()) {
        text_chunks.push_back({std::move(current), current_type});
        current.clear();
      }
    };

    // Decode UTF-8 once because the Latin ranges are Unicode codepoint
    // properties, not byte properties. Iterating std::string bytes would
    // split every non-ASCII character into multiple values.
    for (char32_t c : Utf8ToUtf32(text)) {
      bool is_latin =
          IsLatinCodepoint(c) ||
          (c <= 0x7f &&
           (std::isspace(static_cast<unsigned char>(c)) ||
            std::ispunct(static_cast<unsigned char>(c))));
      ChunkType type =
          is_latin ? ChunkType::kLatin : ChunkType::kNonLatin;
      if (!current.empty() && type != current_type) {
        flush_current();
      }
      current_type = type;
      current += Utf32ToUtf8(c);
    }
    flush_current();

    if (debug_) {
      std::string chunked_text;
      for (const auto &chunk : text_chunks) {
        chunked_text += chunk.text;
      }
      SHERPA_ONNX_LOGE("After language chunking:\n%s", chunked_text.c_str());
      for (const auto &chunk : text_chunks) {
        const char *type =
            chunk.type == ChunkType::kLatin ? "Latin" : "NonLatin";
        SHERPA_ONNX_LOGE("%s: %s", type, chunk.text.c_str());
      }
    }

    return text_chunks;
  }

  bool HasUnsegmentedScript(const std::string &text,
                            const std::string &voice = "") const {
    std::string language = BaseLanguage(voice);
    if (language.rfind("th", 0) == 0 || language.rfind("lo", 0) == 0 ||
        language.rfind("my", 0) == 0 ||
        language.rfind("km", 0) == 0) {
      return true;
    }
    for (char32_t c : Utf8ToUtf32(text)) {
      // Kana is segmented only when Japanese is selected. Thai, Lao, Myanmar,
      // and Khmer do not have a word segmenter in this frontend. Audio still
      // uses the normal frontend output, but term alignments are suppressed.
      if ((language != "ja" && c >= 0x3040 && c <= 0x30ff) ||
          (c >= 0x0e00 && c <= 0x0eff) ||
          (c >= 0x1000 && c <= 0x109f) || (c >= 0x1780 && c <= 0x17ff)) {
        return true;
      }
    }
    return false;
  }

  std::string IdsToPhoneme(const std::vector<int64_t> &ids) const {
    std::string ans;
    size_t begin = 0;
    size_t end = ids.size();
    int32_t space_id = token2id_.at(" ");
    while (begin < end && ids[begin] == space_id) {
      ++begin;
    }
    while (end > begin && ids[end - 1] == space_id) {
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

  // A G2P sentence is a sequence of logical terms, but Kokoro accepts only a
  // bounded number of token IDs per model invocation.  The bound must be
  // applied after tokenization: a short written phrase can expand to many
  // phoneme IDs, and an espeak term can also carry model-only suffix IDs (for
  // example, the separator space after a word).
  //
  // PackTerms() first groups complete terms while their combined token IDs fit
  // in the content budget.  For example, with max_content == 5, terms whose
  // IDs have lengths 2, 3, and 2 become two model sentences:
  //
  //   [term-1 (2 IDs), term-2 (3 IDs)]  [term-3 (2 IDs)]
  //
  // MakeSentence() then adds the BOS/EOS IDs around each group.  Keeping those
  // wrappers at the sentence level is important because every packed unit is
  // sent to Kokoro independently.
  //
  // A single term can be larger than the budget.  Such a term is split by
  // token-ID position; for example, seven IDs with max_content == 5 become
  // [first five IDs] and [last two IDs], each with its own BOS/EOS pair.  The
  // fragment keeps the original written text, while its
  // num_phoneme_tokens/inferred_phonemes fields are clipped to the fragment's
  // leading phoneme IDs.  Any trailing model-only suffix remains tokenized but
  // is excluded from the fragment's public phoneme alignment.
  //
  // Splitting at this stage also preserves the duration-alignment contract:
  // InferTermAlignments() can account for each fragment's phoneme IDs and
  // suffix IDs separately.  TokenizePackAndMerge() may combine short packed
  // sentences later when the combined sequence still fits, but it cannot
  // safely replace this initial budget enforcement.
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
        fragment.raw_phonemes = term.raw_phonemes;
        fragment.token_ids.tokens.assign(ids.begin() + begin,
                                         ids.begin() + end);
        size_t phoneme_begin =
            std::min(begin, static_cast<size_t>(term.num_phoneme_tokens));
        size_t phoneme_end =
            std::min(end, static_cast<size_t>(term.num_phoneme_tokens));
        fragment.num_phoneme_tokens =
            static_cast<int32_t>(phoneme_end - phoneme_begin);
        fragment.inferred_phonemes = IdsToPhoneme(std::vector<int64_t>(
            fragment.token_ids.tokens.begin(),
            fragment.token_ids.tokens.begin() + fragment.num_phoneme_tokens));
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
    term.raw_phonemes = std::move(g2p_term.phonemes);

    auto phoneme_ids = TokenizePhonemes(term.raw_phonemes, term.text);
    term.inferred_phonemes = IdsToPhoneme(phoneme_ids);
    term.num_phoneme_tokens = static_cast<int32_t>(phoneme_ids.size());

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

  bool IsFullOfPunctuation(const std::string &text) const {
    auto codepoints = Utf8ToUtf32(text);
    if (codepoints.empty()) {
      return false;
    }

    return std::all_of(codepoints.begin(), codepoints.end(), [](char32_t c) {
      return c == U';' || c == U':' || c == U',' || c == U'.' || c == U'!' ||
             c == U'?' || c == U'—' || c == U'…' || c == U'"' || c == U'(' ||
             c == U')' || c == U'“' || c == U'”';
    });
  }

  std::string ConvertWordToPhonemes(const std::string &w,
                                    const LexiconData &data) const {
    std::string ans;
    auto iter = data.word2phonemes.find(w);
    if (iter != data.word2phonemes.end()) {
      ans = iter->second;
    } else if (token2id_.count(w)) {
      // For some punctuation existing in the token2id_, we directly use the w itself as the phoneme
      ans = w;
    } else {
      std::vector<std::string> words = SplitUtf8(w);
      for (const auto &word : words) {
        auto word_iter = data.word2phonemes.find(word);
        if (word_iter != data.word2phonemes.end()) {
          ans += word_iter->second;
        } else if (token2id_.count(word)) {
          ans += word;
        } else {
#if __OHOS__
          SHERPA_ONNX_LOGE(
              "Warning: No lexicon entry or model token for '%{public}s' "
              "while converting lexicon chunk '%{public}s'. Ignore it.",
              word.c_str(), w.c_str());
#else
          SHERPA_ONNX_LOGE(
              "Warning: No lexicon entry or model token for '%s' while "
              "converting lexicon chunk '%s'. Ignore it.",
              word.c_str(), w.c_str());
#endif
        }
      }
    }

    if (debug_ && !ans.empty()) {
#if __OHOS__
      SHERPA_ONNX_LOGE("%{public}s: %{public}s", w.c_str(), ans.c_str());
#else
      SHERPA_ONNX_LOGE("%s: %s", w.c_str(), ans.c_str());
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

  bool SplitWithIcu(const std::string &text, const std::string &language,
                    std::vector<std::string> *words) const {
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> break_iterator(
        icu::BreakIterator::createWordInstance(icu::Locale(language.c_str()),
                                               status));
    if (U_FAILURE(status) || !break_iterator) {
#if __OHOS__
      SHERPA_ONNX_LOGE(
          "Failed to create ICU %{public}s word iterator: %{public}s. "
          "Fall back to PhraseMatcher.",
          language.c_str(), u_errorName(status));
#else
      SHERPA_ONNX_LOGE(
          "Failed to create ICU %s word iterator: %s. "
          "Fall back to PhraseMatcher.",
          language.c_str(), u_errorName(status));
#endif
      return false;
    }

    icu::UnicodeString unicode_text = icu::UnicodeString::fromUTF8(text);
    break_iterator->setText(unicode_text);

    words->clear();
    int32_t start = break_iterator->first();
    while (true) {
      int32_t end = break_iterator->next();
      if (end == icu::BreakIterator::DONE) {
        break;
      }

      std::string word;
      unicode_text.tempSubStringBetween(start, end).toUTF8String(word);
      if (!word.empty() &&
          word.find_first_not_of(" \t\r\n") != std::string::npos) {
        words->push_back(std::move(word));
      }
      start = end;
    }
    return true;
  }

  std::vector<G2pSentence> G2pSegmentedLexicon(
      const std::string &text, const std::string &language,
      const LexiconData &data) const {
    // ICU supplies the written term boundaries. A term uses its exact lexicon
    // pronunciation when present; ConvertWordToPhonemes() otherwise composes
    // the pronunciation from single-character entries without greedy
    // sub-phrase matching.
    std::vector<std::string> words;
    bool used_icu = SplitWithIcu(text, language, &words);
    if (!used_icu) {
      // Preserve the previous behavior if ICU cannot load its word iterator
      // or dictionary data: greedily match the longest lexicon phrases, then
      // fall back to individual UTF-8 characters.
      auto characters = SplitUtf8(text);
      PhraseMatcher matcher(&data.all_words, characters, debug_,
                            data.max_phrase_len);
      words.assign(matcher.begin(), matcher.end());
    }

    if (debug_) {
      std::ostringstream os;
      std::string sep;
      for (const auto &word : words) {
        os << sep << word;
        sep = "_";
      }
#if __OHOS__
      SHERPA_ONNX_LOGE(
          "After %{public}s %{public}s segmentation:\n%{public}s",
          used_icu ? "ICU" : "fallback greedy", language.c_str(),
          os.str().c_str());
#else
      SHERPA_ONNX_LOGE("After %s %s segmentation:\n%s",
                       used_icu ? "ICU" : "fallback greedy",
                       language.c_str(), os.str().c_str());
#endif
    }

    G2pSentence sentence;
    for (const std::string &word : words) {
      std::string phonemes = ConvertWordToPhonemes(word, data);
      if (phonemes.empty()) {
#if __OHOS__
        SHERPA_ONNX_LOGE(
            "Warning: Empty pronunciation for lexicon term '%{public}s'.",
            word.c_str());
#else
        SHERPA_ONNX_LOGE(
            "Warning: Empty pronunciation for lexicon term '%s'.",
            word.c_str());
#endif
      }

      G2pTerm term;
      term.text = word;
      term.phonemes = std::move(phonemes);
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
              IsFullOfPunctuation(sentence->terms[i].text)) {
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
      term.phonemes = punctuation_text;
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

  std::vector<G2pSentence> G2pWithEspeak(
      const std::string &text, const std::string &voice) const {
    piper::eSpeakPhonemeConfig config;
    config.voice = BaseLanguage(voice);

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
        term.phonemes = PhonemesToString(word.phonemes);
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
                           term.phonemes.c_str());
        }
      }
    }
    return ans;
  }

  std::vector<G2pSentence> G2pWithSelectedLexicon(
      const std::string &text, const std::string &language) const {
    if (language != "cmn" && language != "ja-cutlet" &&
        language != "ja-jtalk") {
      throw std::runtime_error(
          "Kokoro lexicon G2P is not supported for language '" +
          language + "'.");
    }

    auto data = GetLexiconData(language);
    if (!data) {
      throw std::runtime_error(
          "No Kokoro lexicon is configured for language '" +
          language + "'.");
    }

    return G2pSegmentedLexicon(text, BaseLanguage(language), *data);
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

    // Register full-width and CJK punctuation as aliases of the model's
    // canonical punctuation tokens. id2token_ remains canonical so public
    // phoneme strings use the spelling from tokens.txt.
    for (const auto &[alias, canonical] : FullWidthPunctuationAliases()) {
      auto iter = token2id_.find(Utf32ToUtf8(canonical));
      if (iter != token2id_.end()) {
        token2id_[Utf32ToUtf8(alias)] = iter->second;
      }
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

  static std::string GetLexiconLanguage(const std::string &path) {
    static const std::map<std::string, std::string> filenames = {
        {"en-us", "lexicon-us-en.txt"},
        {"en-gb", "lexicon-gb-en.txt"},
        {"cmn", "lexicon-cmn.txt"},
        {"ja-cutlet", "lexicon-ja-cutlet.txt"},
        {"ja-jtalk", "lexicon-ja-jtalk.txt"},
    };

    // extract the filename from the parameter path
    size_t pos = path.find_last_of("/\\");
    std::string name =
        pos == std::string::npos ? path : path.substr(pos + 1);

    for (const auto &[key, filename] : filenames) {
      if (name == filename) {
        return key;
      }
    }

    throw std::invalid_argument(
        "Unsupported Kokoro lexicon filename '" + name + "'.");
  }

  void RegisterSource(std::string language, std::string path,
                      std::function<std::vector<char>()> reader) {
    LexiconSource source;
    source.language = std::move(language);
    source.path = std::move(path);
    source.read = std::move(reader);

    lexicon_sources_[source.language].push_back(std::move(source));
  }

  void RegisterLexiconSources(const std::string &lexicon) {
    if (lexicon.empty()) {
      return;
    }

    std::vector<std::string> files;
    SplitStringToVector(lexicon, ",", false, &files);
    for (const auto &path : files) {
      std::string language = GetLexiconLanguage(path);
      if (!FileExists(path)) {
        throw std::runtime_error("Kokoro lexicon does not exist: '" +
                                 path + "'.");
      }
      RegisterSource(std::move(language), path,
                     [path]() { return ReadFile(path); });
    }
  }

  template <typename Manager>
  void RegisterLexiconSources(Manager *mgr, const std::string &lexicon) {
    if (lexicon.empty()) {
      return;
    }

    std::vector<std::string> files;
    SplitStringToVector(lexicon, ",", false, &files);
    for (const auto &path : files) {
      std::string language = GetLexiconLanguage(path);
      RegisterSource(std::move(language), path,
                     [mgr, path]() { return ReadFile(mgr, path); });
    }
  }

  std::shared_ptr<LexiconData> ParseLexicon(
      const LexiconSource &source, const std::vector<char> &buffer) const {
    auto data = std::make_shared<LexiconData>();
    std::istringstream is(std::string(buffer.data(), buffer.size()));
    std::string word;
    std::vector<std::string> token_list;
    std::string token;

    std::string line;
    int32_t line_num = 0;
    int32_t num_warn = 0;
    while (std::getline(is, line)) {
      ++line_num;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line[0] == '#') {
        continue;
      }

      // Each lexicon line is whitespace-delimited: the first field is the
      // written word and every remaining field is one phoneme token. For
      // example, the line
      //
      //   東京 t oː kʲ oː
      //
      // gives `word == "東京"` and token_list == {"t", "oː", "kʲ",
      // "oː"}. `operator>>` skips spaces and tabs, so the first extraction
      // distinguishes the word by position; the loop extracts the rest.
      // A literal phoneme-space cannot be represented by ordinary
      // whitespace, so generated lexicons write it as `<space>` and it is
      // converted back to " " below.
      std::istringstream iss(line);

      token_list.clear();
      word.clear();
      // extracts the first field as word
      iss >> word;
      if (word.empty()) {
        continue;
      }

      if (data->word2phonemes.count(word)) {
        num_warn += 1;
        if (num_warn < 10) {
          SHERPA_ONNX_LOGE("Duplicated word: %s at line %d:%s. Ignore it.",
                           word.c_str(), line_num, line.c_str());
        }
        continue;
      }

      // the while loop extracts the rest remaining whitespace-delimited field
      while (iss >> token) {
        if (token == "<space>") {
          token = " ";
        }
        token_list.push_back(std::move(token));
      }

      std::string phonemes;
      for (const auto &t : token_list) {
        phonemes += t;
      }

      if (phonemes.empty() && word != "呣") {
        SHERPA_ONNX_LOGE(
            "Invalid pronunciation for word '%s' at line %d:%s. Ignore it",
            word.c_str(), line_num, line.c_str());
        continue;
      }

      data->max_phrase_len = std::max<int32_t>(
          data->max_phrase_len,
          static_cast<int32_t>(Utf8ToUtf32(word).size()));
      data->word2phonemes.emplace(std::move(word), std::move(phonemes));
    }

    if (data->word2phonemes.empty()) {
      throw std::runtime_error("Kokoro lexicon '" + source.path +
                               "' contains no valid entries.");
    }
    for (const auto &[key, _] : data->word2phonemes) {
      data->all_words.insert(key);
    }
    return data;
  }

  std::shared_ptr<const LexiconData> GetLexiconData(
      const std::string &language) const {
    auto source_iter = lexicon_sources_.find(language);
    if (source_iter == lexicon_sources_.end()) {
      return {};
    }

    std::lock_guard<std::mutex> lock(lexicon_mutex_);
    auto loaded_iter = loaded_lexicons_.find(language);
    if (loaded_iter != loaded_lexicons_.end()) {
      return loaded_iter->second;
    }

    auto combined = std::make_shared<LexiconData>();
    for (const auto &source : source_iter->second) {
      std::vector<char> buffer = source.read();
      if (buffer.empty()) {
        throw std::runtime_error("Failed to read Kokoro lexicon '" +
                                 source.path + "'.");
      }

      auto parsed = ParseLexicon(source, buffer);

      for (auto &[key, phonemes] : parsed->word2phonemes) {
        if (!combined->word2phonemes.emplace(key, std::move(phonemes))
                 .second) {
          SHERPA_ONNX_LOGE(
              "Duplicated word '%s' in Kokoro language '%s'. Ignore it.",
              key.c_str(), language.c_str());
        }
      }
      combined->max_phrase_len =
          std::max(combined->max_phrase_len, parsed->max_phrase_len);
    }

    if (combined->word2phonemes.empty()) {
      throw std::runtime_error(
          "No usable Kokoro lexicon entries for language '" + language +
          "'.");
    }
    for (const auto &[key, _] : combined->word2phonemes) {
      combined->all_words.insert(key);
    }

    loaded_lexicons_[language] = combined;
    return combined;
  }

 private:
  OfflineTtsKokoroModelMetaData meta_data_;

  // Lightweight source descriptors keyed by language. Initialization records
  // only paths and deferred readers; it does not read or parse lexicon files.
  std::unordered_map<std::string, std::vector<LexiconSource>> lexicon_sources_;

  // Parsed lexicons cached by language after their first use. GetLexiconData()
  // populates this map under lexicon_mutex_, so lexicons for unused languages
  // remain unopened and consume no entry-map memory.
  mutable std::unordered_map<std::string,
                             std::shared_ptr<const LexiconData>>
      loaded_lexicons_;
  mutable std::mutex lexicon_mutex_;

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
