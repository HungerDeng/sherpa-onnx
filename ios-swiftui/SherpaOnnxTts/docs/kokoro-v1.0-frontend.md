# Kokoro v1.0 speech frontend stage

Scope: from `frontend_->ConvertTextToTokenIds(...)` inside `OfflineTtsKokoroImpl::Generate` to `OfflineTtsKokoroModel::Run` (the first ONNX inference call).

## Call chain and files

| Step | Where | File |
|---|---|---|
| Entry | `OfflineTtsKokoroImpl::Generate` calls `frontend_->ConvertTextToTokenIds(text, lang)` | `sherpa-onnx/csrc/offline-tts-kokoro-impl.h` |
| Frontend (v1.0, version ≥ 2) | `KokoroMultiLangLexicon::ConvertTextToTokenIds` | `sherpa-onnx/csrc/kokoro-multi-lang-lexicon.cc` |
| espeak-ng helpers | `CallPhonemizeEspeak`, `ConvertTextToTokenIdsKokoroOrKitten`, `PiperPhonemesToIdsKokoroOrKitten` | `sherpa-onnx/csrc/piper-phonemize-lexicon.cc` |
| Model entry | `OfflineTtsKokoroModel::Run(x, sid, speed)` → `sess_->Run(...)` | `sherpa-onnx/csrc/offline-tts-kokoro-model.cc` |

## Sequence diagram

```mermaid
sequenceDiagram
  autonumber
  box rgb(255,240,215) TTS core (C++)
    participant IMPL as OfflineTtsKokoroImpl (offline-tts-kokoro-impl.h)
    participant MODEL as OfflineTtsKokoroModel (offline-tts-kokoro-model.cc)
  end
  box rgb(224,240,220) Speech frontend (kokoro-multi-lang-lexicon.cc)
    participant FE as KokoroMultiLangLexicon (kokoro-multi-lang-lexicon.cc)
    participant NORM as Punctuation router
    participant ROUTE as Chinese / non-Chinese splitter
    participant ZH as Chinese path (ICU word break · lexicon-zh.txt)
    participant EN as Non-Chinese path (en / gb lexicon)
    participant ESP as espeak-ng G2P (piper-phonemize-lexicon.cc)
  end
  box rgb(255,228,222) Model (offline-tts-kokoro-model.cc)
    participant STY as Style lookup (voices.bin)
    participant SESS as ONNX session (sess_->Run)
  end

  IMPL->>IMPL: rule FST normalization (date-zh · phone-zh · number-zh) before frontend
  IMPL->>IMPL: resolve lang → extra → kokoro.lang → meta_data.voice
  IMPL->>FE: ConvertTextToTokenIds(text, lang)
  FE->>NORM: written text: 来听一听，这个是什么口音？How are you doing？Are you ok？Thank you！你觉得中英文说得如何呢？
  Note over NORM,ESP: example values are illustrative · simplified token IDs
  NORM->>ROUTE: partitioned, written text preserved: 来听一听，这个是什么口音？How are you doing？Are you ok？Thank you！你觉得中英文说得如何呢？
  ROUTE->>ZH: Chinese runs (一-龥): 来听一听 · 这个是什么口音 · 你觉得中英文说得如何呢
  ZH->>ZH: ICU BreakIterator(zh) · exact term lookup or per-character composition
  ZH->>FE: lexicon-zh lookup: 来听一听 → l ai2 t ing1 t ing1 · BOS/EOS = 0
  ROUTE->>EN: non-Chinese runs: How are you doing · Are you ok · Thank you
  alt voice / lang non-empty (default en-us)
    EN->>ESP: ConvertTextToTokenIDsWithEspeak
    ESP->>ESP: espeak-ng G2P: how are you doing → hˈaʊ ɑɹ jˈu dˈuɪŋ
    ESP->>EN: Kokoro phoneme → token IDs (tokens.txt) · max_token_len
  else voice empty
    EN->>EN: word split → word2ids_ lookup (lexicon-us-en.txt / lexicon-gb-en.txt)
    EN->>ESP: OOV → CallPhonemizeEspeak fallback
    ESP->>EN: token IDs
  end
  EN->>FE: token IDs: [0, 42, 18, ..., 0] · merge short segments · max_token_len split
  FE->>IMPL: TokenIDs per sentence: [0, 13, 5, ..., 0] (BOS/EOS = 0)
  IMPL->>IMPL: x = token IDs · batch_size = 1 per sentence
  IMPL->>MODEL: model_->Run(x_tensor, sid, speed)
  MODEL->>STY: len = seq - 2 · style slice by (sid, len)
  STY->>MODEL: style embedding (voices.bin · 256 dims)
  MODEL->>SESS: 3 inputs · tokens · style · speed
  SESS->>MODEL: out[0] = audio
```

## Step by step

1. **Entry** — `OfflineTtsKokoroImpl::Generate` resolves `lang` (`gen_config.extra["lang"]` → `config_.model.kokoro.lang` → `meta_data.voice`, which is `en-us` for the exported v1.0 model) and calls `frontend_->ConvertTextToTokenIds(text, lang)`.
2. **Context before the call — FST normalization** — if rule FSTs are configured (`date-zh.fst`, `phone-zh.fst`, `number-zh.fst`), `Generate` runs each of them in order via `kaldifst::TextNormalizer::Normalize` before the frontend. The normalizer converts the input string into a linear FST, composes it with the rule FST (`fst::Compose`), takes the best path (`fst::ShortestPath`), and writes back the output labels (dropping zero-padding bytes). The rule FSTs are string-rewriting transducers: `number-zh.fst` expands digits to their spoken Chinese reading (illustrative: `123` → `一百二十三`), `date-zh.fst` expands dates (illustrative: `2026年8月13日` → `二零二六年八月十三日`), and `phone-zh.fst` handles phone numbers. Anything a rule does not cover passes through unchanged — which is why the two example texts below, containing no digits, dates, or phone numbers, are unchanged by this step.
3. **Punctuation handling** — `ConvertTextToTokenIds` keeps each mapped full-width/CJK punctuation codepoint as an independent, language-neutral chunk and converts only its phoneme to a supported Kokoro symbol. Paired CJK marks map to directional quotes, `・` maps to space, and full-width `－` maps to `—`. General-purpose `-`, `–`, `—`, and `…` remain in non-Chinese text so eSpeak retains their surrounding context. A punctuation-only run naturally isolated between Chinese chunks, such as `……`, still maps directly to its two supported punctuation tokens. Chunking preserves all original whitespace exactly.
4. **Language routing** — the text is split into Chinese runs (`[一-龥]+`), non-Chinese runs, and language-neutral punctuation chunks.
5. **Chinese path** — `G2pChinese` converts each Chinese run to an ICU `UnicodeString` and uses a `zh` word `BreakIterator`. With ICU 78.3, `中國人民不信邪也不怕邪` becomes `"中國" → "人民" → "不信邪" → "也" → "不怕" → "邪"`; these written segments become the alignment terms. `ConvertWordToIds` first looks up the complete ICU term in `word2ids_` (populated from `lexicon-zh.txt`). If the complete term is absent, it splits only that term into UTF-8 characters and concatenates each character's pronunciation in order; it does not greedily match smaller multi-character phrases. Unmatched characters are skipped as OOV. If ICU cannot create its word iterator, this path logs the ICU error and falls back to the previous `SplitUtf8` plus `PhraseMatcher` behavior.
6. **Non-Chinese path** — `ConvertNonChineseToTokenIDs`:
   - voice non-empty (shipped default `en-us`) → `ConvertTextToTokenIDsWithEspeak`, which calls `ConvertTextToTokenIdsKokoroOrKitten` (espeak-ng phonemize → Kokoro phoneme → token IDs, chunked by `max_token_len`);
   - voice empty → word-split and look up `word2ids_` (`lexicon-us-en.txt` / `lexicon-gb-en.txt`), with per-word `CallPhonemizeEspeak` fallback for OOV.
7. **Sentence packing** — every segment gets BOS/EOS zeros (`0`), short segments are merged, and anything exceeding `max_token_len` is split.
8. **Back in `Generate`** — the `vector<TokenIDs>` becomes `x`; note `max_num_sentences != 1` is ignored for Kokoro, so **one sentence per batch**.
9. **Tensor prep** — `Process` flattens one sentence into `x_tensor` of shape `[1, seq]` and calls `model_->Run(x_tensor, sid, speed)`.
10. **`OfflineTtsKokoroModel::Run`** — reads `len = seq - 2` (the BOS/EOS zeros), slices the style embedding from `voices.bin` at `styles_[sid * dim0 * dim1 + len * dim1]` (shape `[1, 256]`), applies `speed` (falling back to `length_scale`), feeds the three inputs (`tokens`, `style`, `speed`), and `sess_->Run(...)` returns `out[0]` as the audio tensor.

## Data inputs used by this stage

| Data | Used for |
|---|---|
| ICU 78+ word-break rules and CJK dictionary | Chinese term segmentation; cross-platform builds provide the target ICU installation through `ICU_ROOT` |
| `lexicon-zh.txt` | Chinese phrase/character → phoneme lookup (`word2ids_`) |
| `lexicon-us-en.txt`, `lexicon-gb-en.txt` | English word → phoneme lookup, only when voice is empty |
| `espeak-ng-data` | espeak-ng G2P (non-Chinese default, and OOV fallback) |
| `tokens.txt` | phoneme/symbol → numeric token ID |
| `voices.bin` | style embedding slice inside `OfflineTtsKokoroModel::Run` |

## Worked examples (CN / EN) after each step

All phoneme and token-ID values below are illustrative (simplified); the real outputs come from `lexicon-zh.txt`, espeak-ng, and `tokens.txt`.

| Step | CN example | EN example |
|---|---|---|
| input (written text) | 中國人民不信邪也不怕邪，不惹事也不怕事，任何外國不要指望我們會拿自己的核心利益做交易，不要指望我們會吞下損害我國主權、安全、發展利益的苦果！ | The sky above the port was the color of television, tuned to a dead channel. |
| 2 · FST normalization | unchanged (no digits / dates / phone numbers) | unchanged |
| 3 · punctuation handling | 中國人民不信邪也不怕邪，不惹事也不怕事，任何外國不要指望我們會拿自己的核心利益做交易，不要指望我們會吞下損害我國主權、安全、發展利益的苦果！ | unchanged (punctuation and whitespace retained) |
| 4 · language routing | Chinese runs: 中國人民不信邪也不怕邪 · 不惹事也不怕事 · 任何外國不要指望我們會拿自己的核心利益做交易 · 不要指望我們會吞下損害我國主權 · 安全 · 發展利益的苦果 (punctuation uses the language-neutral path) | one non-Chinese run: The sky above the port was the color of television, tuned to a dead channel. |
| 5 · Chinese path (ICU + lexicon-zh lookup) | run 中國人民不信邪也不怕邪 → ICU 78.3: 中國 · 人民 · 不信邪 · 也 · 不怕 · 邪 → exact term lookup when present; otherwise concatenate character pronunciations | — |
| 6 · non-Chinese path (espeak-ng G2P) | — | the sky above the port was the color of television, tuned to a dead channel → ðə skˈaɪ əbˈʌv ðə pˈɔɹt wˈʌz ðə kˈʌlɚ əv tˈɛləvɪʒən, tjˈund tə ə dˈɛd tʃˈænəl |
| 7 · numeric token IDs | per sentence, BOS/EOS = 0 · e.g. `[0, 13, 5, …, 0]` | per sentence, BOS/EOS = 0 · e.g. `[0, 42, 18, …, 0]` |

## Chunk boundaries: can a word or phrase be split across sentences?

Whether a single word's (or Chinese phrase's) token IDs can straddle two sub-sentences depends on the path:

| Path | Can a word/phrase be split? | Why |
|---|---|---|
| Chinese ICU term (`G2pChinese`) | Only if the term itself exceeds the model content budget | `PackTerms` keeps a normal term atomic, but fragments an oversized pronunciation across sentences after ICU segmentation and lexicon resolution. |
| Non-Chinese word, voice-empty lexicon path (`ConvertNonChineseToTokenIDs`) | No | Same atomic pattern: `this_sentence.size() + ids.size() + 3 > max_len - 2` flushes first, then the whole word's IDs are inserted. The per-word espeak OOV branch builds the word's IDs first, then applies the same whole-vector check. |
| Non-Chinese run, espeak-ng path (default `en-us`) | **Yes, possible** | `PiperPhonemesToIdsKokoroOrKitten` chunks the flat phoneme stream **per phoneme** at `max_token_len`, with no word-boundary awareness: `if (current.size() > max_len - 1)` flushes before each phoneme, so the boundary can land inside a word — its leading phonemes end one chunk (with BOS/EOS `0`s) and its trailing phonemes start the next. |

Implications:

- In the Kokoro impl each `TokenIDs` entry is processed as its own batch (batch size 1), so an espeak-path word split across chunks is synthesized across two ONNX inferences.
- The outer merge loop in `ConvertTextToTokenIds` cannot repair this: it only stitches *whole* chunks into sentences (or pushes them as-is); it never re-splits or moves individual tokens.
- This matters if you plan to expose per-token alignment data (e.g., `pred_dur` per sentence): durations will correspond to chunk/sentence units, not necessarily to whole words, for espeak-path input.
