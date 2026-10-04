# Introduction

This directory is for kokoro v1.0

`run.sh` exports `kokoro.onnx` with two outputs: `audio` and the per-token
`pred_dur` values used for span timestamps. It rebuilds an existing ONNX model
or quantized model when that output is missing. The script generates
`tokens.txt` and `voices.bin`; it does not generate lexicons or run G2P.

Run `uv sync` in this directory to install the export dependencies from
`pyproject.toml`. This is a script-only project, so it has no package entry
point or build backend. `test.py` and `generate_samples.py` also need Python
bindings built from this sherpa-onnx checkout; the published wheel does not
yet provide `generate_from_phonemes`.

The application obtains a `G2pOutput` from misaki-rs and forwards only its
aggregate `phonemes` and each `span.phonemes` to the SDK. The aggregate string
drives audio generation. Span strings identify the caller's segments for
alignment. For example:

```python
input = sherpa_onnx.PhonemeInput(
    phonemes="ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm. aɪ kˈænt lˈɪv wɪðˈaʊt ɪt.",
    spans=[
        sherpa_onnx.PhonemeSpan(phonemes=p)
        for p in (
            "ˌeɪˈaɪ", "ɪz", "sˌoʊ", "ˈɔːsʌm", ".", "aɪ",
            "kˈænt", "lˈɪv", "wɪðˈaʊt", "ɪt", ".",
        )
    ],
)
audio = tts.generate_from_phonemes(input, sid=20, speed=1.0)
for span in audio.span_alignments:
    print(span.original_phonemes, span.inferred_phonemes,
          span.start_ts, span.end_ts)
```

Kokoro returns one alignment per supplied span. `original_phonemes` retains
the supplied string; `inferred_phonemes` reconstructs the tokens sent to the
model. Timestamps are seconds in the returned waveform. A span with no
supported tokens has empty inferred phonemes and `-1` timestamps. Other
offline TTS implementations leave `span_alignments` unset.

`test.py` uses two real misaki-rs outputs, including a multiword span and
punctuation spans. It checks both exported models and a long input that
requires token rebatching.

`generate_voices_bin.py` generates the Sherpa-ONNX voice table from the
official `Kokoro-82M/voices` directory. The generated v1.0 table contains 54
voices, including the Spanish `em_santa` voice at speaker ID 53. Existing
speaker IDs 0 through 52 remain unchanged.

The generator validates the style embedding shape (`510 x 1 x 256`) and
`float32` dtype before writing the binary file. Run it from this directory
after downloading the Kokoro-82M repository:

```bash
python3 generate_voices_bin.py
```

The corresponding model metadata must be generated with `add_meta_data.py`,
which derives `n_speakers`, `speaker_names`, `id2speaker`, and `speaker2id`
from the same mapping.
