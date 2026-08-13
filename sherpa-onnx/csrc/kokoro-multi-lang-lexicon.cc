// sherpa-onnx/csrc/kokoro-multi-lang-lexicon.cc
//
// Copyright (c)  2025  Xiaomi Corporation

#include "sherpa-onnx/csrc/kokoro-multi-lang-lexicon.h"

#include <fstream>
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

  std::vector<TokenIDs> ConvertTextToTokenIds(const std::string &_text,
                                              const std::string &voice) const {
    // we cannot convert text to lowercase here since it will affect
    // how piper_phonemize handles punctuations inside the text
    std::string text = _text;

    // Text normalization (step 3 of the frontend pipeline): map
    // full-width/Chinese punctuation to ASCII and collapse whitespace runs,
    // e.g. ，→, 、→, 。→. ？→? ！→!. The resulting punctuation characters are
    // non-Chinese, so they will be routed to ConvertNonChineseToTokenIDs()
    // below, never to the Chinese path.
    std::vector<std::pair<std::string, std::string>> replace_str_pairs = {
        {"，", ","}, {":", ","},  {"、", ","}, {"；", ";"},   {"：", ":"},
        {"。", "."}, {"？", "?"}, {"！", "!"}, {"\\s+", " "},
    };
    for (const auto &p : replace_str_pairs) {
      std::regex re(p.first);
      text = std::regex_replace(text, re, p.second);
    }

    if (debug_) {
      SHERPA_ONNX_LOGE("After replacing punctuations and merging spaces:\n%s",
                       text.c_str());
    }

    // https://en.cppreference.com/w/cpp/regex
    // https://stackoverflow.com/questions/37989081/how-to-use-unicode-range-in-c-regex
    // Run splitting: partition the normalized text into alternating Chinese
    // runs ([一-龥]+) and non-Chinese runs ([^一-龥]+).
    //
    // The each split text chunk is processed independently. For example,
    // text: 中國人民不信邪也不怕邪,不惹事!也不怕事...
    // split text chunk list becomes: ["中國人民不信邪也不怕邪", ",", "不惹事",
    // "!","也不怕事"] Chinese runs go to ConvertChineseToTokenIDs() (lexicon-zh
    // lookup via PhraseMatcher); all other runs (punctuation, English, digits)
    // go to ConvertNonChineseToTokenIDs().
    std::string expr_chinese = "([\\u4e00-\\u9fff]+)";
    std::string expr_not_chinese = "([^\\u4e00-\\u9fff]+)";

    std::string expr_both = expr_chinese + "|" + expr_not_chinese;

    auto ws = ToWideString(text);
    std::wstring wexpr_both = ToWideString(expr_both);
    std::wregex we_both(wexpr_both);

    std::wstring wexpr_zh = ToWideString(expr_chinese);
    std::wregex we_zh(wexpr_zh);

    auto begin = std::wsregex_iterator(ws.begin(), ws.end(), we_both);
    auto end = std::wsregex_iterator();

    std::vector<TokenIDs> ans;

    for (std::wsregex_iterator i = begin; i != end; ++i) {
      std::wsmatch match = *i;
      std::wstring match_str = match.str();

      auto ms = ToString(match_str);
      uint8_t c = reinterpret_cast<const uint8_t *>(ms.data())[0];

      std::vector<std::vector<int32_t>> ids_vec;
      // Route the run: Chinese runs go to ConvertChineseToTokenIDs()
      // (lexicon-zh lookup via PhraseMatcher); all other runs (punctuation,
      // English, digits) go to ConvertNonChineseToTokenIDs().
      if (std::regex_match(match_str, we_zh)) {
        if (debug_) {
          SHERPA_ONNX_LOGE("Chinese: %s", ms.c_str());
        }
        ids_vec = ConvertChineseToTokenIDs(ms);
      } else {
        if (debug_) {
          SHERPA_ONNX_LOGE("Non-Chinese: %s", ms.c_str());
        }

        ids_vec = ConvertNonChineseToTokenIDs(ms, voice);
      }

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
      for (const auto &ids : ids_vec) {
        if (ids.size() > 10 + 2) {
          ans.emplace_back(ids);
        } else {
          if (ans.empty()) {
            ans.emplace_back(ids);
          } else {
            if ((ans.back().tokens.size() + ids.size() < 50) ||
                (ids.size() < 5)) {
              ans.back().tokens.back() = ids[1];
              ans.back().tokens.insert(ans.back().tokens.end(), ids.begin() + 2,
                                       ids.end());
            } else {
              ans.emplace_back(ids);
            }
          }
        }
      }
    }

    if (debug_) {
      for (const auto &v : ans) {
        std::ostringstream os;
        os << "\n";
        std::string sep;
        for (auto i : v.tokens) {
          os << sep << i;
          sep = " ";
        }
        os << "\n";
        SHERPA_ONNX_LOGE("%s", os.str().c_str());
      }
    }

    return ans;
  }

 private:
  bool IsPunctuation(const std::string &text) const {
    if (text == ";" || text == ":" || text == "," || text == "." ||
        text == "!" || text == "?" || text == "—" || text == "…" ||
        text == "\"" || text == "(" || text == ")" || text == "“" ||
        text == "”") {
      return true;
    }

    return false;
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

  std::vector<std::vector<int32_t>> ConvertChineseToTokenIDs(
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

    std::vector<std::vector<int32_t>> ans;
    std::vector<int32_t> this_sentence;
    int32_t max_len = meta_data_.max_token_len;

    this_sentence.push_back(0);

    // Per-run bounded chunks: walk the whole Chinese run phrase by phrase.
    // Whenever adding the next phrase would push the sentence past
    // max_token_len (content budget is max_len - 2 after reserving BOS/EOS),
    // flush the current sentence (append EOS 0, push to ans, start a new
    // sentence with BOS 0). A long run therefore comes back as several
    // sub-sentences, each bounded by max_token_len.
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

      // The next phrase would exceed the max_token_len budget, so close
      // this sub-sentence and start a new one.
      if (this_sentence.size() + ids.size() > max_len - 2) {
        this_sentence.push_back(0);
        ans.push_back(std::move(this_sentence));

        this_sentence.push_back(0);
      }

      this_sentence.insert(this_sentence.end(), ids.begin(), ids.end());
    }  // for (const std::string &w : matcher)

    if (this_sentence.size() > 1) {
      this_sentence.push_back(0);
      ans.push_back(std::move(this_sentence));
    }

    if (debug_) {
      for (const auto &v : ans) {
        std::ostringstream os;
        os << "\n";
        std::string sep;
        for (auto i : v) {
          os << sep << i;
          sep = " ";
        }
        os << "\n";
        SHERPA_ONNX_LOGE("%s", os.str().c_str());
      }
    }

    return ans;
  }

  std::vector<std::vector<int32_t>> ConvertTextToTokenIDsWithEspeak(
      const std::string &text, const std::string &voice) const {
    auto temp = ConvertTextToTokenIdsKokoroOrKitten(
        phoneme2id_, meta_data_.max_token_len, text, voice);
    std::vector<std::vector<int32_t>> ans;
    ans.reserve(temp.size());

    for (const auto &i : temp) {
      ans.emplace_back(i.tokens.begin(), i.tokens.end());
    }

    return ans;
  }

  std::vector<std::vector<int32_t>> ConvertNonChineseToTokenIDs(
      const std::string &text, const std::string &voice) const {
    if (IsPunctuation(text)) {
      return {std::vector<int32_t>{0, token2id_.at(text), 0}};
    }

    // Long non-Chinese runs are also split into bounded chunks, but inside
    // the espeak-ng path: ConvertTextToTokenIDsWithEspeak() ends in
    // PiperPhonemesToIdsKokoroOrKitten(), which chunks the phoneme sequence
    // at max_token_len.
    if (!voice.empty()) {
      return ConvertTextToTokenIDsWithEspeak(text, voice);
    }

    // If voice is empty, we split the text into words and use the lexicon
    // to lookup the pronunciation of each word, fallback to espeak if
    // a word is not in the lexicon.

    std::vector<std::string> words = SplitUtf8(text);
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

    std::vector<std::vector<int32_t>> ans;
    // Per-run bounded chunks for the voice-empty path: each word is appended
    // to the current sentence; if it would exceed max_token_len (content
    // budget max_len - 2), the sentence is flushed (EOS 0 pushed to ans, a
    // new sentence starts with BOS 0). Punctuation ". ! ? ;" also ends a
    // sentence.
    int32_t max_len = meta_data_.max_token_len;
    std::vector<int32_t> this_sentence;

    int32_t space_id = token2id_.at(" ");

    this_sentence.push_back(0);

    for (const auto &_word : words) {
      auto word = ToLowerCase(_word);
      if (IsPunctuation(word)) {
        this_sentence.push_back(token2id_.at(word));

        if (this_sentence.size() > max_len - 2) {
          // this sentence is too long, split it
          this_sentence.push_back(0);
          ans.push_back(std::move(this_sentence));

          this_sentence.push_back(0);
          continue;
        }

        if (word == "." || word == "!" || word == "?" || word == ";") {
          // Note: You can add more punctuations here to split the text
          // into sentences. We just use four here: .!?;
          this_sentence.push_back(0);
          ans.push_back(std::move(this_sentence));

          this_sentence.push_back(0);
        }
      } else if (word2ids_.count(word)) {
        const auto &ids = word2ids_.at(word);
        if (this_sentence.size() + ids.size() + 3 > max_len - 2) {
          this_sentence.push_back(0);
          ans.push_back(std::move(this_sentence));

          this_sentence.push_back(0);
        }

        this_sentence.insert(this_sentence.end(), ids.begin(), ids.end());
        this_sentence.push_back(space_id);
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

        std::vector<int32_t> ids;
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

        if (this_sentence.size() + ids.size() + 3 > max_len - 2) {
          this_sentence.push_back(0);
          ans.push_back(std::move(this_sentence));

          this_sentence.push_back(0);
        }

        this_sentence.insert(this_sentence.end(), ids.begin(), ids.end());
        this_sentence.push_back(space_id);
      }
    }

    if (this_sentence.size() > 1) {
      this_sentence.push_back(0);
      ans.push_back(std::move(this_sentence));
    }

    if (debug_) {
      for (const auto &v : ans) {
        std::ostringstream os;
        os << "\n";
        std::string sep;
        for (auto i : v) {
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

    if (debug_) {
      for (const auto &p : token2id_) {
        id2token_[p.second] = p.first;
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
