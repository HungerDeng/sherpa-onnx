# Introduction

This directory is for kokoro v1.0.

`run.sh` generates independent Chinese, US English, British English, and
Japanese lexicons. Japanese is exported with both Misaki backends:

- `lexicon-ja-cutlet.txt`
- `lexicon-ja-jtalk.txt`

To regenerate only one Japanese backend, pass `--backend cutlet` or
`--backend jtalk` to `generate_lexicon_ja.py`.

Install the project dependencies with `uv sync`. The first run downloads the
full UniDic dictionary if it is not already available.

At runtime, a Kokoro language is required. Set `kokoro.lang` in the model
configuration or set `GenerationConfig.extra["lang"]` for each request. The
per-request value takes precedence; model metadata is not used as a fallback.
Supported values are `en-us`, `en-gb`, `ja-cutlet`, `ja-jtalk`, `cmn`, `es`,
`fr`, `hi`, `it`, and `pt-br`. Both Japanese lexicons can be configured
together:

```text
lexicon-us-en.txt,lexicon-cmn.txt,lexicon-ja-cutlet.txt,lexicon-ja-jtalk.txt
```

For a fixed model language, set it on the Kokoro model configuration:

```python
config.model.kokoro.lang = "ja-cutlet"
```

To select it per request instead:

```python
generation_config = sherpa_onnx.GenerationConfig()
generation_config.extra["lang"] = "ja-jtalk"
audio = tts.generate(text, generation_config)
```

Lexicon language is selected from the standard generated basename, so
configured paths must retain those filenames. `ja-cutlet` and `ja-jtalk` are
independent language keys. Lexicons are loaded lazily: only the language
selected by a request is parsed and cached. Text outside the selected
language's script is handled by eSpeak-ng and never consults another language's
lexicon.
